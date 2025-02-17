// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    bt_manifest_integration_lib::mock_component,
    fidl_fuchsia_bluetooth_avrcp::{PeerManagerMarker, PeerManagerProxy},
    fidl_fuchsia_bluetooth_avrcp_test::{PeerManagerExtMarker, PeerManagerExtProxy},
    fidl_fuchsia_bluetooth_bredr::{ProfileMarker, ProfileRequest},
    fuchsia_async as fasync,
    fuchsia_component_test::{
        builder::{ComponentSource, RealmBuilder, RouteEndpoint},
        mock::{Mock, MockHandles},
    },
    futures::{channel::mpsc, SinkExt, StreamExt},
    tracing::info,
};

/// AVRCP component URL.
const AVRCP_URL: &str = "fuchsia-pkg://fuchsia.com/bt-avrcp-smoke-test#meta/bt-avrcp.cm";

/// The different events generated by this test.
/// Note: In order to prevent the component under test from terminating, any FIDL request or
/// Proxy is preserved.
enum Event {
    /// A BR/EDR Profile event.
    Profile(Option<ProfileRequest>),
    /// AVRCP PeerManager event.
    PeerManager(Option<PeerManagerProxy>),
    /// AVRCP PeerManagerExt event.
    PeerManagerExt(Option<PeerManagerExtProxy>),
}

impl From<ProfileRequest> for Event {
    fn from(src: ProfileRequest) -> Self {
        Self::Profile(Some(src))
    }
}

/// Represents a fake AVRCP client that requests the PeerManager and PeerManagerExt services.
async fn mock_avrcp_client(
    mut sender: mpsc::Sender<Event>,
    handles: MockHandles,
) -> Result<(), Error> {
    let peer_manager_svc = handles.connect_to_service::<PeerManagerMarker>()?;
    sender
        .send(Event::PeerManager(Some(peer_manager_svc)))
        .await
        .expect("failed sending ack to test");

    let peer_manager_ext_svc = handles.connect_to_service::<PeerManagerExtMarker>()?;
    sender
        .send(Event::PeerManagerExt(Some(peer_manager_ext_svc)))
        .await
        .expect("failed sending ack to test");
    Ok(())
}

/// Tests that the v2 AVRCP component has the correct topology and verifies that
/// it connects and provides the expected services.
#[fasync::run_singlethreaded(test)]
async fn avrcp_v2_component_topology() {
    fuchsia_syslog::init().unwrap();
    info!("Starting AVRCP v2 smoke test...");

    let (sender, mut receiver) = mpsc::channel(2);
    let profile_tx = sender.clone();
    let fake_client_tx = sender.clone();

    let mut builder = RealmBuilder::new().await.expect("Failed to create test realm builder");
    // The v2 component under test.
    let _ = builder
        .add_component("avrcp", ComponentSource::url(AVRCP_URL.to_string()))
        .await
        .expect("Failed adding avrcp to topology");
    // Mock Profile component to receive bredr.Profile requests.
    let _ = builder
        .add_component(
            "fake-profile",
            ComponentSource::Mock(Mock::new({
                move |mock_handles: MockHandles| {
                    let sender = profile_tx.clone();
                    Box::pin(mock_component::<ProfileMarker, _>(sender, mock_handles))
                }
            })),
        )
        .await
        .expect("Failed adding profile mock to topology");
    // Mock AVRCP client that will request the PeerManager and PeerManagerExt services
    // which are provided by `bt-avrcp.cml`.
    let _ = builder
        .add_eager_component(
            "fake-avrcp-client",
            ComponentSource::Mock(Mock::new({
                move |mock_handles: MockHandles| {
                    let sender = fake_client_tx.clone();
                    Box::pin(mock_avrcp_client(sender, mock_handles))
                }
            })),
        )
        .await
        .expect("Failed adding avrcp client mock to topology");

    // Set up capabilities.
    let _ = builder
        .add_protocol_route::<PeerManagerMarker>(
            RouteEndpoint::component("avrcp"),
            vec![RouteEndpoint::component("fake-avrcp-client")],
        )
        .expect("Failed adding route for PeerManager service")
        .add_protocol_route::<PeerManagerExtMarker>(
            RouteEndpoint::component("avrcp"),
            vec![RouteEndpoint::component("fake-avrcp-client")],
        )
        .expect("Failed adding route for PeerManagerExt service")
        .add_protocol_route::<ProfileMarker>(
            RouteEndpoint::component("fake-profile"),
            vec![RouteEndpoint::component("avrcp")],
        )
        .expect("Failed adding route for Profile service")
        .add_protocol_route::<fidl_fuchsia_logger::LogSinkMarker>(
            RouteEndpoint::AboveRoot,
            vec![
                RouteEndpoint::component("avrcp"),
                RouteEndpoint::component("fake-profile"),
                RouteEndpoint::component("fake-avrcp-client"),
            ],
        )
        .expect("Failed adding LogSink route to test components");
    let _test_topology = builder.build().create().await.unwrap();

    // If the routing is correctly configured, we expect four events:
    //   1: `bt-avrcp` connecting to the Profile service.
    //     a. Making a request to Advertise.
    //     b. Making a request to Search.
    //   2. `fake-avrcp-client` connecting to the PeerManager service which is provided by `bt-avrcp`.
    //   3. `fake-avrcp-client` connecting to the PeerManagerExt service which is provided by `bt-avrcp`.
    let mut events = Vec::new();
    for i in 0..4 {
        let msg = format!("Unexpected error waiting for {:?} event", i);
        events.push(receiver.next().await.expect(&msg));
    }
    assert_eq!(events.len(), 4);
    let discriminants: Vec<_> = events.iter().map(std::mem::discriminant).collect();
    assert_eq!(
        discriminants
            .iter()
            .filter(|&&d| d == std::mem::discriminant(&Event::Profile(None)))
            .count(),
        2
    );
    assert_eq!(
        discriminants
            .iter()
            .filter(|&&d| d == std::mem::discriminant(&Event::PeerManager(None)))
            .count(),
        1
    );
    assert_eq!(
        discriminants
            .iter()
            .filter(|&&d| d == std::mem::discriminant(&Event::PeerManagerExt(None)))
            .count(),
        1
    );

    info!("Finished AVRCP smoke test");
}
