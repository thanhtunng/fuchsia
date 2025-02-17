// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command};

#[ffx_command()]
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "package", description = "List the packages inside a repository")]
pub struct PackagesCommand {
    #[argh(subcommand)]
    pub subcommand: PackagesSubcommand,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand)]
pub enum PackagesSubcommand {
    List(ListSubcommand),
    Show(ShowSubCommand),
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "list", description = "Inspect and manage package repositories")]
pub struct ListSubcommand {
    #[argh(option, short = 'r')]
    /// list packages from this repository.
    pub repository: Option<String>,

    /// if true, package hashes will be displayed in full (i.e. not truncated).
    #[argh(switch)]
    pub full_hash: bool,

    /// toggle whether components in each package will be fetched and shown in the output table
    #[argh(option, default = "true")]
    pub include_components: bool,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "show", description = "Inspect content of a package")]
pub struct ShowSubCommand {
    #[argh(option, short = 'r')]
    /// look from this repository.
    pub repository_name: Option<String>,

    #[argh(option, short = 'p')]
    /// look up this package name from repository.
    pub package_name: Option<String>,

    /// if true, package hashes will be displayed in full (i.e. not truncated).
    #[argh(switch)]
    pub full_hash: bool,
}
