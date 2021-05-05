// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    ffx_core::ffx_plugin,
    ffx_scrutiny_package_list_args::ScrutinyPackageCommand,
    scrutiny_frontend::{
        config::{Config, LaunchConfig, RuntimeConfig},
        launcher,
    },
};

#[ffx_plugin()]
pub async fn scrutiny_package(cmd: ScrutinyPackageCommand) -> Result<(), Error> {
    let config = Config {
        launch: LaunchConfig {
            command: Some(format!("search.package.list --url {}", cmd.url).to_string()),
            script_path: None,
        },
        runtime: RuntimeConfig::minimal(),
    };
    launcher::launch_from_config(config)?;

    Ok(())
}
