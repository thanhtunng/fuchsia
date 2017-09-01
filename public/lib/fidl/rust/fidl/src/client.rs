// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! An implementation of a client for a fidl interface.

use {EncodeBuf, DecodeBuf, MsgType, Error};
use cookiemap::CookieMap;

use tokio_core::reactor::Handle;
use tokio_fuchsia;
use futures::{Async, Future, Poll, future};
use std::collections::btree_map::Entry;
use std::io;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicUsize, Ordering};

/// A shared client channel which tracks expected and received responses
struct ClientInner {
    channel: tokio_fuchsia::Channel,

    /// The number of `Some` entries in `message_interests`.
    /// This is used to prevent unnecessary locking.
    received_messages_count: AtomicUsize,

    /// A map of message interests to either `None` (no message received yet)
    /// or `Some(DecodeBuf)` when a message has been received.
    /// An interest is registered with `register_msg_interest` and deregistered
    /// by either receiving a message via a call to `poll_recv` or manually
    /// deregistering with `deregister_msg_interest`
    message_interests: Mutex<CookieMap<Option<DecodeBuf>>>,
}

impl ClientInner {
    /// Registers interest in a response message.
    ///
    /// This function returns a `u64` ID which should be used to send a message
    /// via the channel. Responses are then received using `poll_recv`.
    fn register_msg_interest(&self) -> u64 {
        self.message_interests.lock().unwrap().insert(None)
    }

    /// Check for receipt of a message with a given ID.
    fn poll_recv(&self, id: u64) -> Poll<DecodeBuf, Error> {
        // Look to see if there are messages available
        if self.received_messages_count.load(Ordering::SeqCst) > 0 {
           if let Entry::Occupied(mut entry) =
               self.message_interests.lock().unwrap().inner_map().entry(id)
           {
               // If a message was received for the ID in question,
               // remove the message interest entry and return the response.
               if let Some(buf) = entry.get_mut().take() {
                   entry.remove_entry();
                   self.received_messages_count.fetch_sub(1, Ordering::SeqCst);
                   return Ok(Async::Ready(buf));
               }
           }
        }

        // Receive messages from the channel until a message with the appropriate ID
        // is found, or the channel is exhausted.
        loop {
            let mut buf = DecodeBuf::new();
            match self.channel.recv_from(0, buf.get_mut_buf()) {
                Ok(()) => {},
                Err(e) => {
                    if e.kind() == io::ErrorKind::WouldBlock {
                        return Ok(Async::NotReady);
                    } else {
                        return Err(e.into());
                    }
                }
            }

            match buf.decode_message_header() {
                Some(MsgType::Response) => {
                    let recvd_id = buf.get_message_id();

                    // If a message was received for the ID in question,
                    // remove the message interest entry and return the response.
                    if recvd_id == id {
                        self.message_interests.lock().unwrap().remove(id);
                        return Ok(Async::Ready(buf));
                    }

                    // Look for a message interest with the given ID.
                    // If one is found, store the message so that it can be picked up later.
                    if let Entry::Occupied(mut entry) =
                        self.message_interests.lock().unwrap().inner_map().entry(id)
                    {
                        *entry.get_mut() = Some(buf);
                        self.received_messages_count.fetch_add(1, Ordering::SeqCst);
                    }
                }
                _ => {
                    // Ignore messages with invalid headers.
                    // We don't want to stop recieving any messages just because the server
                    // sent one bad one.
                },
            }
        }
    }

    fn deregister_msg_interest(&self, id: u64) {
        if self.message_interests
               .lock().unwrap().remove(id)
               .expect("attempted to deregister unregistered message interest")
               .is_some()
        {
            self.received_messages_count.fetch_sub(1, Ordering::SeqCst);
        }
    }
}

pub struct Client {
    inner: Arc<ClientInner>,
}

// TODO: someday impl Trait
type SendMessageExpectResponseFuture = future::Either<
        future::FutureResult<DecodeBuf, Error>,
        MessageResponse>;

impl Client {
    pub fn new(channel: tokio_fuchsia::Channel, handle: &Handle) -> Client {
        // Unused for now-- left in public API to allow for future backwards
        // compatibility and to allow for future changes to spawn futures.
        let _ = handle;
        Client {
            inner: Arc::new(ClientInner {
                channel: channel,
                received_messages_count: AtomicUsize::new(0),
                message_interests: Mutex::new(CookieMap::new()),
            })
        }
    }

    pub fn send_msg(&self, buf: &mut EncodeBuf) -> Result<(), Error> {
        let (out_buf, handles) = buf.get_mut_content();
        Ok(self.inner.channel.write(out_buf, handles, 0)?)
    }

    pub fn send_msg_expect_response(&self, buf: &mut EncodeBuf)
            -> SendMessageExpectResponseFuture
    {
        let id = self.inner.register_msg_interest();
        buf.set_message_id(id);
        let (out_buf, handles) = buf.get_mut_content();
        if let Err(e) = self.inner.channel.write(out_buf, handles, 0) {
            return future::Either::A(future::err(e.into()));
        }

        future::Either::B(MessageResponse {
            id: id,
            client: Some(self.inner.clone()),
        })
    }
}

#[must_use]
pub struct MessageResponse {
    id: u64,
    // `None` if the message response has been recieved
    client: Option<Arc<ClientInner>>,
}

impl Future for MessageResponse {
    type Item = DecodeBuf;
    type Error = Error;
    fn poll(&mut self) -> Poll<Self::Item, Self::Error> {
        let res = if let Some(ref client) = self.client {
            client.poll_recv(self.id)
        } else {
            return Err(Error::PollAfterCompletion)
        };

        // Drop the client reference if the response has been received
        if let Ok(Async::Ready(_)) = res {
            self.client.take();
        }

        res
    }
}

impl Drop for MessageResponse {
    fn drop(&mut self) {
        if let Some(ref client) = self.client {
            client.deregister_msg_interest(self.id)
        }
    }
}

#[cfg(test)]
mod tests {
    use magenta::{self, MessageBuf, ChannelOpts};
    use std::time::Duration;
    use tokio_fuchsia::Channel;
    use tokio_core::reactor::{Core, Timeout};
    use byteorder::{ByteOrder, LittleEndian};
    use super::*;

    #[test]
    fn client() {
        let mut core = Core::new().unwrap();
        let handle = core.handle();

        let (client_end, server_end) = magenta::Channel::create(ChannelOpts::Normal).unwrap();
        let client_end = Channel::from_channel(client_end, &handle).unwrap();
        let client = Client::new(client_end, &handle);

        let server = Channel::from_channel(server_end, &handle).unwrap();
        let mut buffer = MessageBuf::new();
        let receiver = server.recv_msg(0, &mut buffer).map(|(_chan, buf)| {
            let bytes = &[16, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0];
            println!("{:?}", buf.bytes());
            assert_eq!(bytes, buf.bytes());
        });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let rcv_timeout = Timeout::new(Duration::from_millis(300), &handle).unwrap().map(|()| {
            panic!("did not receive message in time!");
        });
        let receiver = receiver.select(rcv_timeout).map(|(_,_)| ()).map_err(|(err,_)| err);

        let sender = Timeout::new(Duration::from_millis(100), &handle).unwrap().map(|()|{
            let mut req = EncodeBuf::new_request(42);
            client.send_msg(&mut req);
        });

        let done = receiver.join(sender);
        core.run(done).unwrap();
    }

    #[test]
    fn client_with_response() {
        let mut core = Core::new().unwrap();
        let handle = core.handle();

        let (client_end, server_end) = magenta::Channel::create(ChannelOpts::Normal).unwrap();
        let client_end = Channel::from_channel(client_end, &handle).unwrap();
        let client = Client::new(client_end, &handle);

        let server = Channel::from_channel(server_end, &handle).unwrap();
        let mut buffer = MessageBuf::new();
        let receiver = server.recv_msg(0, &mut buffer).map(|(chan, buf)| {
            let bytes = &[24, 0, 0, 0, 1, 0, 0, 0, 42, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0];
            println!("{:?}", buf.bytes());
            assert_eq!(bytes, buf.bytes());
            let id = LittleEndian::read_u64(&buf.bytes()[16..24]);

            let mut response = EncodeBuf::new_response(42);
            response.set_message_id(id);
            let (out_buf, handles) = response.get_mut_content();
            let _ = chan.write(out_buf, handles, 0);
        });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let rcv_timeout = Timeout::new(Duration::from_millis(300), &handle).unwrap().map(|()| {
            panic!("did not receive message in time!");
        });
        let receiver = receiver.select(rcv_timeout).map(|(_,_)| ()).map_err(|(err,_)| err);

        let mut req = EncodeBuf::new_request_expecting_response(42);
        let sender = client.send_msg_expect_response(&mut req)
            .map_err(|e| {
                println!("error {:?}", e);
                io::Error::new(io::ErrorKind::Other, "fidl error")
            });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let send_timeout = Timeout::new(Duration::from_millis(300), &handle).unwrap().map(|()| {
            panic!("did not receive response in time!");
        });
        let sender = sender.select(send_timeout).map(|(_,_)| ()).map_err(|(err,_)| err);

        let done = receiver.join(sender);
        core.run(done).unwrap();
    }
}
