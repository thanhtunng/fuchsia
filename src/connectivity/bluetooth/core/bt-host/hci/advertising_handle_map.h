// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_ADVERTISING_HANDLE_MAP_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_ADVERTISING_HANDLE_MAP_H_

#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"

namespace bt::hci {

// When communicating with the controller, we do so in terms of an AdvertisingHandle. This means
// that we frequently need to convert between a DeviceAddress and an AdvertisingHandle.
// AdvertisingHandleMap provides a 1:1 bidirectional mapping between a DeviceAddress and an
// AdvertisingHandle, allocating the next available AdvertisingHandle to new DeviceAddresses.
//
// Users shouldn't rely on any particular ordering of the next available mapping. Any available
// AdvertisingHandle may be used.
//
// TODO(fxbug.dev/78081): implement a bidirectional map that can support looking up key to value as
// well as value to key (two map solution is probably good enough). Makes this clearer, easier to
// maintain, etc.
class AdvertisingHandleMap {
 public:
  // AdvertisingHandles range from 0x00 to kMaxAdvertisingHandle. Also, kMaxAdvertisingHandle is a
  // valid AdvertisingHandle. This value tracks the maximum number of elements that can be stored in
  // this container.
  constexpr static std::size_t kMaxElements = kMaxAdvertisingHandle + 1;

  // Convert a DeviceAddress to an AdvertisingHandle, creating the mapping if it doesn't already
  // exist. The conversion may fail if there are already kMaxAdvertisingHandles in the container.
  std::optional<AdvertisingHandle> MapHandle(const DeviceAddress& address);

  // Convert an AdvertisingHandle to a DeviceAddress. The conversion may fail if there is no
  // DeviceAddress currently mapping to the provided handle.
  std::optional<DeviceAddress> GetAddress(AdvertisingHandle handle) const;

  // Remove the mapping between an AdvertisingHandle and the DeviceAddress it maps to. Immediate
  // future calls to GetAddress(handle) with the same AdvertisingHandle will fail because the
  // mapping no longer exists. The container may reuse the AdvertisingHandle for other
  // DeviceAddresses in the future. Immediate future calls to GetHandle(address) will result in a
  // new mapping with a new AdvertisingHandle.
  //
  // If the given handle doesn't map to any DeviceAddress, this function does nothing.
  void RemoveHandle(AdvertisingHandle handle);

  // Remove the mapping between a DeviceAddress and the AdvertisingHandle it maps to. Immediate
  // future calls to GetAddress(handle) with the preivously mapped AdvertisingHandle will fail
  // because the mapping no longer exists. The container may reuse the AdvertisingHandle for other
  // DeviceAddresses in the future. Immediate future calls to GetHandle(address) will result in a
  // new mapping with a new AdvertisingHandle.
  void RemoveAddress(const DeviceAddress& address);

  // Retrieve the advertising handle that was most recently generated. This function is primarily
  // used by unit tests so as to avoid hardcoding values or making assumptions about the starting
  // point or ordering of advertising handle generation.
  std::optional<AdvertisingHandle> LastUsedHandleForTesting() const;

  // Get the number of unique mappings in the container
  std::size_t Size() const;

  // Return true if the container has no mappings, false otherwise
  bool Empty() const;

  // Remove all mappings in the container
  void Clear();

 private:
  // Although not in the range of valid advertising handles (0x00 to 0xEF), kStartHandle is chosen
  // to be 0xFF because adding one to it will overflow to 0, the first valid advertising handle.
  constexpr static AdvertisingHandle kStartHandle = 0xFF;

  // Generate the next valid, available, and within range AdvertisingHandle. This function may fail
  // if there are already kMaxAdvertisingHandles in the container: there are no more valid
  // AdvertisingHandles.
  std::optional<AdvertisingHandle> NextHandle();

  // The last generated advertising handle used as a hint to generate the next handle; defaults to
  // kStartHandle if no handles have been generated yet.
  AdvertisingHandle last_handle_ = kStartHandle;

  std::unordered_map<DeviceAddress, AdvertisingHandle> addr_to_handle_;
  std::unordered_map<AdvertisingHandle, DeviceAddress> handle_to_addr_;
};

}  // namespace bt::hci

#endif
