// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module provides common constants and helpers to assist in the unit-testing of other
//! modules within the crate.

use account_common::{LocalAccountId, LocalPersonaId};
use fidl_fuchsia_auth::AppConfig;
use lazy_static::lazy_static;
use std::path::PathBuf;
use tempfile::TempDir;

lazy_static! {
    pub static ref TEST_ACCOUNT_ID: LocalAccountId = LocalAccountId::new(111111);
    pub static ref TEST_PERSONA_ID: LocalPersonaId = LocalPersonaId::new(222222);
}

pub static TEST_APPLICATION_URL: &str = "test_app_url";

// TODO(jsankey): If fidl calls ever accept non-mutable structs, move this to a lazy_static.
// Currently FIDL requires mutable access to a type that doesn't support clone, so we just create a
// fresh instance each time.
pub fn create_dummy_app_config() -> AppConfig {
    AppConfig {
        auth_provider_type: "dummy_auth_provider".to_string(),
        client_id: None,
        client_secret: None,
        redirect_uri: None,
    }
}

pub struct TempLocation {
    /// A fresh temp directory that will be deleted when this object is dropped.
    _dir: TempDir,
    /// A path within the temp dir to use for writing the db.
    pub path: PathBuf,
}

impl TempLocation {
    /// Return a writable, temporary location and optionally create it as a directory.
    pub fn new() -> TempLocation {
        let dir = TempDir::new().unwrap();
        let path = dir.path().to_path_buf();
        TempLocation { _dir: dir, path }
    }

    /// Returns a path to a test file inside the temporary location. The file name is static.
    pub fn test_file(&self) -> PathBuf {
        self.path.join("testfile")
    }
}
