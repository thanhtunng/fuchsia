// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{builtin::capability::ResourceCapability, capability::*},
    anyhow::Error,
    async_trait::async_trait,
    cm_rust::CapabilityName,
    fidl::endpoints::ProtocolMarker,
    fidl_fuchsia_kernel as fkernel,
    fuchsia_zircon::{self as zx, HandleBased, Resource, ResourceInfo},
    futures::prelude::*,
    lazy_static::lazy_static,
    std::sync::Arc,
};

lazy_static! {
    static ref MMIO_RESOURCE_CAPABILITY_NAME: CapabilityName = "fuchsia.kernel.MmioResource".into();
}

/// An implementation of fuchsia.kernel.MmioResource protocol.
pub struct MmioResource {
    resource: Resource,
}

impl MmioResource {
    /// `resource` must be the MMIO resource.
    pub fn new(resource: Resource) -> Arc<Self> {
        Arc::new(Self { resource })
    }
}

#[async_trait]
impl ResourceCapability for MmioResource {
    const KIND: zx::sys::zx_rsrc_kind_t = zx::sys::ZX_RSRC_KIND_MMIO;
    const NAME: &'static str = "MmioResource";
    type Marker = fkernel::MmioResourceMarker;

    fn get_resource_info(self: &Arc<Self>) -> Result<ResourceInfo, Error> {
        Ok(self.resource.info()?)
    }

    async fn server_loop(
        self: Arc<Self>,
        mut stream: <Self::Marker as ProtocolMarker>::RequestStream,
    ) -> Result<(), Error> {
        while let Some(fkernel::MmioResourceRequest::Get { responder }) = stream.try_next().await? {
            responder.send(self.resource.duplicate_handle(zx::Rights::SAME_RIGHTS)?)?;
        }
        Ok(())
    }

    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool {
        capability.matches_protocol(&MMIO_RESOURCE_CAPABILITY_NAME)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            builtin::capability::BuiltinCapability,
            model::hooks::{Event, EventPayload, Hooks},
        },
        fidl::endpoints::ClientEnd,
        fidl_fuchsia_kernel as fkernel, fuchsia_async as fasync,
        fuchsia_component::client::connect_to_protocol,
        fuchsia_zircon::AsHandleRef,
        fuchsia_zircon_sys as sys,
        futures::lock::Mutex,
        moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
        std::path::PathBuf,
        std::sync::Weak,
    };

    fn mmio_resource_available() -> bool {
        let bin = std::env::args().next();
        match bin.as_ref().map(String::as_ref) {
            Some("/pkg/bin/component_manager_test") => false,
            Some("/pkg/bin/component_manager_boot_env_test") => true,
            _ => panic!("Unexpected test binary name {:?}", bin),
        }
    }

    async fn get_mmio_resource() -> Result<Resource, Error> {
        let mmio_resource_provider = connect_to_protocol::<fkernel::MmioResourceMarker>()?;
        let mmio_resource_handle = mmio_resource_provider.get().await?;
        Ok(Resource::from(mmio_resource_handle))
    }

    async fn serve_mmio_resource() -> Result<fkernel::MmioResourceProxy, Error> {
        let mmio_resource = get_mmio_resource().await?;

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<fkernel::MmioResourceMarker>()?;
        fasync::Task::local(
            MmioResource::new(mmio_resource)
                .serve(stream)
                .unwrap_or_else(|e| panic!("Error while serving MMIO resource service: {}", e)),
        )
        .detach();
        Ok(proxy)
    }

    #[fuchsia::test]
    async fn fail_with_no_mmio_resource() -> Result<(), Error> {
        if mmio_resource_available() {
            return Ok(());
        }
        let (_, stream) =
            fidl::endpoints::create_proxy_and_stream::<fkernel::MmioResourceMarker>()?;
        assert!(!MmioResource::new(Resource::from(zx::Handle::invalid()))
            .serve(stream)
            .await
            .is_ok());
        Ok(())
    }

    #[fuchsia::test]
    async fn kind_type_is_mmio() -> Result<(), Error> {
        if !mmio_resource_available() {
            return Ok(());
        }

        let mmio_resource_provider = serve_mmio_resource().await?;
        let mmio_resource: Resource = mmio_resource_provider.get().await?;
        let resource_info = mmio_resource.info()?;
        assert_eq!(resource_info.kind, zx::sys::ZX_RSRC_KIND_MMIO);
        assert_eq!(resource_info.base, 0);
        assert_eq!(resource_info.size, 0);
        Ok(())
    }

    #[fuchsia::test]
    async fn can_connect_to_mmio_service() -> Result<(), Error> {
        if !mmio_resource_available() {
            return Ok(());
        }

        let mmio_resource = MmioResource::new(get_mmio_resource().await?);
        let hooks = Hooks::new(None);
        hooks.install(mmio_resource.hooks()).await;

        let provider = Arc::new(Mutex::new(None));
        let source = CapabilitySource::Builtin {
            capability: InternalCapability::Protocol(MMIO_RESOURCE_CAPABILITY_NAME.clone()),
            top_instance: Weak::new(),
        };

        let event = Event::new_for_test(
            AbsoluteMoniker::root(),
            "fuchsia-pkg://root",
            Ok(EventPayload::CapabilityRouted { source, capability_provider: provider.clone() }),
        );
        hooks.dispatch(&event).await?;

        let (client, mut server) = zx::Channel::create()?;
        let _provider_task = if let Some(provider) = provider.lock().await.take() {
            provider.open(0, 0, PathBuf::new(), &mut server).await?.take()
        } else {
            None
        };

        let mmio_client = ClientEnd::<fkernel::MmioResourceMarker>::new(client)
            .into_proxy()
            .expect("failed to create launcher proxy");
        let mmio_resource = mmio_client.get().await?;
        assert_ne!(mmio_resource.raw_handle(), sys::ZX_HANDLE_INVALID);
        Ok(())
    }
}
