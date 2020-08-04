// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::api::{ConfigMapper, ReadConfig, WriteConfig},
    crate::runtime::populate_runtime_config,
    anyhow::{anyhow, bail, Result},
    config_macros::include_default,
    ffx_config_plugin_args::ConfigLevel,
    serde_json::{Map, Value},
    std::fmt,
};

pub(crate) struct Priority {
    default: Option<Value>,
    pub(crate) build: Option<Value>,
    pub(crate) global: Option<Value>,
    pub(crate) user: Option<Value>,
    pub(crate) runtime: Option<Value>,
}

struct PriorityIterator<'a> {
    curr: Option<ConfigLevel>,
    config: &'a Priority,
}

impl<'a> Iterator for PriorityIterator<'a> {
    type Item = &'a Option<Value>;

    fn next(&mut self) -> Option<Self::Item> {
        match &self.curr {
            None => {
                self.curr = Some(ConfigLevel::Runtime);
                Some(&self.config.runtime)
            }
            Some(level) => match level {
                ConfigLevel::Runtime => {
                    self.curr = Some(ConfigLevel::User);
                    Some(&self.config.user)
                }
                ConfigLevel::User => {
                    self.curr = Some(ConfigLevel::Build);
                    Some(&self.config.build)
                }
                ConfigLevel::Build => {
                    self.curr = Some(ConfigLevel::Global);
                    Some(&self.config.global)
                }
                ConfigLevel::Global => {
                    self.curr = Some(ConfigLevel::Default);
                    Some(&self.config.default)
                }
                ConfigLevel::Default => None,
            },
        }
    }
}

impl Priority {
    pub(crate) fn new(
        user: Option<Value>,
        build: Option<Value>,
        global: Option<Value>,
        runtime: &Option<String>,
    ) -> Self {
        Self {
            user,
            build,
            global,
            runtime: populate_runtime_config(runtime),
            default: include_default!(),
        }
    }

    fn iter(&self) -> PriorityIterator<'_> {
        PriorityIterator { curr: None, config: self }
    }

    pub fn nested_map(cur: Option<Value>, mapper: ConfigMapper) -> Option<Value> {
        cur.and_then(|c| {
            if let Value::Object(map) = c {
                let mut result = Map::new();
                for (key, value) in map.iter() {
                    let new_value = if value.is_object() {
                        Priority::nested_map(map.get(key).cloned(), mapper)
                    } else {
                        map.get(key).cloned().and_then(|v| mapper(v))
                    };
                    if let Some(new_value) = new_value {
                        result.insert(key.to_string(), new_value);
                    }
                }
                if result.len() == 0 {
                    None
                } else {
                    Some(Value::Object(result))
                }
            } else {
                mapper(c)
            }
        })
    }

    fn nested_get(
        cur: &Option<Value>,
        key: &str,
        remaining_keys: Vec<&str>,
        mapper: ConfigMapper,
    ) -> Option<Value> {
        cur.as_ref().and_then(|c| {
            if remaining_keys.len() == 0 {
                Priority::nested_map(c.get(key).cloned(), mapper)
            } else {
                Priority::nested_get(
                    &c.get(key).cloned(),
                    remaining_keys[0],
                    remaining_keys[1..].to_vec(),
                    mapper,
                )
            }
        })
    }
}

impl ReadConfig for Priority {
    fn get(&self, key: &str, mapper: ConfigMapper) -> Option<Value> {
        // Check for nested config values if there's a '.' in the key
        let mut key_vec: Vec<&str> = key.split('.').collect();
        let first_key = if key_vec.len() > 0 { key_vec.remove(0) } else { key };
        self.iter().find_map(|c| Priority::nested_get(c, first_key, key_vec[..].to_vec(), mapper))
    }
}

impl fmt::Display for Priority {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(
            f,
            "FFX configuration can come from several places and has an inherent priority assigned\n\
            to the different ways the configuration is gathered. A configuration key can be set\n\
            in multiple locations but the first value found is returned. The following output\n\
            shows the locations checked in descending priority order.\n"
        )?;
        let mut iterator = self.iter();
        while let Some(next) = iterator.next() {
            if let Some(level) = iterator.curr {
                match level {
                    ConfigLevel::Runtime => {
                        write!(f, "Runtime Configuration")?;
                    }
                    ConfigLevel::User => {
                        write!(f, "User Configuration")?;
                    }
                    ConfigLevel::Build => {
                        write!(f, "Build Configuration")?;
                    }
                    ConfigLevel::Global => {
                        write!(f, "Global Configuration")?;
                    }
                    ConfigLevel::Default => {
                        write!(f, "Default Configuration")?;
                    }
                };
            }
            if let Some(value) = next {
                writeln!(f, "")?;
                writeln!(f, "{}", serde_json::to_string_pretty(&value).unwrap())?;
            } else {
                writeln!(f, ": {}", "none")?;
            }
            writeln!(f, "")?;
        }
        Ok(())
    }
}

impl WriteConfig for Priority {
    fn set(&mut self, level: &ConfigLevel, key: &str, value: Value) -> Result<()> {
        let inner_set = |config_data: &mut Option<Value>| match config_data {
            Some(config) => match config.as_object_mut() {
                Some(map) => match map.get_mut(key) {
                    Some(v) => {
                        *v = value;
                        Ok(())
                    }
                    None => {
                        map.insert(key.to_string(), value);
                        Ok(())
                    }
                },
                _ => bail!("Configuration already exists but it is is a JSON literal - not a map"),
            },
            None => {
                let mut config = serde_json::Map::new();
                config.insert(key.to_string(), value);
                *config_data = Some(Value::Object(config));
                Ok(())
            }
        };
        match level {
            ConfigLevel::Runtime => inner_set(&mut self.runtime),
            ConfigLevel::User => inner_set(&mut self.user),
            ConfigLevel::Build => inner_set(&mut self.build),
            ConfigLevel::Global => inner_set(&mut self.global),
            ConfigLevel::Default => inner_set(&mut self.default),
        }
    }

    fn remove(&mut self, level: &ConfigLevel, key: &str) -> Result<()> {
        let inner_remove = |config_data: &mut Option<Value>| -> Result<()> {
            match config_data {
                Some(config) => match config.as_object_mut() {
                    Some(map) => map
                        .remove(&key.to_string())
                        .ok_or_else(|| anyhow!("No config found matching {}", key))
                        .map(|_| ()),
                    None => Err(anyhow!("Config file parsing error")),
                },
                None => Ok(()),
            }
        };
        match level {
            ConfigLevel::User => inner_remove(&mut self.user),
            ConfigLevel::Build => inner_remove(&mut self.build),
            ConfigLevel::Global => inner_remove(&mut self.global),
            ConfigLevel::Default => inner_remove(&mut self.default),
            ConfigLevel::Runtime => inner_remove(&mut self.runtime),
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use crate::identity::identity;
    use regex::Regex;

    const ERROR: &'static str = "0";

    const USER: &'static str = r#"
        {
            "name": "User"
        }"#;

    const BUILD: &'static str = r#"
        {
            "name": "Build"
        }"#;

    const GLOBAL: &'static str = r#"
        {
            "name": "Global"
        }"#;

    const DEFAULT: &'static str = r#"
        {
            "name": "Default"
        }"#;

    const RUNTIME: &'static str = r#"
        {
            "name": "Runtime"
        }"#;

    const MAPPED: &'static str = r#"
        {
            "name": "TEST_MAP"
        }"#;

    const NESTED: &'static str = r#"
        {
            "name": {
               "nested": "Nested"
            }
        }"#;

    const DEEP: &'static str = r#"
        {
            "name": {
               "nested": {
                    "deep": {
                        "name": "TEST_MAP"
                    }
               }
            }
        }"#;

    #[test]
    fn test_priority_iterator() -> Result<()> {
        let test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: Some(serde_json::from_str(RUNTIME)?),
        };

        let mut test_iter = test.iter();
        assert_eq!(test_iter.next(), Some(&test.runtime));
        assert_eq!(test_iter.next(), Some(&test.user));
        assert_eq!(test_iter.next(), Some(&test.build));
        assert_eq!(test_iter.next(), Some(&test.global));
        assert_eq!(test_iter.next(), Some(&test.default));
        Ok(())
    }

    #[test]
    fn test_priority_iterator_with_nones() -> Result<()> {
        let test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: None,
            global: None,
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };

        let mut test_iter = test.iter();
        assert_eq!(test_iter.next(), Some(&test.runtime));
        assert_eq!(test_iter.next(), Some(&test.user));
        assert_eq!(test_iter.next(), Some(&test.build));
        assert_eq!(test_iter.next(), Some(&test.global));
        assert_eq!(test_iter.next(), Some(&test.default));
        Ok(())
    }

    #[test]
    fn test_get() -> Result<()> {
        let test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };

        let value = test.get("name", identity);
        assert!(value.is_some());
        assert_eq!(value.unwrap(), Value::String(String::from("User")));

        let test_build = Priority {
            user: None,
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };

        let value_build = test_build.get("name", identity);
        assert!(value_build.is_some());
        assert_eq!(value_build.unwrap(), Value::String(String::from("Build")));

        let test_global = Priority {
            user: None,
            build: None,
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };

        let value_global = test_global.get("name", identity);
        assert!(value_global.is_some());
        assert_eq!(value_global.unwrap(), Value::String(String::from("Global")));

        let test_default = Priority {
            user: None,
            build: None,
            global: None,
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };

        let value_default = test_default.get("name", identity);
        assert!(value_default.is_some());
        assert_eq!(value_default.unwrap(), Value::String(String::from("Default")));

        let test_none =
            Priority { user: None, build: None, global: None, default: None, runtime: None };

        let value_none = test_none.get("name", identity);
        assert!(value_none.is_none());
        Ok(())
    }

    #[test]
    fn test_set_non_map_value() -> Result<()> {
        let mut test = Priority {
            user: Some(serde_json::from_str(ERROR)?),
            build: None,
            global: None,
            default: None,
            runtime: None,
        };
        let value = test.set(&ConfigLevel::User, "name", Value::String(String::from("whatever")));
        assert!(value.is_err());
        Ok(())
    }

    #[test]
    fn test_get_nonexistent_config() -> Result<()> {
        let test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };
        let value = test.get("field that does not exist", identity);
        assert!(value.is_none());
        Ok(())
    }

    #[test]
    fn test_set() -> Result<()> {
        let mut test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };
        test.set(&ConfigLevel::User, "name", Value::String(String::from("user-test")))?;
        let value = test.get("name", identity);
        assert!(value.is_some());
        assert_eq!(value.unwrap(), Value::String(String::from("user-test")));
        Ok(())
    }

    #[test]
    fn test_set_build_from_none() -> Result<()> {
        let mut test =
            Priority { user: None, build: None, global: None, default: None, runtime: None };
        let value_none = test.get("name", identity);
        assert!(value_none.is_none());
        test.set(&ConfigLevel::Default, "name", Value::String(String::from("default")))?;
        let value_default = test.get("name", identity);
        assert!(value_default.is_some());
        assert_eq!(value_default.unwrap(), Value::String(String::from("default")));
        test.set(&ConfigLevel::Global, "name", Value::String(String::from("global")))?;
        let value_global = test.get("name", identity);
        assert!(value_global.is_some());
        assert_eq!(value_global.unwrap(), Value::String(String::from("global")));
        test.set(&ConfigLevel::Build, "name", Value::String(String::from("build")))?;
        let value_build = test.get("name", identity);
        assert!(value_build.is_some());
        assert_eq!(value_build.unwrap(), Value::String(String::from("build")));
        test.set(&ConfigLevel::User, "name", Value::String(String::from("user")))?;
        let value_user = test.get("name", identity);
        assert!(value_user.is_some());
        assert_eq!(value_user.unwrap(), Value::String(String::from("user")));
        Ok(())
    }

    #[test]
    fn test_remove() -> Result<()> {
        let mut test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };
        test.remove(&ConfigLevel::User, "name")?;
        let user_value = test.get("name", identity);
        assert!(user_value.is_some());
        assert_eq!(user_value.unwrap(), Value::String(String::from("Build")));
        test.remove(&ConfigLevel::Build, "name")?;
        let global_value = test.get("name", identity);
        assert!(global_value.is_some());
        assert_eq!(global_value.unwrap(), Value::String(String::from("Global")));
        test.remove(&ConfigLevel::Global, "name")?;
        let default_value = test.get("name", identity);
        assert!(default_value.is_some());
        assert_eq!(default_value.unwrap(), Value::String(String::from("Default")));
        test.remove(&ConfigLevel::Default, "name")?;
        let none_value = test.get("name", identity);
        assert!(none_value.is_none());
        Ok(())
    }

    #[test]
    fn test_default() {
        let test = Priority::new(None, None, None, &None);
        let default_value = test.get("log.enabled", identity);
        assert_eq!(default_value.unwrap(), Value::String("$FFX_LOG_ENABLED".to_string()));
    }

    #[test]
    fn test_display() -> Result<()> {
        let test = Priority {
            user: Some(serde_json::from_str(USER)?),
            build: Some(serde_json::from_str(BUILD)?),
            global: Some(serde_json::from_str(GLOBAL)?),
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: None,
        };
        let output = format!("{}", test);
        assert!(output.len() > 0);
        let user_reg = Regex::new("\"name\": \"User\"").expect("test regex");
        assert_eq!(1, user_reg.find_iter(&output).count());
        let build_reg = Regex::new("\"name\": \"Build\"").expect("test regex");
        assert_eq!(1, build_reg.find_iter(&output).count());
        let global_reg = Regex::new("\"name\": \"Global\"").expect("test regex");
        assert_eq!(1, global_reg.find_iter(&output).count());
        let default_reg = Regex::new("\"name\": \"Default\"").expect("test regex");
        assert_eq!(1, default_reg.find_iter(&output).count());
        Ok(())
    }

    fn test_map(value: Value) -> Option<Value> {
        if value == "TEST_MAP".to_string() {
            Some(Value::String("passed".to_string()))
        } else {
            Some(Value::String("failed".to_string()))
        }
    }

    #[test]
    fn test_mapping() -> Result<()> {
        let test = Priority {
            user: Some(serde_json::from_str(MAPPED)?),
            build: None,
            global: None,
            default: None,
            runtime: None,
        };
        let test_mapping = "TEST_MAP".to_string();
        let test_passed = "passed".to_string();
        let mapped_value = test.get("name", test_map);
        assert_eq!(mapped_value, Some(Value::String(test_passed)));
        let identity_value = test.get("name", identity);
        assert_eq!(identity_value, Some(Value::String(test_mapping)));
        Ok(())
    }

    #[test]
    fn test_nested() -> Result<()> {
        let test = Priority {
            user: None,
            build: None,
            global: None,
            default: None,
            runtime: Some(serde_json::from_str(NESTED)?),
        };
        let value = test.get("name.nested", identity);
        assert_eq!(value, Some(Value::String("Nested".to_string())));
        Ok(())
    }

    #[test]
    fn test_nested_should_return_sub_tree() -> Result<()> {
        let test = Priority {
            user: None,
            build: None,
            global: None,
            default: Some(serde_json::from_str(DEFAULT)?),
            runtime: Some(serde_json::from_str(NESTED)?),
        };
        let value = test.get("name", identity);
        assert_eq!(value, Some(serde_json::from_str("{\"nested\": \"Nested\"}")?));
        Ok(())
    }

    #[test]
    fn test_nested_should_return_full_match() -> Result<()> {
        let test = Priority {
            user: None,
            build: None,
            global: None,
            default: Some(serde_json::from_str(NESTED)?),
            runtime: Some(serde_json::from_str(RUNTIME)?),
        };
        let value = test.get("name.nested", identity);
        assert_eq!(value, Some(Value::String("Nested".to_string())));
        Ok(())
    }

    #[test]
    fn test_nested_should_map_values_in_sub_tree() -> Result<()> {
        let test = Priority {
            user: None,
            build: None,
            global: None,
            default: Some(serde_json::from_str(NESTED)?),
            runtime: Some(serde_json::from_str(DEEP)?),
        };
        let value = test.get("name.nested", test_map);
        assert_eq!(value, Some(serde_json::from_str("{\"deep\": {\"name\": \"passed\"}}")?));
        Ok(())
    }
}
