// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    anyhow::{Context, Error},
    blobfs_ramdisk::BlobfsRamdisk,
    fidl::endpoints::{ClientEnd, RequestStream, ServerEnd},
    fidl_fuchsia_io::{
        self as fio, DirectoryAdminRequest, DirectoryAdminRequestStream, DirectoryMarker,
        DirectoryObject, NodeAttributes, NodeInfo, NodeMarker,
    },
    fidl_fuchsia_paver::{Asset, Configuration, PaverRequestStream},
    fidl_fuchsia_pkg_ext::{MirrorConfigBuilder, RepositoryConfigBuilder, RepositoryConfigs},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_merkle::Hash,
    fuchsia_pkg_testing::{serve::ServedRepository, Package, PackageBuilder, RepositoryBuilder},
    fuchsia_url::pkg_url::RepoUrl,
    fuchsia_zircon as zx,
    futures::prelude::*,
    http::uri::Uri,
    isolated_ota::{download_and_apply_update, OmahaConfig, UpdateError},
    matches::assert_matches,
    mock_omaha_server::{OmahaResponse, OmahaServer},
    mock_paver::{hooks as mphooks, MockPaverService, MockPaverServiceBuilder, PaverEvent},
    serde_json::json,
    std::{
        collections::{BTreeSet, HashMap},
        io::Write,
        str::FromStr,
        sync::Arc,
    },
    tempfile::TempDir,
};

const EMPTY_REPO_PATH: &str = "/pkg/empty-repo";
const TEST_CERTS_PATH: &str = "/pkg/data/ssl";
const TEST_REPO_URL: &str = "fuchsia-pkg://integration.test.fuchsia.com";

enum OmahaState {
    /// Don't use Omaha for this update.
    Disabled,
    /// Set up an Omaha server automatically.
    Auto(OmahaResponse),
    /// Pass the given OmahaConfig to Omaha.
    Manual(OmahaConfig),
}

struct TestEnvBuilder {
    blobfs: Option<ClientEnd<DirectoryMarker>>,
    board: String,
    channel: String,
    images: HashMap<String, Vec<u8>>,
    omaha: OmahaState,
    packages: Vec<Package>,
    paver: MockPaverServiceBuilder,
    repo_config: Option<RepositoryConfigs>,
    version: String,
}

impl TestEnvBuilder {
    pub fn new() -> Self {
        TestEnvBuilder {
            blobfs: None,
            board: "test-board".to_owned(),
            channel: "test".to_owned(),
            images: HashMap::new(),
            omaha: OmahaState::Disabled,
            packages: vec![],
            paver: MockPaverServiceBuilder::new(),
            repo_config: None,
            version: "0.1.2.3".to_owned(),
        }
    }

    /// Add an image to the update package that will be generated by this TestEnvBuilder
    pub fn add_image(mut self, name: &str, data: &[u8]) -> Self {
        self.images.insert(name.to_owned(), data.to_vec());
        self
    }

    /// Add a package to the repository generated by this TestEnvBuilder.
    /// The package will also be listed in the generated update package
    /// so that it will be downloaded as part of the OTA.
    pub fn add_package(mut self, pkg: Package) -> Self {
        self.packages.push(pkg);
        self
    }

    pub fn blobfs(mut self, client: ClientEnd<DirectoryMarker>) -> Self {
        self.blobfs = Some(client);
        self
    }

    /// Provide a TUF repository configuration to the package resolver.
    /// This will override the repository that the builder would otherwise generate.
    pub fn repo_config(mut self, repo: RepositoryConfigs) -> Self {
        self.repo_config = Some(repo);
        self
    }

    /// Enable/disable Omaha. OmahaState::Auto will automatically set up an Omaha server and tell
    /// the updater to use it.
    pub fn omaha_state(mut self, state: OmahaState) -> Self {
        self.omaha = state;
        self
    }

    /// Mutate the MockPaverServiecBuilder used by this TestEnvBuilder.
    pub fn paver<F>(mut self, func: F) -> Self
    where
        F: FnOnce(MockPaverServiceBuilder) -> MockPaverServiceBuilder,
    {
        self.paver = func(self.paver);
        self
    }

    fn generate_packages(&self) -> String {
        json!({
            "version": "1",
            "content": self.packages
                .iter()
                .map(|p| format!("{}/{}/0?hash={}",
                                 TEST_REPO_URL,
                                 p.name(),
                                 p.meta_far_merkle_root()))
                .collect::<Vec<String>>(),
        })
        .to_string()
    }

    /// Turn this |TestEnvBuilder| into a |TestEnv|
    pub async fn build(mut self) -> Result<TestEnv, Error> {
        let mut update = PackageBuilder::new("update")
            .add_resource_at("packages.json", self.generate_packages().as_bytes());

        let (repo_config, served_repo, cert_dir, packages, merkle) = if self.repo_config.is_none() {
            // If no repo config was specified, host a repo containing the provided packages,
            // and an update package containing given images + all packages in the repo.
            for (name, data) in self.images.iter() {
                update = update.add_resource_at(name, data.as_slice());
            }

            let update = update.build().await.context("Building update package")?;
            let repo = Arc::new(
                self.packages
                    .iter()
                    .fold(
                        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH).add_package(&update),
                        |repo, package| repo.add_package(package),
                    )
                    .build()
                    .await
                    .context("Building repo")?,
            );

            let served_repo = Arc::clone(&repo).server().start().context("serving repo")?;
            let config = RepositoryConfigs::Version1(vec![
                served_repo.make_repo_config(TEST_REPO_URL.parse()?)
            ]);

            let update_merkle = update.meta_far_merkle_root().clone();
            // Add the update package to the list of packages, so that TestResult::check_packages
            // will expect to see the update package's blobs in blobfs.
            let mut packages = vec![update];
            packages.append(&mut self.packages);
            (
                config,
                Some(served_repo),
                std::fs::File::open(TEST_CERTS_PATH).context("opening test certificates")?,
                packages,
                update_merkle,
            )
        } else {
            // Use the provided repo config. Assume that this means we'll actually want to use
            // real SSL certificates, and that we don't need to host our own repository.
            (
                self.repo_config.unwrap(),
                None,
                std::fs::File::open("/config/ssl").context("opening system ssl certificates")?,
                vec![],
                Hash::from_str("deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
                    .context("Making merkle")?,
            )
        };

        let dir = tempfile::tempdir()?;
        let mut path = dir.path().to_owned();
        path.push("repo_config.json");
        let path = path.as_path();
        let mut file =
            std::io::BufWriter::new(std::fs::File::create(path).context("creating file")?);
        serde_json::to_writer(&mut file, &repo_config).unwrap();
        file.flush().unwrap();

        Ok(TestEnv {
            blobfs: self.blobfs,
            board: self.board,
            channel: self.channel,
            omaha: self.omaha,
            packages,
            paver: Arc::new(self.paver.build()),
            _repo: served_repo,
            repo_config_dir: dir,
            ssl_certs: cert_dir,
            update_merkle: merkle,
            version: self.version,
        })
    }
}

struct TestEnv {
    blobfs: Option<ClientEnd<DirectoryMarker>>,
    board: String,
    channel: String,
    omaha: OmahaState,
    packages: Vec<Package>,
    paver: Arc<MockPaverService>,
    _repo: Option<ServedRepository>,
    repo_config_dir: TempDir,
    ssl_certs: std::fs::File,
    update_merkle: Hash,
    version: String,
}

impl TestEnv {
    fn start_omaha(omaha: OmahaState, merkle: Hash) -> Result<Option<OmahaConfig>, Error> {
        match omaha {
            OmahaState::Disabled => Ok(None),
            OmahaState::Manual(cfg) => Ok(Some(cfg)),
            OmahaState::Auto(response) => {
                let server = OmahaServer::new_with_hash(response, merkle);
                let addr = server.start().context("Starting omaha server")?;
                let config =
                    OmahaConfig { app_id: "integration-test-appid".to_owned(), server_url: addr };

                Ok(Some(config))
            }
        }
    }

    /// Run the update, consuming this |TestEnv| and returning a |TestResult|.
    pub async fn run(mut self) -> TestResult {
        let (blobfs, blobfs_handle) = if let Some(client) = self.blobfs.take() {
            (None, client)
        } else {
            let blobfs = BlobfsRamdisk::start().expect("launching blobfs");
            let client = blobfs.root_dir_handle().expect("getting blobfs root handle");
            (Some(blobfs), client)
        };

        let omaha_config =
            TestEnv::start_omaha(self.omaha, self.update_merkle).expect("Starting Omaha server");

        let mut service_fs = ServiceFs::new();
        let paver_clone = Arc::clone(&self.paver);
        service_fs.add_fidl_service(move |stream: PaverRequestStream| {
            fasync::Task::spawn(
                Arc::clone(&paver_clone)
                    .run_paver_service(stream)
                    .unwrap_or_else(|e| panic!("Failed to run mock paver: {:?}", e)),
            )
            .detach();
        });
        let (client, server) = zx::Channel::create().expect("creating channel for servicefs");
        service_fs.serve_connection(server).expect("Failed to serve connection");
        fasync::Task::spawn(service_fs.collect()).detach();

        let result = download_and_apply_update(
            blobfs_handle,
            ClientEnd::from(client),
            std::fs::File::open(self.repo_config_dir.into_path()).expect("Opening repo config dir"),
            self.ssl_certs,
            &self.channel,
            &self.board,
            &self.version,
            omaha_config,
        )
        .await;

        TestResult {
            blobfs,
            packages: self.packages,
            paver_events: self.paver.take_events(),
            result,
        }
    }
}

struct TestResult {
    blobfs: Option<BlobfsRamdisk>,
    packages: Vec<Package>,
    pub paver_events: Vec<PaverEvent>,
    pub result: Result<(), UpdateError>,
}

impl TestResult {
    /// Assert that all blobs in all the packages that were part of the Update
    /// have been installed into the blobfs, and that the blobfs contains no extra blobs.
    pub fn check_packages(&self) {
        let written_blobs = self
            .blobfs
            .as_ref()
            .unwrap_or_else(|| panic!("Test had no blobfs"))
            .list_blobs()
            .expect("Listing blobfs blobs");
        let mut all_package_blobs = BTreeSet::new();
        for package in self.packages.iter() {
            all_package_blobs.append(&mut package.list_blobs().expect("Listing package blobs"));
        }

        assert_eq!(written_blobs, all_package_blobs);
    }
}

async fn build_test_package() -> Result<Package, Error> {
    PackageBuilder::new("test-package")
        .add_resource_at("data/test", "hello, world!".as_bytes())
        .build()
        .await
        .context("Building test package")
}

#[fasync::run_singlethreaded(test)]
pub async fn test_no_network() -> Result<(), Error> {
    // Test what happens when we can't reach the remote repo.
    let bad_mirror =
        MirrorConfigBuilder::new("http://does-not-exist.fuchsia.com".parse::<Uri>().unwrap())?
            .build();
    let invalid_repo = RepositoryConfigs::Version1(vec![RepositoryConfigBuilder::new(
        RepoUrl::new("fuchsia.com".to_owned())?,
    )
    .add_mirror(bad_mirror)
    .build()]);

    let env = TestEnvBuilder::new()
        .repo_config(invalid_repo)
        .build()
        .await
        .context("Building TestEnv")?;

    let update_result = env.run().await;
    assert_eq!(
        update_result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
        ]
    );
    update_result.check_packages();

    let err = update_result.result.unwrap_err();
    assert_matches!(err, UpdateError::InstallError(_));
    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_pave_fails() -> Result<(), Error> {
    // Test what happens if the paver fails while paving.
    let test_package = build_test_package().await?;
    let paver_hook = |p: &PaverEvent| {
        if let PaverEvent::WriteAsset { payload, .. } = p {
            if payload.as_slice() == "FAIL".as_bytes() {
                return zx::Status::IO;
            }
        }
        zx::Status::OK
    };

    let env = TestEnvBuilder::new()
        .paver(|p| p.insert_hook(mphooks::return_error(paver_hook)))
        .add_package(test_package)
        .add_image("zbi.signed", "FAIL".as_bytes())
        .add_image("fuchsia.vbmeta", "FAIL".as_bytes())
        .build()
        .await
        .context("Building TestEnv")?;

    let result = env.run().await;
    assert_eq!(
        result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            PaverEvent::WriteAsset {
                asset: Asset::Kernel,
                configuration: Configuration::B,
                payload: "FAIL".as_bytes().to_vec(),
            },
        ]
    );
    println!("Paver events: {:?}", result.paver_events);
    assert_matches!(result.result.unwrap_err(), UpdateError::InstallError(_));

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_updater_succeeds() -> Result<(), Error> {
    let mut builder = TestEnvBuilder::new()
        .add_image("zbi.signed", "This is a zbi".as_bytes())
        .add_image("fuchsia.vbmeta", "This is a vbmeta".as_bytes())
        .add_image("zedboot.signed", "This is zedboot".as_bytes())
        .add_image("recovery.vbmeta", "This is another vbmeta".as_bytes())
        .add_image("bootloader", "This is a bootloader upgrade".as_bytes())
        .add_image("firmware_test", "This is the test firmware".as_bytes());
    for i in 0i64..3 {
        let name = format!("test-package{}", i);
        let package = PackageBuilder::new(name)
            .add_resource_at(
                format!("data/my-package-data-{}", i),
                format!("This is some test data for test package {}", i).as_bytes(),
            )
            .add_resource_at("bin/binary", "#!/boot/bin/sh\necho Hello".as_bytes())
            .build()
            .await
            .context("Building test package")?;
        builder = builder.add_package(package);
    }

    let env = builder.build().await.context("Building TestEnv")?;
    let result = env.run().await;

    result.check_packages();
    assert!(result.result.is_ok());
    assert_eq!(
        result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "".to_owned(),
                payload: "This is a bootloader upgrade".as_bytes().to_vec(),
            },
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "test".to_owned(),
                payload: "This is the test firmware".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::Kernel,
                payload: "This is a zbi".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::VerifiedBootMetadata,
                payload: "This is a vbmeta".as_bytes().to_vec(),
            },
            // Note that zedboot/recovery isn't written, as isolated-ota skips them.
            PaverEvent::SetConfigurationActive { configuration: Configuration::B },
            PaverEvent::DataSinkFlush,
            PaverEvent::BootManagerFlush,
            // This is the isolated-ota library checking to see if the paver configured ABR properly.
            PaverEvent::QueryActiveConfiguration,
        ]
    );
    Ok(())
}

fn launch_cloned_blobfs(end: ServerEnd<NodeMarker>, flags: u32, parent_flags: u32) {
    let flags = if (flags & fio::CLONE_FLAG_SAME_RIGHTS) == fio::CLONE_FLAG_SAME_RIGHTS {
        parent_flags
    } else {
        flags
    };
    let chan = fidl::AsyncChannel::from_channel(end.into_channel()).expect("cloning blobfs dir");
    let stream = DirectoryAdminRequestStream::from_channel(chan);
    fasync::Task::spawn(async move {
        serve_failing_blobfs(stream, flags)
            .await
            .unwrap_or_else(|e| panic!("Failed to serve cloned blobfs handle: {:?}", e));
    })
    .detach();
}

async fn serve_failing_blobfs(
    mut stream: DirectoryAdminRequestStream,
    open_flags: u32,
) -> Result<(), Error> {
    if (open_flags & fio::OPEN_FLAG_DESCRIBE) == fio::OPEN_FLAG_DESCRIBE {
        stream
            .control_handle()
            .send_on_open_(
                zx::Status::OK.into_raw(),
                Some(&mut NodeInfo::Directory(DirectoryObject)),
            )
            .context("sending on open")?;
    }
    while let Some(req) = stream.try_next().await? {
        match req {
            DirectoryAdminRequest::Clone { object, flags, .. } => {
                launch_cloned_blobfs(object, flags, open_flags)
            }
            DirectoryAdminRequest::Close { responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing close")?
            }
            DirectoryAdminRequest::Describe { responder } => {
                responder.send(&mut NodeInfo::Directory(DirectoryObject)).context("describing")?
            }
            DirectoryAdminRequest::Sync { responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing sync")?
            }
            DirectoryAdminRequest::AdvisoryLock { responder, .. } => {
                responder.send(&mut Err(zx::sys::ZX_ERR_NOT_SUPPORTED))?
            }
            DirectoryAdminRequest::GetAttr { responder } => responder
                .send(
                    zx::Status::IO.into_raw(),
                    &mut NodeAttributes {
                        mode: 0,
                        id: 0,
                        content_size: 0,
                        storage_size: 0,
                        link_count: 0,
                        creation_time: 0,
                        modification_time: 0,
                    },
                )
                .context("failing getattr")?,
            DirectoryAdminRequest::SetAttr { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing setattr")?
            }
            DirectoryAdminRequest::NodeGetFlags { responder } => {
                responder.send(zx::Status::IO.into_raw(), 0).context("failing getflags")?
            }
            DirectoryAdminRequest::NodeSetFlags { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing setflags")?
            }
            DirectoryAdminRequest::Open { object, flags, path, .. } => {
                if &path == "." {
                    launch_cloned_blobfs(object, flags, open_flags);
                } else {
                    object.close_with_epitaph(zx::Status::IO).context("failing open")?;
                }
            }
            DirectoryAdminRequest::AddInotifyFilter { responder, .. } => {
                responder.send().context("failing addinotifyfilter")?
            }
            DirectoryAdminRequest::Unlink { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing unlink")?
            }
            DirectoryAdminRequest::Unlink2 { responder, .. } => {
                responder.send(&mut Err(zx::Status::IO.into_raw())).context("failing unlink2")?
            }
            DirectoryAdminRequest::ReadDirents { responder, .. } => {
                responder.send(zx::Status::IO.into_raw(), &[]).context("failing readdirents")?
            }
            DirectoryAdminRequest::Rewind { responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing rewind")?
            }
            DirectoryAdminRequest::GetToken { responder } => {
                responder.send(zx::Status::IO.into_raw(), None).context("failing gettoken")?
            }
            DirectoryAdminRequest::Rename { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing rename")?
            }
            DirectoryAdminRequest::Rename2 { responder, .. } => {
                responder.send(&mut Err(zx::Status::IO.into_raw())).context("failing rename2")?
            }
            DirectoryAdminRequest::Link { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing link")?
            }
            DirectoryAdminRequest::Watch { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing watch")?
            }
            DirectoryAdminRequest::Mount { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing mount")?
            }
            DirectoryAdminRequest::MountAndCreate { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing mountandcreate")?
            }
            DirectoryAdminRequest::Unmount { responder, .. } => {
                responder.send(zx::Status::IO.into_raw()).context("failing umount")?
            }
            DirectoryAdminRequest::UnmountNode { responder, .. } => {
                responder.send(zx::Status::IO.into_raw(), None).context("failing unmountnode")?
            }
            DirectoryAdminRequest::QueryFilesystem { responder, .. } => responder
                .send(zx::Status::IO.into_raw(), None)
                .context("failing queryfilesystem")?,
            DirectoryAdminRequest::GetDevicePath { responder } => {
                responder.send(zx::Status::IO.into_raw(), None).context("failing getdevicepath")?
            }
        };
    }

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_blobfs_broken() -> Result<(), Error> {
    let (client, server) = zx::Channel::create().context("creating blobfs channel")?;
    let package = build_test_package().await?;
    let paver_hook = |_: &PaverEvent| zx::Status::IO;
    let env = TestEnvBuilder::new()
        .add_package(package)
        .add_image("zbi.signed", "ZBI".as_bytes())
        .blobfs(ClientEnd::from(client))
        .paver(|p| p.insert_hook(mphooks::return_error(paver_hook)))
        .build()
        .await
        .context("Building TestEnv")?;

    let stream =
        DirectoryAdminRequestStream::from_channel(fidl::AsyncChannel::from_channel(server)?);

    fasync::Task::spawn(async move {
        serve_failing_blobfs(stream, 0)
            .await
            .unwrap_or_else(|e| panic!("Failed to serve blobfs: {:?}", e));
    })
    .detach();

    let result = env.run().await;

    assert_matches!(result.result, Err(UpdateError::InstallError(_)));

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_omaha_broken() -> Result<(), Error> {
    let bad_omaha_config = OmahaConfig {
        app_id: "broken-omaha-test".to_owned(),
        server_url: "http://does-not-exist.fuchsia.com".to_owned(),
    };
    let package = build_test_package().await?;
    let env = TestEnvBuilder::new()
        .add_package(package)
        .add_image("zbi.signed", "ZBI".as_bytes())
        .omaha_state(OmahaState::Manual(bad_omaha_config))
        .build()
        .await
        .context("Building TestEnv")?;

    let result = env.run().await;
    assert_matches!(result.result, Err(UpdateError::InstallError(_)));

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_omaha_works() -> Result<(), Error> {
    let mut builder = TestEnvBuilder::new()
        .add_image("zbi.signed", "This is a zbi".as_bytes())
        .add_image("fuchsia.vbmeta", "This is a vbmeta".as_bytes())
        .add_image("zedboot.signed", "This is zedboot".as_bytes())
        .add_image("recovery.vbmeta", "This is another vbmeta".as_bytes())
        .add_image("bootloader", "This is a bootloader upgrade".as_bytes())
        .add_image("firmware_test", "This is the test firmware".as_bytes());
    for i in 0i64..3 {
        let name = format!("test-package{}", i);
        let package = PackageBuilder::new(name)
            .add_resource_at(
                format!("data/my-package-data-{}", i),
                format!("This is some test data for test package {}", i).as_bytes(),
            )
            .add_resource_at("bin/binary", "#!/boot/bin/sh\necho Hello".as_bytes())
            .build()
            .await
            .context("Building test package")?;
        builder = builder.add_package(package);
    }

    let env = builder
        .omaha_state(OmahaState::Auto(OmahaResponse::Update))
        .build()
        .await
        .context("Building TestEnv")?;

    let result = env.run().await;
    result.check_packages();
    assert!(result.result.is_ok());
    assert_eq!(
        result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "".to_owned(),
                payload: "This is a bootloader upgrade".as_bytes().to_vec(),
            },
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "test".to_owned(),
                payload: "This is the test firmware".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::Kernel,
                payload: "This is a zbi".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::VerifiedBootMetadata,
                payload: "This is a vbmeta".as_bytes().to_vec(),
            },
            // Note that zedboot/recovery isn't written, as isolated-ota skips them.
            PaverEvent::SetConfigurationActive { configuration: Configuration::B },
            PaverEvent::DataSinkFlush,
            PaverEvent::BootManagerFlush,
            // This is the isolated-ota library checking to see if the paver configured ABR properly.
            PaverEvent::QueryActiveConfiguration,
        ]
    );

    Ok(())
}
