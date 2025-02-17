// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use anyhow::Context as _;
use async_utils::async_once::Once;
use dhcp::protocol::IntoFidlExt as _;
use fuchsia_async::TimeoutExt as _;
use futures::future::TryFutureExt as _;
use futures::stream::{self, StreamExt as _, TryStreamExt as _};
use net_declare::{fidl_ip_v4, fidl_mac};
use netstack_testing_common::realms::{
    constants, KnownServiceProvider, Netstack2, TestSandboxExt as _,
};
use netstack_testing_common::Result;
use netstack_testing_macros::variants_test;
use std::cell::RefCell;

// Encapsulates a minimal configuration needed to test a DHCP client/server combination.
struct DhcpTestConfig {
    // Server IP address.
    server_addr: fidl_fuchsia_net::Ipv4Address,

    // Address pool for the DHCP server.
    managed_addrs: dhcp::configuration::ManagedAddresses,
}

impl DhcpTestConfig {
    fn expected_acquired(&self) -> fidl_fuchsia_net::Subnet {
        let Self {
            server_addr: _,
            managed_addrs:
                dhcp::configuration::ManagedAddresses { mask, pool_range_start, pool_range_stop: _ },
        } = self;
        fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv4(pool_range_start.into_fidl()),
            prefix_len: mask.ones(),
        }
    }

    fn server_subnet(&self) -> fidl_fuchsia_net::Subnet {
        let Self {
            server_addr,
            managed_addrs:
                dhcp::configuration::ManagedAddresses { mask, pool_range_start: _, pool_range_stop: _ },
        } = self;
        fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv4(*server_addr),
            prefix_len: mask.ones(),
        }
    }

    fn dhcp_parameters(&self) -> Vec<fidl_fuchsia_net_dhcp::Parameter> {
        let Self { server_addr, managed_addrs } = self;
        vec![
            fidl_fuchsia_net_dhcp::Parameter::IpAddrs(vec![*server_addr]),
            fidl_fuchsia_net_dhcp::Parameter::AddressPool(managed_addrs.into_fidl()),
        ]
    }
}

const DEFAULT_TEST_CONFIG: DhcpTestConfig = DhcpTestConfig {
    server_addr: fidl_ip_v4!("192.168.0.1"),
    managed_addrs: dhcp::configuration::ManagedAddresses {
        // We know this is safe because 25 is less than the size of an IPv4 address in bits.
        mask: unsafe { dhcp::configuration::SubnetMask::new_unchecked(25) },
        pool_range_start: std::net::Ipv4Addr::new(192, 168, 0, 2),
        pool_range_stop: std::net::Ipv4Addr::new(192, 168, 0, 5),
    },
};

const DEFAULT_NETWORK_NAME: &str = "net1";

enum DhcpEndpointType {
    Client { expected_acquired: fidl_fuchsia_net::Subnet },
    Server { static_addrs: Vec<fidl_fuchsia_net::Subnet> },
    Unbound { static_addrs: Vec<fidl_fuchsia_net::Subnet> },
}

struct DhcpTestEndpointConfig<'a> {
    ep_type: DhcpEndpointType,
    network: &'a DhcpTestNetwork<'a>,
}

// Struct for encapsulating a netemul network alongside various metadata.
// We rely heavily on the interior mutability pattern here so that we can
// pass around multiple references to the network while still modifying
// its internals.
struct DhcpTestNetwork<'a> {
    name: &'a str,
    network: Once<netemul::TestNetwork<'a>>,
    next_ep_idx: RefCell<usize>,
    sandbox: &'a netemul::TestSandbox,
}

impl<'a> DhcpTestNetwork<'a> {
    fn new(name: &'a str, sandbox: &'a netemul::TestSandbox) -> DhcpTestNetwork<'a> {
        DhcpTestNetwork { name, network: Once::new(), next_ep_idx: RefCell::new(0), sandbox }
    }

    async fn create_endpoint<E: netemul::Endpoint>(&self) -> Result<netemul::TestEndpoint<'_>> {
        let DhcpTestNetwork { name, network, next_ep_idx, sandbox } = self;
        let net = network
            .get_or_try_init(async { sandbox.create_network(name.to_string()).await })
            .await
            .context("failed to create network")?;
        let curr_idx: usize = next_ep_idx.replace_with(|&mut old| old + 1);
        let ep_name = format!("{}-{}", name, curr_idx);
        let endpoint = net
            .create_endpoint::<E, _>(ep_name.clone())
            .await
            .context("failed to create endpoint")?;
        Ok(endpoint)
    }
}

struct TestServerConfig<'a> {
    endpoints: &'a [DhcpTestEndpointConfig<'a>],
    settings: Settings<'a>,
}

struct TestNetstackRealmConfig<'a> {
    clients: &'a [DhcpTestEndpointConfig<'a>],
    servers: &'a mut [TestServerConfig<'a>],
}

async fn set_server_settings(
    dhcp_server: &fidl_fuchsia_net_dhcp::Server_Proxy,
    parameters: &mut [fidl_fuchsia_net_dhcp::Parameter],
    options: &mut [fidl_fuchsia_net_dhcp::Option_],
) -> Result {
    let parameters = stream::iter(parameters.iter_mut()).map(Ok).try_for_each_concurrent(
        None,
        |parameter| async move {
            dhcp_server
                .set_parameter(parameter)
                .await
                .context("failed to call dhcp/Server.SetParameter")?
                .map_err(fuchsia_zircon::Status::from_raw)
                .with_context(|| {
                    format!("dhcp/Server.SetParameter({:?}) returned error", parameter)
                })
        },
    );
    let options = stream::iter(options.iter_mut()).map(Ok).try_for_each_concurrent(
        None,
        |option| async move {
            dhcp_server
                .set_option(option)
                .await
                .context("failed to call dhcp/Server.SetOption")?
                .map_err(fuchsia_zircon::Status::from_raw)
                .with_context(|| format!("dhcp/Server.SetOption({:?}) returned error", option))
        },
    );
    let ((), ()) = futures::future::try_join(parameters, options).await?;
    Ok(())
}

async fn assert_client_acquires_addr(
    client_realm: &netemul::TestRealm<'_>,
    client_interface: &netemul::TestInterface<'_>,
    expected_acquired: fidl_fuchsia_net::Subnet,
    cycles: usize,
    client_renews: bool,
) {
    let client_interface_state = client_realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("failed to connect to client fuchsia.net.interfaces/State");
    let (watcher, watcher_server) =
        ::fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces::WatcherMarker>()
            .expect("failed to create interface watcher proxy");
    let () = client_interface_state
        .get_watcher(fidl_fuchsia_net_interfaces::WatcherOptions::EMPTY, watcher_server)
        .expect("failed to initialize interface watcher");
    let mut properties =
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(client_interface.id());
    for () in std::iter::repeat(()).take(cycles) {
        // Enable the interface and assert that binding fails before the address is acquired.
        let () = client_interface.stop_dhcp().await.expect("failed to stop DHCP");
        let () = client_interface.set_link_up(true).await.expect("failed to bring link up");
        matches::assert_matches!(
            bind(&client_realm, expected_acquired).await,
            Err(e @ anyhow::Error {..})
                if e.downcast_ref::<std::io::Error>()
                    .expect("bind() did not return std::io::Error")
                    .raw_os_error() == Some(libc::EADDRNOTAVAIL)
        );

        let () = client_interface.start_dhcp().await.expect("failed to start DHCP");

        let () = assert_interface_assigned_addr(
            client_realm,
            expected_acquired,
            &watcher,
            &mut properties,
        )
        .await;

        // If test covers renewal behavior, check that a subsequent interface changed event
        // occurs where the client retains its address, i.e. that it successfully renewed its
        // lease. It will take lease_length/2 duration for the client to renew its address
        // and trigger the subsequent interface changed event.
        if client_renews {
            let () = assert_interface_assigned_addr(
                client_realm,
                expected_acquired,
                &watcher,
                &mut properties,
            )
            .await;
        }
        // Set interface online signal to down and wait for address to be removed.
        let () = client_interface.set_link_up(false).await.expect("failed to bring link down");

        let () = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
            fidl_fuchsia_net_interfaces_ext::event_stream(watcher.clone()),
            &mut properties,
            |fidl_fuchsia_net_interfaces_ext::Properties {
                 id: _,
                 addresses,
                 online: _,
                 device_class: _,
                 has_default_ipv4_route: _,
                 has_default_ipv6_route: _,
                 name: _,
             }| {
                if addresses.iter().any(
                    |&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                        addr == expected_acquired
                    },
                ) {
                    None
                } else {
                    Some(())
                }
            },
        )
        .await
        .expect("failed to wait for address removal");
    }
}

async fn assert_interface_assigned_addr(
    client_realm: &netemul::TestRealm<'_>,
    expected_acquired: fidl_fuchsia_net::Subnet,
    watcher: &fidl_fuchsia_net_interfaces::WatcherProxy,
    mut properties: &mut fidl_fuchsia_net_interfaces_ext::InterfaceState,
) {
    let addr = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        fidl_fuchsia_net_interfaces_ext::event_stream(watcher.clone()),
        &mut properties,
        |fidl_fuchsia_net_interfaces_ext::Properties {
             id: _,
             addresses,
             online: _,
             device_class: _,
             has_default_ipv4_route: _,
             has_default_ipv6_route: _,
             name: _,
         }| {
            addresses.iter().find_map(
                |&fidl_fuchsia_net_interfaces_ext::Address { addr: subnet, valid_until: _ }| {
                    let fidl_fuchsia_net::Subnet { addr, prefix_len: _ } = subnet;
                    match addr {
                        fidl_fuchsia_net::IpAddress::Ipv4(_) => Some(subnet),
                        fidl_fuchsia_net::IpAddress::Ipv6(_) => None,
                    }
                },
            )
        },
    )
    .map_err(anyhow::Error::from)
    .on_timeout(
        // Netstack's DHCP client retries every 3 seconds. At the time of writing, dhcpd
        // loses the race here and only starts after the first request from the DHCP
        // client, which results in a 3 second toll. This test typically takes ~4.5
        // seconds; we apply a large multiple to be safe.
        fuchsia_async::Time::after(fuchsia_zircon::Duration::from_seconds(60)),
        || Err(anyhow::anyhow!("timed out")),
    )
    .await
    .expect("failed to observe DHCP acquisition on client ep");
    assert_eq!(addr, expected_acquired);

    // Address acquired; bind is expected to succeed.
    let _: std::net::UdpSocket =
        bind(&client_realm, expected_acquired).await.expect("binding to UDP socket failed");
}

fn bind<'a>(
    client_realm: &'a netemul::TestRealm<'_>,
    fidl_fuchsia_net::Subnet { addr, prefix_len: _ }: fidl_fuchsia_net::Subnet,
) -> impl futures::Future<Output = Result<std::net::UdpSocket>> + 'a {
    use netemul::RealmUdpSocket as _;

    let fidl_fuchsia_net_ext::IpAddress(ip_address) = addr.into();
    std::net::UdpSocket::bind_in_realm(client_realm, std::net::SocketAddr::new(ip_address, 0))
}

struct Settings<'a> {
    parameters: &'a mut [fidl_fuchsia_net_dhcp::Parameter],
    options: &'a mut [fidl_fuchsia_net_dhcp::Option_],
}

#[variants_test]
async fn test_acquire_with_dhcpd_bound_device<E: netemul::Endpoint>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("failed to create sandbox");
    let network = DhcpTestNetwork::new(DEFAULT_NETWORK_NAME, &sandbox);

    test_dhcp::<E>(
        name,
        &sandbox,
        &mut [
            TestNetstackRealmConfig {
                clients: &[DhcpTestEndpointConfig {
                    ep_type: DhcpEndpointType::Client {
                        expected_acquired: DEFAULT_TEST_CONFIG.expected_acquired(),
                    },
                    network: &network,
                }],
                servers: &mut [],
            },
            TestNetstackRealmConfig {
                clients: &[],
                servers: &mut [TestServerConfig {
                    endpoints: &[DhcpTestEndpointConfig {
                        ep_type: DhcpEndpointType::Server {
                            static_addrs: vec![DEFAULT_TEST_CONFIG.server_subnet()],
                        },
                        network: &network,
                    }],
                    settings: Settings {
                        parameters: &mut DEFAULT_TEST_CONFIG.dhcp_parameters(),
                        options: &mut [],
                    },
                }],
            },
        ],
        1,
        false,
    )
    .await
}

#[variants_test]
async fn test_acquire_then_renew_with_dhcpd_bound_device<E: netemul::Endpoint>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("failed to create sandbox");
    let network = DhcpTestNetwork::new(DEFAULT_NETWORK_NAME, &sandbox);

    // A realistic lease length that won't expire within the test timeout of 2 minutes.
    const LONG_LEASE: u32 = 60 * 60 * 24;
    // A short client renewal time which will trigger well before the test timeout of 2 minutes.
    const SHORT_RENEW: u32 = 3;

    let mut parameters = DEFAULT_TEST_CONFIG.dhcp_parameters();
    parameters.push(fidl_fuchsia_net_dhcp::Parameter::Lease(fidl_fuchsia_net_dhcp::LeaseLength {
        default: Some(LONG_LEASE),
        max: Some(LONG_LEASE),
        ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
    }));

    test_dhcp::<E>(
        name,
        &sandbox,
        &mut [
            TestNetstackRealmConfig {
                clients: &[DhcpTestEndpointConfig {
                    ep_type: DhcpEndpointType::Client {
                        expected_acquired: DEFAULT_TEST_CONFIG.expected_acquired(),
                    },
                    network: &network,
                }],
                servers: &mut [],
            },
            TestNetstackRealmConfig {
                clients: &[],
                servers: &mut [TestServerConfig {
                    endpoints: &[DhcpTestEndpointConfig {
                        ep_type: DhcpEndpointType::Server {
                            static_addrs: vec![DEFAULT_TEST_CONFIG.server_subnet()],
                        },
                        network: &network,
                    }],
                    settings: Settings {
                        parameters: &mut parameters.to_vec(),
                        options: &mut [fidl_fuchsia_net_dhcp::Option_::RenewalTimeValue(
                            SHORT_RENEW,
                        )],
                    },
                }],
            },
        ],
        1,
        true,
    )
    .await
}

#[variants_test]
async fn test_acquire_with_dhcpd_bound_device_dup_addr<E: netemul::Endpoint>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("failed to create sandbox");
    let network = DhcpTestNetwork::new(DEFAULT_NETWORK_NAME, &sandbox);

    let expected_acquired = DEFAULT_TEST_CONFIG.expected_acquired();
    let expected_addr = match expected_acquired.addr {
        fidl_fuchsia_net::IpAddress::Ipv4(fidl_fuchsia_net::Ipv4Address { addr: mut octets }) => {
            // We expect to assign the address numericaly succeeding the default client address
            // since the default client address will be assigned to a neighbor of the client so
            // the client should decline the offer and restart DHCP.
            *octets.iter_mut().last().expect("IPv4 addresses have a non-zero number of octets") +=
                1;

            fidl_fuchsia_net::Subnet {
                addr: fidl_fuchsia_net::IpAddress::Ipv4(fidl_fuchsia_net::Ipv4Address {
                    addr: octets,
                }),
                ..expected_acquired
            }
        }
        fidl_fuchsia_net::IpAddress::Ipv6(a) => {
            panic!("expected IPv4 address; got IPv6 address = {:?}", a)
        }
    };

    test_dhcp::<E>(
        name,
        &sandbox,
        &mut [
            TestNetstackRealmConfig {
                clients: &[DhcpTestEndpointConfig {
                    ep_type: DhcpEndpointType::Client { expected_acquired: expected_addr },
                    network: &network,
                }],
                servers: &mut [],
            },
            TestNetstackRealmConfig {
                clients: &[],
                servers: &mut [TestServerConfig {
                    endpoints: &[
                        DhcpTestEndpointConfig {
                            ep_type: DhcpEndpointType::Server {
                                static_addrs: vec![DEFAULT_TEST_CONFIG.server_subnet()],
                            },
                            network: &network,
                        },
                        DhcpTestEndpointConfig {
                            ep_type: DhcpEndpointType::Unbound {
                                static_addrs: vec![expected_acquired],
                            },
                            network: &network,
                        },
                    ],
                    settings: Settings {
                        parameters: &mut DEFAULT_TEST_CONFIG.dhcp_parameters(),
                        options: &mut [],
                    },
                }],
            },
        ],
        1,
        false,
    )
    .await
}

/// test_dhcp provides a flexible way to test DHCP acquisition across various network topologies.
///
/// This method will:
///   -- For each passed netstack config:
///        -- Start a netstack in a new realm
///        -- Create netemul endpoints in the referenced network and install them on the netstack
///        -- Start DHCP servers on each server endpoint
///        -- Start DHCP clients on each client endpoint and verify that they acquire the expected
///           addresses
fn test_dhcp<'a, E: netemul::Endpoint>(
    test_name: &'a str,
    sandbox: &'a netemul::TestSandbox,
    netstack_configs: &'a mut [TestNetstackRealmConfig<'a>],
    dhcp_loop_cycles: usize,
    expect_client_renews: bool,
) -> impl futures::Future<Output = ()> + 'a {
    async move {
        let dhcp_objects = stream::iter(netstack_configs.into_iter())
            .enumerate()
            .then(|(id, netstack)| async move {
                let TestNetstackRealmConfig { servers, clients } = netstack;
                let netstack_realm = sandbox
                    .create_netstack_realm_with::<Netstack2, _, _>(
                        format!("netstack_realm_{}_{}", test_name, id),
                        &[
                            KnownServiceProvider::DhcpServer { persistent: false },
                            KnownServiceProvider::DnsResolver,
                            KnownServiceProvider::SecureStash,
                        ],
                    )
                    .context("failed to create netstack realm")?;
                let netstack_realm_ref = &netstack_realm;

                let servers = stream::iter(servers.into_iter())
                    .then(|server| async move {
                        let TestServerConfig {
                            endpoints,
                            settings: Settings { options, parameters },
                        } = server;

                        let (ifaces, names_to_bind): (
                            Vec<netemul::TestInterface<'_>>,
                            Vec<String>,
                        ) = stream::iter(endpoints.into_iter())
                            .enumerate()
                            .then(|(idx, endpoint)| async move {
                                let DhcpTestEndpointConfig { ep_type, network } = endpoint;
                                let endpoint = network
                                    .create_endpoint::<E>()
                                    .await
                                    .expect("failed to create endpoint");
                                let if_name = format!("{}{}", "testeth", idx);
                                let iface = netstack_realm_ref
                                    .install_endpoint(
                                        endpoint,
                                        &netemul::InterfaceConfig::None,
                                        Some(if_name.clone()),
                                    )
                                    .await
                                    .context("failed to install server endpoint")?;
                                let (static_addrs, server_should_bind) = match ep_type {
                                    DhcpEndpointType::Client { expected_acquired: _ } => {
                                        Err(anyhow::anyhow!(
                                            "found client endpoint instead of server or unbound endpoint"
                                        ))
                                    }
                                    DhcpEndpointType::Server { static_addrs } => {
                                        Ok((static_addrs, true))
                                    }
                                    DhcpEndpointType::Unbound { static_addrs } => {
                                        Ok((static_addrs, false))
                                    }
                                }?;
                                for addr in static_addrs.into_iter() {
                                    let () = iface.add_ip_addr(*addr).await.with_context(|| {
                                        format!("failed to add address {:?}", addr)
                                    })?;
                                }
                                Result::Ok((iface, server_should_bind.then(|| if_name)))
                            })
                            .try_fold(
                                (Vec::new(), Vec::new()),
                                |(mut ifaces, mut names_to_bind), (iface, if_name)| async {
                                    let () = ifaces.push(iface);
                                    if let Some(if_name) = if_name {
                                        let () = names_to_bind.push(if_name);
                                    }
                                    Ok((ifaces, names_to_bind))
                                },
                            )
                            .await?;

                        let dhcp_server = netstack_realm_ref
                            .connect_to_protocol::<fidl_fuchsia_net_dhcp::Server_Marker>()
                            .context("failed to connect to DHCP server")?;

                        let mut parameters = parameters.to_vec();
                        let () = parameters.push(
                            fidl_fuchsia_net_dhcp::Parameter::BoundDeviceNames(names_to_bind),
                        );
                        let () =
                            set_server_settings(&dhcp_server, &mut parameters, *options).await?;

                        dhcp_server
                            .start_serving()
                            .await
                            .context("failed to call dhcp/Server.StartServing")?
                            .map_err(fuchsia_zircon::Status::from_raw)
                            .context("dhcp/Server.StartServing returned error")?;
                        Result::Ok((dhcp_server, ifaces))
                    })
                    .try_collect::<Vec<_>>()
                    .await?;

                let clients = stream::iter(clients.into_iter())
                    .then(|client| async move {
                        let DhcpTestEndpointConfig { ep_type, network } = client;
                        let endpoint = network
                            .create_endpoint::<E>()
                            .await
                            .expect("failed to create endpoint");
                        let iface = netstack_realm_ref
                            .install_endpoint(endpoint, &netemul::InterfaceConfig::None, None)
                            .await
                            .context("failed to install client endpoint")?;
                        let expected_acquired = match ep_type {
                            DhcpEndpointType::Client { expected_acquired } => Ok(expected_acquired),
                            DhcpEndpointType::Server { static_addrs: _ } => Err(anyhow::anyhow!(
                                "found server endpoint instead of client endpoint"
                            )),
                            DhcpEndpointType::Unbound { static_addrs: _ } => Err(anyhow::anyhow!(
                                "found unbound endpoint instead of client endpoint"
                            )),
                        }?;
                        Result::Ok((iface, expected_acquired))
                    })
                    .try_collect::<Vec<_>>()
                    .await?;

                Result::Ok((netstack_realm, servers, clients))
            })
            .try_collect::<Vec<_>>()
            .await
            .expect("failed to create DHCP domain objects");

        for (netstack_realm, servers, clients) in dhcp_objects {
            for (client, expected_acquired) in clients {
                assert_client_acquires_addr(
                    &netstack_realm,
                    &client,
                    *expected_acquired,
                    dhcp_loop_cycles,
                    expect_client_renews,
                )
                .await;
            }
            for (server, _) in servers {
                let () = server.stop_serving().await.expect("failed to stop server");
            }
        }
    }
}

#[derive(Copy, Clone)]
enum PersistenceMode {
    Persistent,
    Ephemeral,
}

impl std::fmt::Display for PersistenceMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PersistenceMode::Persistent => write!(f, "persistent"),
            PersistenceMode::Ephemeral => write!(f, "ephemeral"),
        }
    }
}

impl PersistenceMode {
    fn dhcpd_params_after_restart(
        &self,
        if_name: String,
    ) -> Result<Vec<(fidl_fuchsia_net_dhcp::ParameterName, fidl_fuchsia_net_dhcp::Parameter)>> {
        Ok(match self {
            Self::Persistent => {
                let params =
                    test_dhcpd_parameters(if_name).context("failed to create test dhcpd params")?;
                params.into_iter().map(|p| (param_name(&p), p)).collect()
            }
            Self::Ephemeral => vec![
                fidl_fuchsia_net_dhcp::Parameter::IpAddrs(vec![]),
                fidl_fuchsia_net_dhcp::Parameter::AddressPool(fidl_fuchsia_net_dhcp::AddressPool {
                    prefix_length: Some(0),
                    range_start: Some(fidl_fuchsia_net::Ipv4Address { addr: [0, 0, 0, 0] }),
                    range_stop: Some(fidl_fuchsia_net::Ipv4Address { addr: [0, 0, 0, 0] }),
                    ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
                }),
                fidl_fuchsia_net_dhcp::Parameter::Lease(fidl_fuchsia_net_dhcp::LeaseLength {
                    default: Some(86400),
                    max: Some(86400),
                    ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
                }),
                fidl_fuchsia_net_dhcp::Parameter::PermittedMacs(vec![]),
                fidl_fuchsia_net_dhcp::Parameter::StaticallyAssignedAddrs(vec![]),
                fidl_fuchsia_net_dhcp::Parameter::ArpProbe(false),
                fidl_fuchsia_net_dhcp::Parameter::BoundDeviceNames(vec![]),
            ]
            .into_iter()
            .map(|p| (param_name(&p), p))
            .collect(),
        })
    }
}

// This collection of parameters is defined as a function because we need to allocate a Vec which
// cannot be done statically, i.e. as a constant.
fn test_dhcpd_parameters(if_name: String) -> Result<Vec<fidl_fuchsia_net_dhcp::Parameter>> {
    Ok(vec![
        fidl_fuchsia_net_dhcp::Parameter::IpAddrs(vec![DEFAULT_TEST_CONFIG.server_addr]),
        fidl_fuchsia_net_dhcp::Parameter::AddressPool(
            DEFAULT_TEST_CONFIG.managed_addrs.into_fidl(),
        ),
        fidl_fuchsia_net_dhcp::Parameter::Lease(fidl_fuchsia_net_dhcp::LeaseLength {
            default: Some(60),
            max: Some(60),
            ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
        }),
        fidl_fuchsia_net_dhcp::Parameter::PermittedMacs(vec![fidl_mac!("aa:bb:cc:dd:ee:ff")]),
        fidl_fuchsia_net_dhcp::Parameter::StaticallyAssignedAddrs(vec![
            fidl_fuchsia_net_dhcp::StaticAssignment {
                host: Some(fidl_mac!("aa:bb:cc:dd:ee:ff")),
                assigned_addr: Some(fidl_ip_v4!("192.168.0.2")),
                ..fidl_fuchsia_net_dhcp::StaticAssignment::EMPTY
            },
        ]),
        fidl_fuchsia_net_dhcp::Parameter::ArpProbe(true),
        fidl_fuchsia_net_dhcp::Parameter::BoundDeviceNames(vec![if_name]),
    ])
}

fn param_name(param: &fidl_fuchsia_net_dhcp::Parameter) -> fidl_fuchsia_net_dhcp::ParameterName {
    match param {
        fidl_fuchsia_net_dhcp::Parameter::IpAddrs(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::IpAddrs
        }
        fidl_fuchsia_net_dhcp::Parameter::AddressPool(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::AddressPool
        }
        fidl_fuchsia_net_dhcp::Parameter::Lease(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::LeaseLength
        }
        fidl_fuchsia_net_dhcp::Parameter::PermittedMacs(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::PermittedMacs
        }
        fidl_fuchsia_net_dhcp::Parameter::StaticallyAssignedAddrs(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::StaticallyAssignedAddrs
        }
        fidl_fuchsia_net_dhcp::Parameter::ArpProbe(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::ArpProbe
        }
        fidl_fuchsia_net_dhcp::Parameter::BoundDeviceNames(_) => {
            fidl_fuchsia_net_dhcp::ParameterName::BoundDeviceNames
        }
        fidl_fuchsia_net_dhcp::ParameterUnknown!() => {
            panic!("attempted to retrieve name of Parameter::Unknown");
        }
    }
}

// This test guards against regression for the issue found in https://fxbug.dev/62989. The test
// attempts to create an inconsistent state on the dhcp server by allowing the server to complete a
// transaction with a client, thereby creating a record of a lease. The server is then restarted;
// if the linked issue has not been fixed, then the server will inadvertently erase its
// configuration parameters from persistent storage, which will lead to an inconsistent server
// state on the next restart.  Finally, the server is restarted one more time, and then its
// clear_leases() function is triggered, which will cause a panic if the server is in an
// inconsistent state.
#[variants_test]
async fn acquire_persistent_dhcp_server_after_restart<E: netemul::Endpoint>(name: &str) -> Result {
    let mode = PersistenceMode::Persistent;
    Ok(acquire_dhcp_server_after_restart::<E>(&format!("{}_{}", name, mode), mode).await?)
}

// An ephemeral dhcp server cannot become inconsistent with its persistent state because it has
// none.  However, without persistent state, an ephemeral dhcp server cannot run without explicit
// configuration.  This test verifies that an ephemeral dhcp server will return an error if run
// after restarting.
#[variants_test]
async fn acquire_ephemeral_dhcp_server_after_restart<E: netemul::Endpoint>(name: &str) -> Result {
    let mode = PersistenceMode::Ephemeral;
    Ok(acquire_dhcp_server_after_restart::<E>(&format!("{}_{}", name, mode), mode).await?)
}

async fn acquire_dhcp_server_after_restart<E: netemul::Endpoint>(
    name: &str,
    mode: PersistenceMode,
) -> Result {
    let sandbox = netemul::TestSandbox::new().context("failed to create sandbox")?;

    let server_realm = sandbox
        .create_netstack_realm_with::<Netstack2, _, _>(
            format!("{}_server", name),
            &[
                match mode {
                    PersistenceMode::Ephemeral => {
                        KnownServiceProvider::DhcpServer { persistent: false }
                    }
                    PersistenceMode::Persistent => {
                        KnownServiceProvider::DhcpServer { persistent: true }
                    }
                },
                KnownServiceProvider::DnsResolver,
                KnownServiceProvider::SecureStash,
            ],
        )
        .context("failed to create server Realm")?;

    let client_realm = sandbox
        .create_netstack_realm::<Netstack2, _>(format!("{}_client", name))
        .context("failed to create client Realm")?;

    let network = sandbox.create_network(name).await.context("failed to create network")?;
    let if_name = "testeth";
    let endpoint =
        network.create_endpoint::<E, _>("server-ep").await.context("failed to create endpoint")?;
    let _server_ep = server_realm
        .install_endpoint(
            endpoint,
            &netemul::InterfaceConfig::StaticIp(DEFAULT_TEST_CONFIG.server_subnet()),
            Some(if_name.to_string()),
        )
        .await
        .context("failed to create server network endpoint")?;
    let client_ep = client_realm
        .join_network::<E, _>(&network, "client-ep", &netemul::InterfaceConfig::None)
        .await
        .context("failed to create client network endpoint")?;

    // Complete initial DHCP transaction in order to store a lease record in the server's
    // persistent storage.
    {
        let dhcp_server =
            server_realm.connect_to_protocol::<fidl_fuchsia_net_dhcp::Server_Marker>()?;
        let mut parameters = DEFAULT_TEST_CONFIG.dhcp_parameters();
        parameters
            .push(fidl_fuchsia_net_dhcp::Parameter::BoundDeviceNames(vec![if_name.to_string()]));
        let () = set_server_settings(&dhcp_server, &mut parameters, &mut []).await?;
        let () = dhcp_server
            .start_serving()
            .await
            .context("failed to call dhcp/Server.StartServing")?
            .map_err(fuchsia_zircon::Status::from_raw)
            .context("dhcp/Server.StartServing returned error")?;
        let () = assert_client_acquires_addr(
            &client_realm,
            &client_ep,
            DEFAULT_TEST_CONFIG.expected_acquired(),
            1,
            false,
        )
        .await;
        let () =
            dhcp_server.stop_serving().await.context("failed to call dhcp/Server.StopServing")?;
        let () = server_realm
            .stop_child_component(constants::dhcp_server::COMPONENT_NAME)
            .await
            .context("failed to stop dhcpd")?;
    }

    // Restart the server in an attempt to force the server's persistent storage into an
    // inconsistent state whereby the addresses leased to clients do not agree with the contents of
    // the server's address pool. If the server is in ephemeral mode, it will fail at the call to
    // start_serving() since it will not have retained its parameters.
    {
        let dhcp_server =
            server_realm.connect_to_protocol::<fidl_fuchsia_net_dhcp::Server_Marker>()?;
        let () = match mode {
            PersistenceMode::Persistent => {
                let () = dhcp_server
                    .start_serving()
                    .await
                    .context("failed to call dhcp/Server.StartServing")?
                    .map_err(fuchsia_zircon::Status::from_raw)
                    .context("dhcp/Server.StartServing returned error")?;
                dhcp_server
                    .stop_serving()
                    .await
                    .context("failed to call dhcp/Server.StopServing")?
            }
            PersistenceMode::Ephemeral => {
                matches::assert_matches!(
                    dhcp_server
                        .start_serving()
                        .await
                        .context("failed to call dhcp/Server.StartServing")?
                        .map_err(fuchsia_zircon::Status::from_raw),
                    Err(fuchsia_zircon::Status::INVALID_ARGS)
                );
            }
        };
        let () = server_realm
            .stop_child_component(constants::dhcp_server::COMPONENT_NAME)
            .await
            .context("failed to stop dhcpd")?;
    }

    // Restart the server again in order to load the inconsistent state into the server's runtime
    // representation. Call clear_leases() to trigger a panic resulting from inconsistent state,
    // provided that the issue motivating this test is unfixed/regressed. If the server is in
    // ephemeral mode, it will fail at the call to start_serving() since it will not have retained
    // its parameters.
    {
        let dhcp_server =
            server_realm.connect_to_protocol::<fidl_fuchsia_net_dhcp::Server_Marker>()?;
        let () = match mode {
            PersistenceMode::Persistent => {
                let () = dhcp_server
                    .start_serving()
                    .await
                    .context("failed to call dhcp/Server.StartServing")?
                    .map_err(fuchsia_zircon::Status::from_raw)
                    .context("dhcp/Server.StartServing returned error")?;
                let () = dhcp_server
                    .stop_serving()
                    .await
                    .context("failed to call dhcp/Server.StopServing")?;
                dhcp_server
                    .clear_leases()
                    .await
                    .context("failed to call dhcp/Server.ClearLeases")?
                    .map_err(fuchsia_zircon::Status::from_raw)
                    .context("dhcp/Server.ClearLeases returned error")?;
            }
            PersistenceMode::Ephemeral => {
                matches::assert_matches!(
                    dhcp_server
                        .start_serving()
                        .await
                        .context("failed to call dhcp/Server.StartServing")?
                        .map_err(fuchsia_zircon::Status::from_raw),
                    Err(fuchsia_zircon::Status::INVALID_ARGS)
                );
            }
        };
        let () = server_realm
            .stop_child_component(constants::dhcp_server::COMPONENT_NAME)
            .await
            .context("failed to stop dhcpd")?;
    }

    Ok(())
}

#[variants_test]
async fn test_dhcp_server_persistence_mode_persistent<E: netemul::Endpoint>(name: &str) -> Result {
    let mode = PersistenceMode::Persistent;
    Ok(test_dhcp_server_persistence_mode::<E>(&format!("{}_{}", name, mode), mode).await?)
}

#[variants_test]
async fn test_dhcp_server_persistence_mode_ephemeral<E: netemul::Endpoint>(name: &str) -> Result {
    let mode = PersistenceMode::Ephemeral;
    Ok(test_dhcp_server_persistence_mode::<E>(&format!("{}_{}", name, mode), mode).await?)
}

async fn test_dhcp_server_persistence_mode<E: netemul::Endpoint>(
    name: &str,
    mode: PersistenceMode,
) -> Result {
    let sandbox = netemul::TestSandbox::new().context("failed to create sandbox")?;

    let server_realm = sandbox
        .create_netstack_realm_with::<Netstack2, _, _>(
            format!("{}_server", name),
            &[
                match mode {
                    PersistenceMode::Ephemeral => {
                        KnownServiceProvider::DhcpServer { persistent: false }
                    }
                    PersistenceMode::Persistent => {
                        KnownServiceProvider::DhcpServer { persistent: true }
                    }
                },
                KnownServiceProvider::DnsResolver,
                KnownServiceProvider::SecureStash,
            ],
        )
        .context("failed to create server Realm")?;

    let network = sandbox.create_network(name).await.context("failed to create network")?;
    let if_name = "testeth";
    let endpoint =
        network.create_endpoint::<E, _>("server-ep").await.context("failed to create endpoint")?;
    let _server_ep = server_realm
        .install_endpoint(
            endpoint,
            &netemul::InterfaceConfig::StaticIp(DEFAULT_TEST_CONFIG.server_subnet()),
            Some(if_name.to_string()),
        )
        .await
        .context("failed to create server network endpoint")?;

    // Configure the server with parameters and then restart it.
    {
        let mut settings = test_dhcpd_parameters(if_name.to_string())
            .context("failed to create test dhcpd params")?;
        let dhcp_server =
            server_realm.connect_to_protocol::<fidl_fuchsia_net_dhcp::Server_Marker>()?;
        let () = set_server_settings(&dhcp_server, &mut settings, &mut []).await?;
        let () = server_realm
            .stop_child_component(constants::dhcp_server::COMPONENT_NAME)
            .await
            .context("failed to stop dhcpd")?;
    }

    // Assert that configured parameters after the restart correspond to the persistence mode of the
    // server.
    {
        let dhcp_server =
            server_realm.connect_to_protocol::<fidl_fuchsia_net_dhcp::Server_Marker>()?;
        let dhcp_server = &dhcp_server;
        let mut params = mode
            .dhcpd_params_after_restart(if_name.to_string())
            .context("failed to create dhcpd params after restart.")?;
        let () = stream::iter(params.iter_mut())
            .map(Ok)
            .try_for_each_concurrent(None, |(name, parameter)| async move {
                Result::Ok(assert_eq!(
                    dhcp_server
                        .get_parameter(*name)
                        .await
                        .with_context(|| {
                            format!("failed to call dhcp/Server.GetParameter({:?})", name)
                        })?
                        .map_err(fuchsia_zircon::Status::from_raw)
                        .with_context(|| {
                            format!("dhcp/Server.GetParameter({:?}) returned error", name)
                        })
                        .unwrap(),
                    *parameter
                ))
            })
            .await
            .context("failed to get server parameters")?;
        let () = server_realm
            .stop_child_component(constants::dhcp_server::COMPONENT_NAME)
            .await
            .context("failed to stop dhcpd")?;
    }
    Ok(())
}
