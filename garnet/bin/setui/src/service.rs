// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Service-wide MessageHub definition.
//!
//! The service mod defines a MessageHub (and associated Address, Payload, and
//! Role namespaces) to facilitate service-wide communication. All
//! communication, both intra-component and inter-component, should be sent
//! through this hub. The address space of this MessageHub allows any component
//! to be reached when given a public address. The Role space allows granular
//! message filtering and audience targeting.
//!
//! The static Address and Role definitions below provide a way to reference
//! such values at build time. However, many use-cases that rely on these
//! features can be done through values generated at runtime instead. Care
//! should be taken before expanding either enumeration.
//!
//! Currently, service communication happens in a number of domain-specific
//! message hubs located in the internal mod. Communication from these hubs
//! should migrate here over time.

use crate::base::SettingType;
use crate::handler::base as handler;
use crate::handler::setting_handler as controller;
use crate::message_hub_definition;
use crate::policy::base as policy;
use crate::policy::base::PolicyType;

message_hub_definition!(Payload, Address, Role);

/// The `Address` enumeration defines a namespace for entities that can be
/// reached by a predefined name. Care should be taken when adding new child
/// namespaces here. Each address can only belong to a single entity.
/// Most communication can be instead facilitated with a messenger's signature,
/// which is available at messenger creation time.
#[derive(PartialEq, Copy, Clone, Debug, Eq, Hash)]
pub enum Address {
    Handler(SettingType),
    PolicyHandler(PolicyType),
}

/// The types of data that can be sent through the service [`MessageHub`]. This
/// enumeration is meant to provide a top level definition. Further definitions
/// for particular domains should be located in the appropriate mod.
#[derive(Clone, PartialEq, Debug)]
pub enum Payload {
    /// The Setting type captures communication pertaining to settings,
    /// including requests to access/change settings and the associated
    /// responses.
    Setting(handler::Payload),
    /// The communication to and from a controller to handle requests and
    /// lifetime.
    Controller(controller::Payload),
    /// Policy payloads contain communication to and from policy handlers, including general
    /// requests and responses served by any policy as well as domain-specific requests and
    /// responses defined for individual policies.
    Policy(policy::Payload),
}

/// A trait implemented by payloads for extracting the payload and associated
/// [`message::MessageClient`] from a [`crate::message::base::MesageEvent`].
pub trait TryFromWithClient<T>: Sized {
    type Error;

    fn try_from_with_client(value: T) -> Result<(Self, message::MessageClient), Self::Error>;
}

/// `Role` defines grouping for responsibilities within the service. Roles allow
/// for addressing a particular audience space. Similar to the other
/// enumerations, details about each role should be defined near to the domain.
#[derive(PartialEq, Copy, Clone, Debug, Eq, Hash)]
pub enum Role {}

/// The payload_convert macro helps convert between the domain-specific payloads
/// (variants of [`Payload`]) and the [`Payload`] container(to/from) & MessageHub
/// MessageEvent (from). The first matcher is the [`Payload`] variant name where
/// the payload type can be found. Note that this is the direct variant name
/// and not fully qualified. The second matcher is the domain-specific payload
/// type.
///
/// The macro implements the following in a mod called convert:
/// - Into from domain Payload to service MessageHub Payload
/// - TryFrom from service MessageHub Payload to domain Payload
/// - TryFromWithClient from service MessageHub MessageEvent to domain Payload
///   and client.
/// - TryFromWithClient from a service MessageHub MessageEvent option to domain
///   Payload and client.
#[macro_export]
macro_rules! payload_convert {
    ($service_payload_type:ident, $payload_type:ty) => {
        pub mod convert {
            use super::*;
            use crate::service;
            use crate::service::TryFromWithClient;

            use std::convert::TryFrom;

            impl Into<service::Payload> for $payload_type {
                fn into(self) -> service::Payload {
                    paste::paste! {
                        service::Payload::[<$service_payload_type>](self)
                    }
                }
            }

            impl TryFrom<service::Payload> for $payload_type {
                type Error = String;

                fn try_from(value: service::Payload) -> Result<Self, Self::Error> {
                    paste::paste! {
                        match value {
                            service::Payload::[<$service_payload_type>](payload) => Ok(payload),
                            _=> Err(format!("unexpected payload encountered: {:?}", value)),
                        }
                    }
                }
            }

            impl TryFrom<service::message::MessageEvent> for $payload_type {
                type Error = String;

                fn try_from(value: service::message::MessageEvent) -> Result<Self, Self::Error> {
                    paste::paste! {
                        if let service::message::MessageEvent::Message(payload, _) = value {
                            Payload::try_from(payload)
                        } else {
                            Err(String::from("wrong message type"))
                        }
                    }
                }
            }

            impl TryFromWithClient<service::message::MessageEvent> for $payload_type {
                type Error = String;

                fn try_from_with_client(
                    value: service::message::MessageEvent,
                ) -> Result<(Self, service::message::MessageClient), Self::Error> {
                    if let service::message::MessageEvent::Message(payload, client) = value {
                        Ok((Payload::try_from(payload)?, client))
                    } else {
                        Err(String::from("wrong message type"))
                    }
                }
            }
        }
    };
}
