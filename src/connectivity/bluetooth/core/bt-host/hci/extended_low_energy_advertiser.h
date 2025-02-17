// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_EXTENDED_LOW_ENERGY_ADVERTISER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_EXTENDED_LOW_ENERGY_ADVERTISER_H_

#include "src/connectivity/bluetooth/core/bt-host/hci/advertising_handle_map.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/low_energy_advertiser.h"

namespace bt::hci {

class Transport;
class SequentialCommandRunner;

class ExtendedLowEnergyAdvertiser final : public LowEnergyAdvertiser {
 public:
  explicit ExtendedLowEnergyAdvertiser(fxl::WeakPtr<Transport> hci);
  ~ExtendedLowEnergyAdvertiser() override;

  bool AllowsRandomAddressChange() const override { return !IsAdvertising(); }

  // TODO(fxbug.dev/81470): The maximum length of data that can be advertised. For backwards
  // compatibility and because supporting it is a much larger project, we currently only support
  // legacy PDUs. When using legacy PDUs, the maximum advertising data size is
  // kMaxLEAdvertisingDataLength.
  //
  // TODO(fxbug.dev/77476): Extended advertising supports sending larger amounts of data, but they
  // have to be fragmented across multiple commands to the controller. This is not yet supported in
  // this implementation. We should support larger than kMaxLEExtendedAdvertisingDataLength
  // advertising data with fragmentation.
  size_t GetSizeLimit() const override { return kMaxLEAdvertisingDataLength; }

  // Attempt to start advertising. See LowEnergyAdvertiser::StartAdvertising for full documentation.
  //
  // According to the Bluetooth Spec, Volume 4, Part E, Section 7.8.58, "the number of advertising
  // sets that can be supported is not fixed and the Controller can change it at any time. The
  // memory used to store advertising sets can also be used for other purposes."
  //
  // This method may report an error if the controller cannot currently support another advertising
  // set.
  void StartAdvertising(const DeviceAddress& address, const AdvertisingData& data,
                        const AdvertisingData& scan_rsp, AdvertisingOptions adv_options,
                        ConnectionCallback connect_callback,
                        StatusCallback status_callback) override;

  // Stops advertisement on all currently advertising addresses. Idempotent and asynchronous.
  // Returns true if advertising will be stopped, false otherwise.
  void StopAdvertising() override;

  // Stops any advertisement currently active on |address|. Idempotent and asynchronous. Returns
  // true if advertising will be stopped, false otherwise.
  void StopAdvertising(const DeviceAddress& address) override;

  void OnIncomingConnection(ConnectionHandle handle, Connection::Role role,
                            const DeviceAddress& peer_address,
                            const LEConnectionParameters& conn_params) override;

  // Returns the last used advertising handle that was used for an advertising set when
  // communicating with the controller.
  std::optional<AdvertisingHandle> LastUsedHandleForTesting() const {
    return advertising_handle_map_.LastUsedHandleForTesting();
  }

 private:
  struct StagedConnectionParameters {
    Connection::Role role;
    DeviceAddress peer_address;
    LEConnectionParameters conn_params;
  };

  struct StagedAdvertisingParameters {
    bool include_tx_power_level = false;
    int8_t selected_tx_power_level = 0;
  };

  std::unique_ptr<CommandPacket> BuildEnablePacket(const DeviceAddress& address,
                                                   GenericEnableParam enable) override;

  std::unique_ptr<CommandPacket> BuildSetAdvertisingParams(
      const DeviceAddress& address, LEAdvertisingType type, LEOwnAddressType own_address_type,
      AdvertisingIntervalRange interval) override;

  std::unique_ptr<CommandPacket> BuildSetAdvertisingData(const DeviceAddress& address,
                                                         const AdvertisingData& data,
                                                         AdvFlags flags) override;

  std::unique_ptr<CommandPacket> BuildUnsetAdvertisingData(const DeviceAddress& address) override;

  std::unique_ptr<CommandPacket> BuildSetScanResponse(const DeviceAddress& address,
                                                      const AdvertisingData& scan_rsp) override;

  std::unique_ptr<CommandPacket> BuildUnsetScanResponse(const DeviceAddress& address) override;

  std::unique_ptr<CommandPacket> BuildRemoveAdvertisingSet(const DeviceAddress& address) override;

  void OnSetAdvertisingParamsComplete(const EventPacket& event) override;

  void OnCurrentOperationComplete() override;

  // Event handler for the HCI LE Advertising Set Terminated event
  CommandChannel::EventCallbackResult OnAdvertisingSetTerminatedEvent(const EventPacket& event);
  CommandChannel::EventHandlerId event_handler_id_;

  AdvertisingHandleMap advertising_handle_map_;
  std::unordered_map<ConnectionHandle, StagedConnectionParameters> connection_handle_map_;
  std::queue<fit::closure> op_queue_;
  StagedAdvertisingParameters staged_advertising_parameters_;

  // Keep this as the last member to make sure that all weak pointers are invalidated before other
  // members get destroyed
  fxl::WeakPtrFactory<ExtendedLowEnergyAdvertiser> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ExtendedLowEnergyAdvertiser);
};

}  // namespace bt::hci

#endif
