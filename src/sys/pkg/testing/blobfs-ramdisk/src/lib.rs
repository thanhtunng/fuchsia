// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(missing_docs)]

//! Test utilities for starting a blobfs server.

use {
    anyhow::{format_err, Context as _, Error},
    fdio::{SpawnAction, SpawnOptions},
    fidl::endpoints::{ClientEnd, ServerEnd},
    fidl_fuchsia_io::{
        DirectoryAdminMarker, DirectoryAdminProxy, DirectoryMarker, DirectoryProxy, NodeProxy,
    },
    fuchsia_component::server::ServiceFs,
    fuchsia_merkle::{Hash, MerkleTreeBuilder},
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon::{self as zx, prelude::*},
    futures::prelude::*,
    ramdevice_client::RamdiskClient,
    scoped_task::Scoped,
    std::{borrow::Cow, collections::BTreeSet, ffi::CString},
};

const RAMDISK_BLOCK_SIZE: u64 = 512;

#[cfg(test)]
mod test;

/// A blob's hash, length, and contents.
#[derive(Debug, Clone)]
pub struct BlobInfo {
    merkle: Hash,
    contents: Cow<'static, [u8]>,
}

impl<B> From<B> for BlobInfo
where
    B: Into<Cow<'static, [u8]>>,
{
    fn from(bytes: B) -> Self {
        let bytes = bytes.into();
        let mut tree = MerkleTreeBuilder::new();
        tree.write(&bytes);
        Self { merkle: tree.finish().root(), contents: bytes }
    }
}

/// A helper to construct [`BlobfsRamdisk`] instances.
pub struct BlobfsRamdiskBuilder {
    ramdisk: Option<FormattedRamdisk>,
    blobs: Vec<BlobInfo>,
}

impl BlobfsRamdiskBuilder {
    fn new() -> Self {
        Self { ramdisk: None, blobs: vec![] }
    }

    /// Configures this blobfs to use the already formatted given backing ramdisk.
    pub fn ramdisk(mut self, ramdisk: FormattedRamdisk) -> Self {
        self.ramdisk = Some(ramdisk);
        self
    }

    /// Write the provided blob after mounting blobfs if the blob does not already exist.
    pub fn with_blob(mut self, blob: impl Into<BlobInfo>) -> Self {
        self.blobs.push(blob.into());
        self
    }

    /// Starts a blobfs server with the current configuration options.
    pub fn start(self) -> Result<BlobfsRamdisk, Error> {
        // Use the provided ramdisk or format a fresh one with blobfs.
        let ramdisk = if let Some(ramdisk) = self.ramdisk {
            ramdisk
        } else {
            Ramdisk::start().context("creating backing ramdisk for blobfs")?.format()?
        };

        // Spawn blobfs on top of the ramdisk.
        let block_device_handle_id = HandleInfo::new(HandleType::User0, 1);
        let fs_root_handle_id = HandleInfo::new(HandleType::User0, 0);

        let block_handle = ramdisk.clone_channel().context("cloning ramdisk channel")?;

        let (proxy, blobfs_server_end) = fidl::endpoints::create_proxy::<DirectoryAdminMarker>()?;
        let process = scoped_task::spawn_etc(
            scoped_task::job_default(),
            SpawnOptions::CLONE_ALL,
            &CString::new("/pkg/bin/blobfs").unwrap(),
            &[&CString::new("blobfs").unwrap(), &CString::new("mount").unwrap()],
            None,
            &mut [
                SpawnAction::add_handle(block_device_handle_id, block_handle.into()),
                SpawnAction::add_handle(fs_root_handle_id, blobfs_server_end.into()),
            ],
        )
        .map_err(|(status, _)| status)
        .context("spawning 'blobfs mount'")?;

        let blobfs = BlobfsRamdisk { backing_ramdisk: ramdisk, process, proxy };

        // Write all the requested missing blobs to the mounted filesystem.
        if !self.blobs.is_empty() {
            let mut present_blobs = blobfs.list_blobs()?;

            for blob in self.blobs {
                if present_blobs.contains(&blob.merkle) {
                    continue;
                }
                blobfs
                    .write_blob_sync(&blob.merkle, &blob.contents)
                    .context(format!("writing {}", blob.merkle))?;
                present_blobs.insert(blob.merkle);
            }
        }

        Ok(blobfs)
    }
}

/// A ramdisk-backed blobfs instance
pub struct BlobfsRamdisk {
    backing_ramdisk: FormattedRamdisk,
    process: Scoped<fuchsia_zircon::Process>,
    proxy: DirectoryAdminProxy,
}

impl BlobfsRamdisk {
    /// Creates a new [`BlobfsRamdiskBuilder`] with no pre-configured ramdisk.
    pub fn builder() -> BlobfsRamdiskBuilder {
        BlobfsRamdiskBuilder::new()
    }

    /// Starts a blobfs server backed by a freshly formatted ramdisk.
    pub fn start() -> Result<Self, Error> {
        Self::builder().start()
    }

    /// Returns a new connection to blobfs using the blobfs::Client wrapper type.
    ///
    /// # Panics
    ///
    /// Panics on error
    pub fn client(&self) -> blobfs::Client {
        blobfs::Client::new(self.root_dir_proxy().unwrap())
    }

    /// Returns a new connection to blobfs's root directory as a raw zircon channel.
    pub fn root_dir_handle(&self) -> Result<ClientEnd<DirectoryMarker>, Error> {
        let (root_clone, server_end) = zx::Channel::create()?;
        self.proxy.clone(fidl_fuchsia_io::CLONE_FLAG_SAME_RIGHTS, server_end.into())?;
        Ok(root_clone.into())
    }

    /// Returns a new connection to blobfs's root directory as a DirectoryProxy.
    pub fn root_dir_proxy(&self) -> Result<DirectoryProxy, Error> {
        Ok(self.root_dir_handle()?.into_proxy()?)
    }

    /// Returns a new connetion to blobfs's root directory as a openat::Dir.
    pub fn root_dir(&self) -> Result<openat::Dir, Error> {
        fdio::create_fd(self.root_dir_handle()?.into()).context("failed to create fd")
    }

    /// Signals blobfs to unmount and waits for it to exit cleanly, returning a new
    /// [`BlobfsRamdiskBuilder`] initialized with the ramdisk.
    pub async fn into_builder(self) -> Result<BlobfsRamdiskBuilder, Error> {
        let ramdisk = self.unmount().await?;
        Ok(Self::builder().ramdisk(ramdisk))
    }

    /// Signals blobfs to unmount and waits for it to exit cleanly, returning the backing Ramdisk.
    pub async fn unmount(self) -> Result<FormattedRamdisk, Error> {
        zx::Status::ok(self.proxy.unmount().await.context("sending blobfs unmount")?)
            .context("unmounting blobfs")?;

        self.process
            .wait_handle(
                zx::Signals::PROCESS_TERMINATED,
                zx::Time::after(zx::Duration::from_seconds(30)),
            )
            .context("waiting for 'blobfs mount' to exit")?;
        let ret = self.process.info().context("getting 'blobfs mount' process info")?.return_code;
        if ret != 0 {
            return Err(format_err!("'blobfs mount' returned nonzero exit code {}", ret));
        }

        Ok(self.backing_ramdisk)
    }

    /// Signals blobfs to unmount and waits for it to exit cleanly, stopping the inner ramdisk.
    pub async fn stop(self) -> Result<(), Error> {
        self.unmount().await?.stop()
    }

    /// Returns a sorted list of all blobs present in this blobfs instance.
    pub fn list_blobs(&self) -> Result<BTreeSet<Hash>, Error> {
        self.root_dir()?
            .list_dir(".")?
            .map(|entry| {
                Ok(entry?
                    .file_name()
                    .to_str()
                    .ok_or_else(|| anyhow::format_err!("expected valid utf-8"))?
                    .parse()?)
            })
            .collect()
    }

    /// Writes the blob to blobfs.
    pub fn add_blob_from(
        &self,
        merkle: &Hash,
        mut source: impl std::io::Read,
    ) -> Result<(), Error> {
        let mut bytes = vec![];
        source.read_to_end(&mut bytes)?;
        self.write_blob_sync(merkle, &bytes)
    }

    fn write_blob_sync(&self, merkle: &Hash, bytes: &[u8]) -> Result<(), Error> {
        use std::{convert::TryInto, io::Write};

        let mut file = self.root_dir().unwrap().write_file(merkle.to_string(), 0o777)?;
        file.set_len(bytes.len().try_into().unwrap())?;
        file.write_all(&bytes)?;
        Ok(())
    }
}

/// A helper to construct [`Ramdisk`] instances.
pub struct RamdiskBuilder {
    block_count: u64,
}

impl RamdiskBuilder {
    fn new() -> Self {
        Self { block_count: 1 << 20 }
    }

    /// Set the block count of the [`Ramdisk`].
    pub fn block_count(mut self, block_count: u64) -> Self {
        self.block_count = block_count;
        self
    }

    /// Starts a new ramdisk.
    pub fn start(self) -> Result<Ramdisk, Error> {
        let mut client = RamdiskClient::builder(RAMDISK_BLOCK_SIZE, self.block_count);
        // TODO(fxbug.dev/63402) unconditionally use /dev/sys/platform/00:00:2d/ramctl once all clients are on CFv2.
        //
        // CFv1 clients have the isolated devmgr directory injected as a service named
        // "fuchsia.test.IsolatedDevmgr".
        // CFv2 clients have the isolated devmgr directory injected directly as "/dev" since
        // it is easier to inject directories with arbitrary implementations in CFv2.
        if std::path::Path::new("/svc/fuchsia.test.IsolatedDevmgr").exists() {
            client.isolated_dev_root();
        } else {
            ramdevice_client::wait_for_device(
                "/dev/sys/platform/00:00:2d/ramctl",
                std::time::Duration::from_secs(30),
            )
            .context("/dev/sys/platform/00:00:2d/ramctl did not appear")?;
        }
        let client = client.build()?;
        let proxy = NodeProxy::new(fuchsia_async::Channel::from_channel(client.open()?)?);
        Ok(Ramdisk { proxy, client })
    }

    /// Create a [`BlobfsRamdiskBuilder`] that uses this as its backing ramdisk.
    pub fn into_blobfs_builder(self) -> Result<BlobfsRamdiskBuilder, Error> {
        Ok(BlobfsRamdiskBuilder::new().ramdisk(self.start()?.format()?))
    }
}

/// A virtual memory-backed block device.
pub struct Ramdisk {
    proxy: NodeProxy,
    client: RamdiskClient,
}

// FormattedRamdisk Derefs to Ramdisk, which is only safe if all of the &self Ramdisk methods
// preserve the blobfs formatting.
impl Ramdisk {
    /// Create a RamdiskBuilder that defaults to 1024 * 1024 blocks of size 512 bytes.
    pub fn builder() -> RamdiskBuilder {
        RamdiskBuilder::new()
    }

    /// Starts a new ramdisk with 1024 * 1024 blocks and a block size of 512 bytes, resulting in a
    /// drive with 512MiB capacity.
    pub fn start() -> Result<Self, Error> {
        Self::builder().start()
    }

    fn clone_channel(&self) -> Result<zx::Channel, Error> {
        let (result, server_end) = zx::Channel::create()?;
        self.proxy.clone(fidl_fuchsia_io::CLONE_FLAG_SAME_RIGHTS, ServerEnd::new(server_end))?;
        Ok(result)
    }

    fn clone_handle(&self) -> Result<zx::Handle, Error> {
        Ok(self.clone_channel().context("cloning ramdisk channel")?.into())
    }

    fn format(self) -> Result<FormattedRamdisk, Error> {
        mkblobfs_block(self.clone_handle()?)?;
        Ok(FormattedRamdisk(self))
    }

    /// Shuts down this ramdisk.
    pub fn stop(self) -> Result<(), Error> {
        Ok(self.client.destroy()?)
    }
}

/// A [`Ramdisk`] formatted for use by blobfs.
pub struct FormattedRamdisk(Ramdisk);

// This is safe as long as all of the &self methods of Ramdisk maintain the blobfs formatting.
impl std::ops::Deref for FormattedRamdisk {
    type Target = Ramdisk;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl FormattedRamdisk {
    /// Corrupt the blob given by merkle.
    pub async fn corrupt_blob(&self, merkle: &Hash) {
        let ramdisk = Clone::clone(&self.0.proxy);
        blobfs_corrupt_blob(ramdisk, merkle).await.unwrap();
    }

    /// Shuts down this ramdisk.
    pub fn stop(self) -> Result<(), Error> {
        self.0.stop()
    }
}

async fn blobfs_corrupt_blob(ramdisk: NodeProxy, merkle: &Hash) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.root_dir().add_service_at("block", |chan| {
        ramdisk
            .clone(
                fidl_fuchsia_io::CLONE_FLAG_SAME_RIGHTS | fidl_fuchsia_io::OPEN_FLAG_DESCRIBE,
                ServerEnd::new(chan),
            )
            .unwrap();
        None
    });

    let (devfs_client, devfs_server) = zx::Channel::create()?;
    fs.serve_connection(devfs_server)?;
    let serve_fs = fs.collect::<()>();

    let spawn_and_wait = async move {
        let p = fdio::spawn_etc(
            &fuchsia_runtime::job_default(),
            SpawnOptions::CLONE_ALL - SpawnOptions::CLONE_NAMESPACE,
            &CString::new("/pkg/bin/blobfs-corrupt").unwrap(),
            &[
                &CString::new("blobfs-corrupt").unwrap(),
                &CString::new("--device").unwrap(),
                &CString::new("/dev/block").unwrap(),
                &CString::new("--merkle").unwrap(),
                &CString::new(merkle.to_string()).unwrap(),
            ],
            None,
            &mut [SpawnAction::add_namespace_entry(
                &CString::new("/dev").unwrap(),
                devfs_client.into(),
            )],
        )
        .map_err(|(status, _)| status)
        .context("spawning 'blobfs-corrupt'")?;

        wait_for_process_async(p).await.context("'blobfs-corrupt'")?;
        Ok(())
    };

    let ((), res) = futures::join!(serve_fs, spawn_and_wait);

    res
}

async fn wait_for_process_async(proc: fuchsia_zircon::Process) -> Result<(), Error> {
    let signals =
        fuchsia_async::OnSignals::new(&proc.as_handle_ref(), zx::Signals::PROCESS_TERMINATED)
            .await
            .context("waiting for tool to terminate")?;
    assert_eq!(signals, zx::Signals::PROCESS_TERMINATED);

    let ret = proc.info().context("getting tool process info")?.return_code;
    if ret != 0 {
        return Err(format_err!("tool returned nonzero exit code {}", ret));
    }
    Ok(())
}

fn mkblobfs_block(block_device: zx::Handle) -> Result<(), Error> {
    let block_device_handle_id = HandleInfo::new(HandleType::User0, 1);
    let p = fdio::spawn_etc(
        &fuchsia_runtime::job_default(),
        SpawnOptions::CLONE_ALL,
        &CString::new("/pkg/bin/blobfs").unwrap(),
        &[&CString::new("blobfs").unwrap(), &CString::new("mkfs").unwrap()],
        None,
        &mut [SpawnAction::add_handle(block_device_handle_id, block_device)],
    )
    .map_err(|(status, _)| status)
    .context("spawning 'blobfs mkfs'")?;

    // Frequently takes more than 30 seconds on profile builds.
    wait_for_process(p, zx::Duration::from_seconds(90)).context("waiting for 'blobfs mkfs'")?;
    Ok(())
}

fn wait_for_process(proc: fuchsia_zircon::Process, duration: zx::Duration) -> Result<(), Error> {
    proc.wait_handle(zx::Signals::PROCESS_TERMINATED, zx::Time::after(duration))
        .context("waiting for tool to terminate")?;
    let ret = proc.info().context("getting tool process info")?.return_code;
    if ret != 0 {
        return Err(format_err!("tool returned nonzero exit code {}", ret));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use {super::*, maplit::btreeset, std::io::Write};

    #[fuchsia_async::run_singlethreaded(test)]
    async fn clean_start_and_stop() {
        let blobfs = BlobfsRamdisk::start().unwrap();

        let proxy = blobfs.root_dir_proxy().unwrap();
        drop(proxy);

        blobfs.stop().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn clean_start_contains_no_blobs() {
        let blobfs = BlobfsRamdisk::start().unwrap();

        assert_eq!(blobfs.list_blobs().unwrap(), btreeset![]);

        blobfs.stop().await.unwrap();
    }

    #[test]
    fn blob_info_conversions() {
        let a = BlobInfo::from(&b"static slice"[..]);
        let b = BlobInfo::from(b"owned vec".to_vec());
        let c = BlobInfo::from(Cow::from(&b"cow"[..]));
        assert_ne!(a.merkle, b.merkle);
        assert_ne!(b.merkle, c.merkle);
        assert_eq!(
            a.merkle,
            fuchsia_merkle::MerkleTree::from_reader(&b"static slice"[..]).unwrap().root()
        );

        // Verify the following calling patterns build, but don't bother building the ramdisk.
        let _ = BlobfsRamdisk::builder()
            .with_blob(&b"static slice"[..])
            .with_blob(b"owned vec".to_vec())
            .with_blob(Cow::from(&b"cow"[..]));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn with_blob_ignores_duplicates() {
        let blob = BlobInfo::from(&b"duplicate"[..]);

        let blobfs = BlobfsRamdisk::builder()
            .with_blob(blob.clone())
            .with_blob(blob.clone())
            .start()
            .unwrap();
        assert_eq!(blobfs.list_blobs().unwrap(), btreeset![blob.merkle.clone()]);

        let blobfs = blobfs.into_builder().await.unwrap().with_blob(blob.clone()).start().unwrap();
        assert_eq!(blobfs.list_blobs().unwrap(), btreeset![blob.merkle.clone()]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn build_with_two_blobs() {
        let blobfs = BlobfsRamdisk::builder()
            .with_blob(&b"blob 1"[..])
            .with_blob(&b"blob 2"[..])
            .start()
            .unwrap();

        let expected = btreeset![
            fuchsia_merkle::MerkleTree::from_reader(&b"blob 1"[..]).unwrap().root(),
            fuchsia_merkle::MerkleTree::from_reader(&b"blob 2"[..]).unwrap().root(),
        ];
        assert_eq!(expected.len(), 2);
        assert_eq!(blobfs.list_blobs().unwrap(), expected);

        blobfs.stop().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn remount() {
        let blobfs = BlobfsRamdisk::builder().with_blob(&b"test"[..]).start().unwrap();
        let blobs = blobfs.list_blobs().unwrap();

        let blobfs = blobfs.into_builder().await.unwrap().start().unwrap();

        assert_eq!(blobs, blobfs.list_blobs().unwrap());

        blobfs.stop().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_appears_in_readdir() {
        let blobfs = BlobfsRamdisk::start().unwrap();
        let root = blobfs.root_dir().unwrap();

        let hello_merkle = write_blob(&root, "Hello blobfs!".as_bytes());
        assert_eq!(list_blobs(&root), vec![hello_merkle]);

        drop(root);
        blobfs.stop().await.unwrap();
    }

    /// Writes a blob to blobfs, returning the computed merkle root of the blob.
    fn write_blob(dir: &openat::Dir, payload: &[u8]) -> String {
        let merkle = fuchsia_merkle::MerkleTree::from_reader(payload).unwrap().root().to_string();

        let mut f = dir.new_file(&merkle, 0600).unwrap();
        f.set_len(payload.len() as u64).unwrap();
        f.write_all(payload).unwrap();

        merkle
    }

    /// Returns an unsorted list of blobs in the given blobfs dir.
    fn list_blobs(dir: &openat::Dir) -> Vec<String> {
        dir.list_dir(".")
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_owned().into_string().unwrap())
            .collect()
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn ramdisk_builder_sets_block_count() {
        for block_count in &[1, 2, 3, 16] {
            let ramdisk = Ramdisk::builder().block_count(*block_count).start().unwrap();

            let (status, attr) = ramdisk.proxy.get_attr().await.unwrap();
            zx::Status::ok(status).unwrap();
            assert_eq!(attr.content_size, RAMDISK_BLOCK_SIZE * block_count);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn ramdisk_into_blobfs_formats_ramdisk() {
        let _: BlobfsRamdisk = Ramdisk::builder().into_blobfs_builder().unwrap().start().unwrap();
    }
}
