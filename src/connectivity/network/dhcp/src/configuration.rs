// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::protocol::{FidlCompatible, FromFidlExt, IntoFidlExt};
use anyhow::Context;
use serde::{Deserialize, Serialize};
use serde_json;
use std::collections::HashMap;
use std::convert::{Infallible as Never, TryFrom, TryInto};
use std::io;
use std::net::Ipv4Addr;
use std::num::TryFromIntError;
use thiserror::Error;

/// A collection of the basic configuration parameters needed by the server.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct ServerParameters {
    /// The IPv4 addresses of the host running the server.
    pub server_ips: Vec<Ipv4Addr>,
    /// The duration for which leases should be assigned to clients
    pub lease_length: LeaseLength,
    /// The IPv4 addresses which the server is responsible for managing and leasing to
    /// clients.
    pub managed_addrs: ManagedAddresses,
    /// A list of MAC addresses which are permitted to request a lease. If empty, any MAC address
    /// may request a lease.
    pub permitted_macs: PermittedMacs,
    /// A collection of static address assignments. Any client whose MAC address has a static
    /// assignment will be offered the assigned IP address.
    pub static_assignments: StaticAssignments,
    /// Enables server behavior where the server ARPs an IP address prior to issuing
    /// it in a lease.
    pub arp_probe: bool,
    /// The interface names to which the server's UDP sockets are bound. If
    /// this vector is empty, the server will not bind to a specific interface
    /// and will process incoming DHCP messages regardless of the interface on
    /// which they arrive.
    pub bound_device_names: Vec<String>,
}

impl ServerParameters {
    pub fn is_valid(&self) -> bool {
        let Self {
            server_ips,
            lease_length: crate::configuration::LeaseLength { default_seconds, max_seconds },
            managed_addrs:
                crate::configuration::ManagedAddresses { mask: _, pool_range_start, pool_range_stop },
            permitted_macs: _,
            static_assignments: _,
            arp_probe: _,
            bound_device_names: _,
        } = self;
        if server_ips.is_empty() {
            return false;
        }
        if std::array::IntoIter::new([pool_range_start, pool_range_stop])
            .chain(server_ips.iter())
            .any(std::net::Ipv4Addr::is_unspecified)
        {
            return false;
        }
        if *default_seconds == 0 {
            return false;
        }
        if *max_seconds == 0 {
            return false;
        }
        true
    }
}

/// Parameters controlling lease duration allocation. Per,
/// https://tools.ietf.org/html/rfc2131#section-3.3, times are represented as relative times.
#[derive(Clone, Debug, PartialEq, Deserialize, Serialize)]
pub struct LeaseLength {
    /// The default lease duration assigned by the server.
    pub default_seconds: u32,
    /// The maximum allowable lease duration which a client can request.
    pub max_seconds: u32,
}

impl FidlCompatible<fidl_fuchsia_net_dhcp::LeaseLength> for LeaseLength {
    type FromError = anyhow::Error;
    type IntoError = Never;

    fn try_from_fidl(fidl: fidl_fuchsia_net_dhcp::LeaseLength) -> Result<Self, Self::FromError> {
        if let fidl_fuchsia_net_dhcp::LeaseLength { default: Some(default_seconds), max, .. } = fidl
        {
            Ok(LeaseLength {
                default_seconds,
                // Per fuchsia.net.dhcp, if omitted, max defaults to the value of default.
                max_seconds: max.unwrap_or(default_seconds),
            })
        } else {
            Err(anyhow::format_err!(
                "fuchsia.net.dhcp.LeaseLength missing required field: {:?}",
                fidl
            ))
        }
    }

    fn try_into_fidl(self) -> Result<fidl_fuchsia_net_dhcp::LeaseLength, Self::IntoError> {
        let LeaseLength { default_seconds, max_seconds } = self;
        Ok(fidl_fuchsia_net_dhcp::LeaseLength {
            default: Some(default_seconds),
            max: Some(max_seconds),
            ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
        })
    }
}

/// The IP addresses which the server will manage and lease to clients.
#[derive(Copy, Clone, Debug, PartialEq, Deserialize, Serialize)]
pub struct ManagedAddresses {
    /// The subnet mask of the subnet for which the server will manage addresses.
    pub mask: SubnetMask,
    /// The inclusive starting address of the range of managed addresses.
    pub pool_range_start: Ipv4Addr,
    /// The exclusive stopping address of the range of managed addresses.
    pub pool_range_stop: Ipv4Addr,
}

impl ManagedAddresses {
    fn pool_range_inner(&self) -> std::ops::Range<u32> {
        let Self { mask: _, pool_range_start, pool_range_stop } = *self;
        pool_range_start.into()..pool_range_stop.into()
    }
    /// Returns an iterator of the `Ipv4Addr`s from `pool_range_start`, inclusive, to
    /// `pool_range_stop`, exclusive.
    pub fn pool_range(&self) -> impl Iterator<Item = Ipv4Addr> {
        self.pool_range_inner().map(Into::into)
    }

    /// Returns the number of `Ipv4Addr`s from `pool_range_start`, inclusive, to
    /// `pool_range_stop`, exclusive.
    pub fn pool_range_size(&self) -> Result<u32, TryFromIntError> {
        self.pool_range_inner().len().try_into()
    }
}

impl FidlCompatible<fidl_fuchsia_net_dhcp::AddressPool> for ManagedAddresses {
    type FromError = anyhow::Error;
    type IntoError = Never;

    fn try_from_fidl(fidl: fidl_fuchsia_net_dhcp::AddressPool) -> Result<Self, Self::FromError> {
        if let fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: Some(prefix_length),
            range_start: Some(pool_range_start),
            range_stop: Some(pool_range_stop),
            ..
        } = fidl
        {
            let mask = SubnetMask::try_from(prefix_length).with_context(|| {
                format!("failed to create subnet mask from prefix_length={}", prefix_length)
            })?;
            let pool_range_start = Ipv4Addr::from_fidl(pool_range_start);
            let pool_range_stop = Ipv4Addr::from_fidl(pool_range_stop);
            let addresses_candidate = Self { mask, pool_range_start, pool_range_stop };
            if pool_range_start > pool_range_stop {
                return Err(anyhow::format_err!(
                    "fuchsia.net.dhcp.AddressPool contained range_start ({}) > range_stop ({})",
                    pool_range_start,
                    pool_range_stop
                ));
            }
            let pool_range_size = addresses_candidate.pool_range_size().with_context(|| {
                format!("failed to determine address pool size for range_start ({}) and range_stop ({})", pool_range_start, pool_range_stop)
            })?;
            if pool_range_size > mask.subnet_size() {
                Err(anyhow::format_err!("fuchsia.net.dhcp.AddressPool contained prefix_length ({}) which cannot fit address pool defined by range_start: ({}) and range_stop: ({})", prefix_length, pool_range_start, pool_range_stop))
            } else {
                Ok(addresses_candidate)
            }
        } else {
            Err(anyhow::format_err!("fuchsia.net.dhcp.AddressPool missing fields: {:?}", fidl))
        }
    }

    fn try_into_fidl(self) -> Result<fidl_fuchsia_net_dhcp::AddressPool, Self::IntoError> {
        let ManagedAddresses { mask, pool_range_start, pool_range_stop } = self;
        Ok(fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: Some(mask.ones()),
            range_start: Some(pool_range_start.into_fidl()),
            range_stop: Some(pool_range_stop.into_fidl()),
            ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
        })
    }
}

/// A list of MAC addresses which are permitted to request a lease.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PermittedMacs(pub Vec<fidl_fuchsia_net_ext::MacAddress>);

impl FidlCompatible<Vec<fidl_fuchsia_net::MacAddress>> for PermittedMacs {
    type FromError = Never;
    type IntoError = Never;

    fn try_from_fidl(fidl: Vec<fidl_fuchsia_net::MacAddress>) -> Result<Self, Self::FromError> {
        Ok(PermittedMacs(fidl.into_iter().map(|mac| mac.into()).collect()))
    }

    fn try_into_fidl(self) -> Result<Vec<fidl_fuchsia_net::MacAddress>, Self::IntoError> {
        Ok(self.0.into_iter().map(|mac| mac.into()).collect())
    }
}

/// A collection of static address assignments. Any client whose MAC address has a static
/// assignment will be offered the assigned IP address.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct StaticAssignments(pub HashMap<fidl_fuchsia_net_ext::MacAddress, Ipv4Addr>);

impl FidlCompatible<Vec<fidl_fuchsia_net_dhcp::StaticAssignment>> for StaticAssignments {
    type FromError = anyhow::Error;
    type IntoError = Never;

    fn try_from_fidl(
        fidl: Vec<fidl_fuchsia_net_dhcp::StaticAssignment>,
    ) -> Result<Self, Self::FromError> {
        match fidl.into_iter().try_fold(HashMap::new(), |mut acc, assignment| {
            if let (Some(host), Some(assigned_addr)) = (assignment.host, assignment.assigned_addr) {
                let mac = fidl_fuchsia_net_ext::MacAddress::from(host);
                match acc.insert(mac, Ipv4Addr::from_fidl(assigned_addr)) {
                    Some(_ip) => Err(anyhow::format_err!(
                        "fuchsia.net.dhcp.StaticAssignment contained multiple entries for {}",
                        mac
                    )),
                    None => Ok(acc),
                }
            } else {
                Err(anyhow::format_err!(
                    "fuchsia.net.dhcp.StaticAssignment contained entry with missing fields: {:?}",
                    assignment
                ))
            }
        }) {
            Ok(static_assignments) => Ok(StaticAssignments(static_assignments)),
            Err(e) => Err(e),
        }
    }

    fn try_into_fidl(
        self,
    ) -> Result<Vec<fidl_fuchsia_net_dhcp::StaticAssignment>, Self::IntoError> {
        Ok(self
            .0
            .into_iter()
            .map(|(host, assigned_addr)| fidl_fuchsia_net_dhcp::StaticAssignment {
                host: Some(host.into()),
                assigned_addr: Some(assigned_addr.into_fidl()),
                ..fidl_fuchsia_net_dhcp::StaticAssignment::EMPTY
            })
            .collect())
    }
}

/// A wrapper around the error types which can be returned when loading a
/// `ServerConfig` from file with `load_server_config_from_file()`.
#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("io error: {}", _0)]
    IoError(io::Error),
    #[error("json deserialization error: {}", _0)]
    JsonError(serde_json::Error),
}

impl From<io::Error> for ConfigError {
    fn from(e: io::Error) -> Self {
        ConfigError::IoError(e)
    }
}

impl From<serde_json::Error> for ConfigError {
    fn from(e: serde_json::Error) -> Self {
        ConfigError::JsonError(e)
    }
}

const U32_BITS: u8 = (std::mem::size_of::<u32>() * 8) as u8;

/// A bitmask which represents the boundary between the Network part and Host part of an IPv4
/// address.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
pub struct SubnetMask {
    /// The set high-order bits of the mask.
    ones: u8,
}

impl SubnetMask {
    /// Returns a `SubnetMask` without checking the value.
    ///
    /// # Safety
    ///
    /// The value must be <= 32 (the size of an IPv4 address in bits).
    pub const unsafe fn new_unchecked(ones: u8) -> Self {
        SubnetMask { ones }
    }

    /// Returns a byte-array representation of the `SubnetMask` in Network (Big-Endian) byte-order.
    pub fn octets(&self) -> [u8; 4] {
        self.to_u32().to_be_bytes()
    }

    fn to_u32(&self) -> u32 {
        let n = u32::max_value();
        // overflowing_shl() will not shift arguments >= the bit length of the calling value, so we
        // must special case a mask with 0 ones.
        if self.ones == 0 {
            0
        } else {
            // overflowing_shl() must be used here for panic safety.
            let (n, _overflow) = n.overflowing_shl((U32_BITS - self.ones) as u32);
            n
        }
    }

    /// Returns the count of the set high-order bits of the `SubnetMask`.
    pub fn ones(&self) -> u8 {
        self.ones
    }

    /// Returns the network address resulting from masking the argument.
    pub fn apply_to(&self, target: &Ipv4Addr) -> Ipv4Addr {
        let subnet_mask_bits = self.to_u32();
        let target_bits = u32::from_be_bytes(target.octets());
        Ipv4Addr::from(target_bits & subnet_mask_bits)
    }

    /// Computes the broadcast address for the argument.
    pub fn broadcast_of(&self, target: &Ipv4Addr) -> Ipv4Addr {
        let subnet_mask_bits = self.to_u32();
        let target_bits = u32::from_be_bytes(target.octets());
        Ipv4Addr::from(!subnet_mask_bits | target_bits)
    }

    /// Returns the size of the subnet defined by this mask.
    pub fn subnet_size(&self) -> u32 {
        !self.to_u32()
    }
}

impl TryFrom<u8> for SubnetMask {
    type Error = anyhow::Error;

    /// Returns a `Ok(SubnetMask)` with the `ones` high-order bits set if `ones` < 32, else `Err`.
    fn try_from(ones: u8) -> Result<Self, Self::Error> {
        if ones > U32_BITS {
            Err(anyhow::format_err!(
                "failed precondition: argument must be <= 32 (bit length of an IPv4 address)",
            ))
        } else {
            Ok(SubnetMask { ones })
        }
    }
}

impl TryFrom<Ipv4Addr> for SubnetMask {
    type Error = anyhow::Error;

    fn try_from(mask: Ipv4Addr) -> Result<Self, Self::Error> {
        let mask: u32 = mask.into();
        let ones = mask.count_ones();
        if ones + mask.trailing_zeros() != 32 {
            Err(anyhow::format_err!("mask did not have consecutive ones {:b}", mask))
        } else {
            Ok(SubnetMask { ones: ones as u8 })
        }
    }
}

impl FidlCompatible<fidl_fuchsia_net::Ipv4Address> for SubnetMask {
    type FromError = anyhow::Error;
    type IntoError = Never;

    fn try_from_fidl(fidl: fidl_fuchsia_net::Ipv4Address) -> Result<Self, Self::FromError> {
        let addr = Ipv4Addr::from_fidl(fidl);
        SubnetMask::try_from(addr)
    }

    fn try_into_fidl(self) -> Result<fidl_fuchsia_net::Ipv4Address, Self::IntoError> {
        let addr = Ipv4Addr::from(self.to_u32());
        Ok(addr.into_fidl())
    }
}

impl Into<Ipv4Addr> for SubnetMask {
    fn into(self) -> Ipv4Addr {
        Ipv4Addr::from(self.to_u32())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::server::tests::{random_ipv4_generator, random_mac_generator};
    use net_declare::{fidl_ip_v4, std_ip_v4};

    /// Asserts that the supplied Result is an err whose error string contains `substr`.
    ///
    /// We expect that the contained error implements Display, so that we can extract
    /// that error string.
    #[macro_export]
    macro_rules! assert_err_with_substring {
        ($result:expr, $substr:expr) => {{
            match $result {
                Err(e) => {
                    let err_str = e.to_string();
                    assert!(err_str.contains($substr), "{} not in {}", $substr, err_str)
                }
                Ok(v) => panic!(
                    "{} (Ok({:?})) is not an Err containing {} ({})",
                    stringify!($result),
                    v,
                    stringify!($substr),
                    $substr
                ),
            }
        }};
    }

    #[test]
    fn subnet_mask_new_unchecked() {
        for ones in 0..=U32_BITS {
            assert_eq!(
                unsafe { SubnetMask::new_unchecked(ones) },
                SubnetMask::try_from(ones).expect("expected a valid subnet mask")
            );
        }
    }

    #[test]
    fn test_try_from_ipv4addr_with_consecutive_ones_returns_mask() {
        assert_eq!(
            SubnetMask::try_from(std_ip_v4!("255.255.255.0"))
                .expect("failed to create /24 subnet mask"),
            SubnetMask { ones: 24 }
        );
        assert_eq!(
            SubnetMask::try_from(std_ip_v4!("255.255.255.255"))
                .expect("failed to create /32 subnet mask"),
            SubnetMask { ones: 32 }
        );
    }

    #[test]
    fn test_try_from_ipv4addr_with_nonconsecutive_ones_returns_err() {
        assert!(SubnetMask::try_from(std_ip_v4!("255.255.255.1")).is_err());
    }

    #[test]
    fn subnet_mask_try_from_u8() {
        for ones in 0..=U32_BITS {
            assert_eq!(
                SubnetMask::try_from(ones).expect("expected valid subnet mask"),
                SubnetMask { ones }
            );
        }

        let _: anyhow::Error =
            SubnetMask::try_from(U32_BITS + 1).expect_err("expected invalid subnet mask");
    }

    #[test]
    fn test_into_ipv4addr_returns_ipv4addr() {
        let mask: Ipv4Addr =
            SubnetMask::try_from(24).expect("expected 24 to be a valid subnet mask").into();
        assert_eq!(mask, std_ip_v4!("255.255.255.0"));

        let mask: Ipv4Addr =
            SubnetMask::try_from(0).expect("expected 0 to be a valid subnet mask").into();
        assert_eq!(mask, Ipv4Addr::UNSPECIFIED);

        let mask: Ipv4Addr =
            SubnetMask::try_from(32).expect("expected 32 to be a valid subnet mask").into();
        assert_eq!(mask, std_ip_v4!("255.255.255.255"));
    }

    #[test]
    fn test_lease_length_try_from_fidl() {
        let both = fidl_fuchsia_net_dhcp::LeaseLength {
            default: Some(42),
            max: Some(42),
            ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
        };
        let with_default = fidl_fuchsia_net_dhcp::LeaseLength {
            default: Some(42),
            max: None,
            ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
        };
        let with_max = fidl_fuchsia_net_dhcp::LeaseLength {
            default: None,
            max: Some(42),
            ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
        };
        let neither = fidl_fuchsia_net_dhcp::LeaseLength {
            default: None,
            max: None,
            ..fidl_fuchsia_net_dhcp::LeaseLength::EMPTY
        };

        assert_eq!(
            LeaseLength::try_from_fidl(both).unwrap(),
            LeaseLength { default_seconds: 42, max_seconds: 42 }
        );
        assert_eq!(
            LeaseLength::try_from_fidl(with_default).unwrap(),
            LeaseLength { default_seconds: 42, max_seconds: 42 }
        );
        assert!(LeaseLength::try_from_fidl(with_max).is_err());
        assert!(LeaseLength::try_from_fidl(neither).is_err());
    }

    #[test]
    fn test_managed_addresses_try_from_fidl() {
        let prefix_length = 24;
        let start_addr = fidl_ip_v4!("192.168.0.2");
        let stop_addr = fidl_ip_v4!("192.168.0.254");
        let correct_pool = fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: Some(prefix_length),
            range_start: Some(start_addr),
            range_stop: Some(stop_addr),
            ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
        };

        matches::assert_matches!(
            ManagedAddresses::try_from_fidl(correct_pool),
            Ok(ManagedAddresses {
                mask,
                pool_range_start,
                pool_range_stop,
            }) if mask.ones() == prefix_length && pool_range_start.into_fidl() == start_addr && pool_range_stop.into_fidl() == stop_addr
        );

        let bad_prefix_length_pool = fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: Some(33),
            range_start: Some(fidl_ip_v4!("192.168.0.2")),
            range_stop: Some(fidl_ip_v4!("192.168.0.254")),
            ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
        };

        assert_err_with_substring!(
            ManagedAddresses::try_from_fidl(bad_prefix_length_pool),
            "from prefix_length"
        );

        let missing_fields_pool = fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: None,
            range_start: Some(fidl_ip_v4!("192.168.0.2")),
            range_stop: Some(fidl_ip_v4!("192.168.0.254")),
            ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
        };

        assert_err_with_substring!(
            ManagedAddresses::try_from_fidl(missing_fields_pool),
            "missing fields"
        );

        let start_after_stop_pool = fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: Some(24),
            range_start: Some(fidl_ip_v4!("192.168.0.20")),
            range_stop: Some(fidl_ip_v4!("192.168.0.10")),
            ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
        };

        assert_err_with_substring!(
            ManagedAddresses::try_from_fidl(start_after_stop_pool),
            "> range_stop"
        );

        let mask_range_too_small_pool = fidl_fuchsia_net_dhcp::AddressPool {
            prefix_length: Some(24),
            range_start: Some(fidl_ip_v4!("192.168.0.0")),
            range_stop: Some(fidl_ip_v4!("192.168.1.0")),
            ..fidl_fuchsia_net_dhcp::AddressPool::EMPTY
        };

        assert_err_with_substring!(
            ManagedAddresses::try_from_fidl(mask_range_too_small_pool),
            "cannot fit address pool"
        );
    }

    #[test]
    fn test_static_assignments_try_from_fidl() {
        use std::iter::FromIterator;

        let mac = random_mac_generator().bytes();
        let ip = random_ipv4_generator();
        let fields_present = vec![fidl_fuchsia_net_dhcp::StaticAssignment {
            host: Some(fidl_fuchsia_net::MacAddress { octets: mac.clone() }),
            assigned_addr: Some(ip.into_fidl()),
            ..fidl_fuchsia_net_dhcp::StaticAssignment::EMPTY
        }];
        let multiple_entries = vec![
            fidl_fuchsia_net_dhcp::StaticAssignment {
                host: Some(fidl_fuchsia_net::MacAddress { octets: mac.clone() }),
                assigned_addr: Some(ip.into_fidl()),
                ..fidl_fuchsia_net_dhcp::StaticAssignment::EMPTY
            },
            fidl_fuchsia_net_dhcp::StaticAssignment {
                host: Some(fidl_fuchsia_net::MacAddress { octets: mac.clone() }),
                assigned_addr: Some(random_ipv4_generator().into_fidl()),
                ..fidl_fuchsia_net_dhcp::StaticAssignment::EMPTY
            },
        ];
        let fields_missing = vec![fidl_fuchsia_net_dhcp::StaticAssignment {
            host: None,
            assigned_addr: None,
            ..fidl_fuchsia_net_dhcp::StaticAssignment::EMPTY
        }];

        assert_eq!(
            StaticAssignments::try_from_fidl(fields_present).unwrap(),
            StaticAssignments(HashMap::from_iter(
                vec![(fidl_fuchsia_net_ext::MacAddress { octets: mac }, ip)].into_iter()
            ))
        );
        assert!(StaticAssignments::try_from_fidl(multiple_entries).is_err());
        assert!(StaticAssignments::try_from_fidl(fields_missing).is_err());
    }
}
