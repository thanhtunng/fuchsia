// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context, Result};
use assembly_util::PathToStringExt;
use std::path::PathBuf;

/// A builder that receives a sparse FVM, and prepares it for nand flashing.
///
/// ```
/// let builder = NandFvmBuilder {
///     output: PathBuf::from("path/to/output.blk"),
///     sparse_blob_fvm: PathBuf::from("path/to/fvm.blob.sparse.blk"),
///     max_disk_size: None,
///     compression: None,
///     page_size: 0,
///     oob_size: 0,
///     pages_per_block: 0,
///     block_count: 0,
/// };
/// builder.build();
/// ```

pub struct NandFvmBuilder {
    /// Path to the fvm host tool.
    pub tool: PathBuf,
    /// The path to write the FVM to.
    pub output: PathBuf,
    /// The path to the sparse, blob-only FVM on the host.
    pub sparse_blob_fvm: PathBuf,
    /// The maximum disk size for the sparse FVM.
    /// The build will fail if the sparse FVM is larger than this.
    pub max_disk_size: Option<u64>,
    /// The compression algorithm to use.
    pub compression: Option<String>,
    /// The nand page size.
    pub page_size: u64,
    /// The out of bound size.
    pub oob_size: u64,
    /// The pages per block.
    pub pages_per_block: u64,
    /// The number of blocks.
    pub block_count: u64,
}

impl NandFvmBuilder {
    /// Build the FVM.
    pub fn build(self) -> Result<()> {
        let args = self.build_args()?;
        let output = std::process::Command::new(&self.tool).args(&args).output();
        let output = output.context("Failed to run the fvm tool")?;
        if !output.status.success() {
            anyhow::bail!(format!(
                "Failed to generate fvm with status: {}\n{}",
                output.status,
                String::from_utf8_lossy(output.stderr.as_slice())
            ));
        }

        Ok(())
    }

    fn build_args(&self) -> Result<Vec<String>> {
        let mut args: Vec<String> = Vec::new();
        args.push(self.output.path_to_string()?);
        args.push("ftl-raw-nand".to_string());

        // Append key and value to the `args` if the value is present.
        fn maybe_append_value(
            args: &mut Vec<String>,
            key: impl AsRef<str>,
            value: Option<impl std::string::ToString>,
        ) {
            if let Some(value) = value {
                args.push(format!("--{}", key.as_ref()));
                args.push(value.to_string());
            }
        }

        maybe_append_value(&mut args, "nand-page-size", Some(self.page_size));
        maybe_append_value(&mut args, "nand-oob-size", Some(self.oob_size));
        maybe_append_value(&mut args, "nand-pages-per-block", Some(self.pages_per_block));
        maybe_append_value(&mut args, "nand-block-count", Some(self.block_count));
        maybe_append_value(&mut args, "sparse", Some(self.sparse_blob_fvm.path_to_string()?));
        maybe_append_value(&mut args, "max-disk-size", self.max_disk_size);
        maybe_append_value(&mut args, "compress", self.compression.as_ref());

        Ok(args)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nand_args() {
        let builder = NandFvmBuilder {
            tool: "fvm".into(),
            output: "mypath".into(),
            sparse_blob_fvm: "sparsepath".into(),
            max_disk_size: Some(500),
            compression: Some("supercompress".into()),
            page_size: 1,
            oob_size: 2,
            pages_per_block: 3,
            block_count: 4,
        };
        let args = builder.build_args().unwrap();
        assert_eq!(
            args,
            [
                "mypath",
                "ftl-raw-nand",
                "--nand-page-size",
                "1",
                "--nand-oob-size",
                "2",
                "--nand-pages-per-block",
                "3",
                "--nand-block-count",
                "4",
                "--sparse",
                "sparsepath",
                "--max-disk-size",
                "500",
                "--compress",
                "supercompress"
            ]
        );
    }
}
