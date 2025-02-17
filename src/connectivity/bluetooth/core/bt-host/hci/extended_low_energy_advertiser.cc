// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/hci/extended_low_energy_advertiser.h"

#include "src/connectivity/bluetooth/core/bt-host/hci-spec/util.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"

namespace bt::hci {

ExtendedLowEnergyAdvertiser::ExtendedLowEnergyAdvertiser(fxl::WeakPtr<Transport> hci_ptr)
    : LowEnergyAdvertiser(std::move(hci_ptr)), weak_ptr_factory_(this) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  event_handler_id_ = hci()->command_channel()->AddLEMetaEventHandler(
      kLEAdvertisingSetTerminatedSubeventCode, [self](const EventPacket& event_packet) {
        if (self) {
          return self->OnAdvertisingSetTerminatedEvent(event_packet);
        }

        return CommandChannel::EventCallbackResult::kRemove;
      });
}

ExtendedLowEnergyAdvertiser::~ExtendedLowEnergyAdvertiser() {
  hci()->command_channel()->RemoveEventHandler(event_handler_id_);
  StopAdvertising();
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildEnablePacket(
    const DeviceAddress& address, GenericEnableParam enable) {
  // We only enable or disable a single address at a time. The multiply by 1 is set explicitly to
  // show that data[] within LESetExtendedAdvertisingEnableData is of size 1.
  constexpr size_t kPayloadSize = sizeof(LESetExtendedAdvertisingEnableCommandParams) +
                                  1 * sizeof(LESetExtendedAdvertisingEnableData);
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(kLESetExtendedAdvertisingEnable, kPayloadSize);
  packet->mutable_view()->mutable_payload_data().SetToZeros();

  auto payload = packet->mutable_payload<LESetExtendedAdvertisingEnableCommandParams>();
  payload->enable = enable;
  payload->number_of_sets = 1;

  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }

  // TODO(fxbug.dev/77614): advertising currently continues until disabled. We should provide
  // options to the user to advertise only a set amount of times. Duration can already be controlled
  // via the client closing the FIDL protocol after a set amount of time. We don't want to
  // support a second way to enable duration.
  LESetExtendedAdvertisingEnableData enable_data;
  enable_data.adv_handle = handle.value();
  enable_data.max_extended_adv_events = kNoMaxExtendedAdvertisingEvents;
  enable_data.duration = kNoAdvertisingDuration;

  auto buffer = packet->mutable_view()->mutable_payload_data().mutable_view();
  buffer.WriteObj(enable_data, sizeof(LESetExtendedAdvertisingEnableCommandParams));

  return packet;
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildSetAdvertisingParams(
    const DeviceAddress& address, LEAdvertisingType type, LEOwnAddressType own_address_type,
    AdvertisingIntervalRange interval) {
  constexpr size_t kPayloadSize = sizeof(LESetExtendedAdvertisingParametersCommandParams);
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(kLESetExtendedAdvertisingParameters, kPayloadSize);
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  auto payload = packet->mutable_payload<LESetExtendedAdvertisingParametersCommandParams>();

  // advertising handle
  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }
  payload->adv_handle = handle.value();

  // advertising event properties
  std::optional<AdvertisingEventBits> bits = AdvertisingTypeToEventBits(type);
  if (!bits) {
    bt_log(WARN, "hci-le", "could not generate event bits for type: %hhu", type);
    return nullptr;
  }
  payload->adv_event_properties = bits.value();

  // advertising interval, NOTE: LE advertising parameters allow for up to 3 octets (10 ms to
  // 10428 s) to configure an advertising interval. However, we expose only the recommended
  // advertising interval configurations to users, as specified in the Bluetooth Spec Volume 3, Part
  // C, Appendix A. These values are expressed as uint16_t so we simply copy them (taking care of
  // endianness) into the 3 octets as is.
  EncodeLegacyAdvertisingInterval(interval.min(), payload->primary_adv_interval_min);
  EncodeLegacyAdvertisingInterval(interval.max(), payload->primary_adv_interval_max);

  // other settings
  payload->primary_adv_channel_map = kLEAdvertisingChannelAll;
  payload->own_address_type = own_address_type;
  payload->adv_filter_policy = LEAdvFilterPolicy::kAllowAll;
  payload->adv_tx_power = kLEExtendedAdvertisingTxPowerNoPreference;
  payload->scan_request_notification_enable = GenericEnableParam::kDisable;

  // TODO(fxbug.dev/81470): using legacy PDUs requires advertisements on the LE 1M PHY.
  payload->primary_adv_phy = LEPHY::kLE1M;

  // Payload values were initialized to zero above. By not setting the values for the following
  // fields, we are purposely ignoring them:
  //
  // advertising_sid: We use only legacy PDUs, the controller ignores this field in that case
  // peer_address: We don't support directed advertising yet
  // peer_address_type: We don't support directed advertising yet
  // secondary_adv_max_skip: We use only legacy PDUs, the controller ignores this field in that case
  // secondary_adv_phy: We use only legacy PDUs, the controller ignores this field in that case

  return packet;
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildSetAdvertisingData(
    const DeviceAddress& address, const AdvertisingData& data, AdvFlags flags) {
  size_t block_size = data.CalculateBlockSize(/*include_flags=*/true);

  AdvertisingData tx_power_data;
  if (staged_advertising_parameters_.include_tx_power_level) {
    data.Copy(&tx_power_data);

    tx_power_data.SetTxPower(staged_advertising_parameters_.selected_tx_power_level);
    block_size = tx_power_data.CalculateBlockSize(/*include_flags=*/true);
  }

  size_t kPayloadSize = sizeof(LESetExtendedAdvertisingDataCommandParams) + block_size;
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(kLESetExtendedAdvertisingData, kPayloadSize);
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  auto payload = packet->mutable_payload<LESetExtendedAdvertisingDataCommandParams>();

  // advertising handle
  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }
  payload->adv_handle = handle.value();

  // TODO(fxbug.dev/81470): We support only legacy PDUs and do not support fragmented extended
  // advertising data at this time.
  payload->operation = LESetExtendedAdvDataOp::kComplete;
  payload->fragment_preference = LEExtendedAdvFragmentPreference::kShouldNotFragment;

  // advertising data
  payload->adv_data_length = block_size;
  MutableBufferView buffer = packet->mutable_view()->mutable_payload_data().mutable_view(
      sizeof(LESetExtendedAdvertisingDataCommandParams));

  if (staged_advertising_parameters_.include_tx_power_level) {
    tx_power_data.WriteBlock(&buffer, flags);
  } else {
    data.WriteBlock(&buffer, flags);
  }

  return packet;
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildUnsetAdvertisingData(
    const DeviceAddress& address) {
  std::unique_ptr<CommandPacket> packet = CommandPacket::New(
      kLESetExtendedAdvertisingData, sizeof(LESetExtendedAdvertisingDataCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  auto payload = packet->mutable_payload<LESetExtendedAdvertisingDataCommandParams>();

  // advertising handle
  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }
  payload->adv_handle = handle.value();

  // TODO(fxbug.dev/81470): We support only legacy PDUs and do not support fragmented extended
  // advertising data at this time.
  payload->operation = LESetExtendedAdvDataOp::kComplete;
  payload->fragment_preference = LEExtendedAdvFragmentPreference::kShouldNotFragment;
  payload->adv_data_length = 0;

  return packet;
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildSetScanResponse(
    const DeviceAddress& address, const AdvertisingData& scan_rsp) {
  size_t block_size = scan_rsp.CalculateBlockSize();

  AdvertisingData tx_power_scan_rsp;
  if (staged_advertising_parameters_.include_tx_power_level) {
    scan_rsp.Copy(&tx_power_scan_rsp);

    tx_power_scan_rsp.SetTxPower(staged_advertising_parameters_.selected_tx_power_level);
    block_size = tx_power_scan_rsp.CalculateBlockSize();
  }

  size_t kPayloadSize = sizeof(LESetExtendedScanResponseDataCommandParams) + block_size;
  std::unique_ptr<CommandPacket> packet =
      CommandPacket::New(kLESetExtendedScanResponseData, kPayloadSize);
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  auto payload = packet->mutable_payload<LESetExtendedScanResponseDataCommandParams>();

  // advertising handle
  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }
  payload->adv_handle = handle.value();

  // TODO(fxbug.dev/81470): We support only legacy PDUs and do not support fragmented extended
  // advertising data at this time.
  payload->operation = LESetExtendedAdvDataOp::kComplete;
  payload->fragment_preference = LEExtendedAdvFragmentPreference::kShouldNotFragment;

  // scan response data
  payload->scan_rsp_data_length = block_size;
  MutableBufferView buffer = packet->mutable_view()->mutable_payload_data().mutable_view(
      sizeof(LESetExtendedScanResponseDataCommandParams));

  if (staged_advertising_parameters_.include_tx_power_level) {
    tx_power_scan_rsp.WriteBlock(&buffer, std::nullopt);
  } else {
    scan_rsp.WriteBlock(&buffer, std::nullopt);
  }

  return packet;
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildUnsetScanResponse(
    const DeviceAddress& address) {
  std::unique_ptr<CommandPacket> packet = CommandPacket::New(
      kLESetExtendedScanResponseData, sizeof(LESetExtendedScanResponseDataCommandParams));
  packet->mutable_view()->mutable_payload_data().SetToZeros();
  auto payload = packet->mutable_payload<LESetExtendedScanResponseDataCommandParams>();

  // advertising handle
  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }
  payload->adv_handle = handle.value();

  // TODO(fxbug.dev/81470): We support only legacy PDUs and do not support fragmented extended
  // advertising data at this time.
  payload->operation = LESetExtendedAdvDataOp::kComplete;
  payload->fragment_preference = LEExtendedAdvFragmentPreference::kShouldNotFragment;
  payload->scan_rsp_data_length = 0;

  return packet;
}

std::unique_ptr<CommandPacket> ExtendedLowEnergyAdvertiser::BuildRemoveAdvertisingSet(
    const DeviceAddress& address) {
  auto packet =
      CommandPacket::New(kLERemoveAdvertisingSet, sizeof(LERemoveAdvertisingSetCommandParams));
  auto payload = packet->mutable_payload<LERemoveAdvertisingSetCommandParams>();

  std::optional<AdvertisingHandle> handle = advertising_handle_map_.MapHandle(address);
  if (!handle) {
    bt_log(WARN, "hci-le", "could not locate advertising handle for address: %s", bt_str(address));
    return nullptr;
  }
  payload->adv_handle = handle.value();

  return packet;
}

void ExtendedLowEnergyAdvertiser::OnSetAdvertisingParamsComplete(const EventPacket& event) {
  ZX_ASSERT(event.event_code() == kCommandCompleteEventCode);
  ZX_ASSERT(event.params<CommandCompleteEventParams>().command_opcode ==
            kLESetExtendedAdvertisingParameters);

  Status status = event.ToStatus();
  if (bt_is_error(status, WARN, "hci-le", "set advertising parameters, error received: %s",
                  bt_str(status))) {
    return;  // full error handling done in super class, can just return here
  }

  auto params = event.return_params<LESetExtendedAdvertisingParametersReturnParams>();
  ZX_ASSERT(params);

  if (staged_advertising_parameters_.include_tx_power_level) {
    staged_advertising_parameters_.selected_tx_power_level = params->selected_tx_power;
  }
}

void ExtendedLowEnergyAdvertiser::StartAdvertising(const DeviceAddress& address,
                                                   const AdvertisingData& data,
                                                   const AdvertisingData& scan_rsp,
                                                   AdvertisingOptions options,
                                                   ConnectionCallback connect_callback,
                                                   StatusCallback status_callback) {
  ZX_ASSERT(status_callback);
  ZX_ASSERT(address.type() != DeviceAddress::Type::kBREDR);

  // if there is an operation currently in progress, enqueue this operation and we will get to it
  // the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(INFO, "hci-le", "hci cmd runner not ready, queuing advertisement commands for now");

    AdvertisingData copied_data;
    data.Copy(&copied_data);

    AdvertisingData copied_scan_rsp;
    scan_rsp.Copy(&copied_scan_rsp);

    op_queue_.push([this, address, data = std::move(copied_data),
                    scan_rsp = std::move(copied_scan_rsp), options,
                    conn_cb = std::move(connect_callback),
                    status_cb = std::move(status_callback)]() mutable {
      StartAdvertising(address, data, scan_rsp, options, std::move(conn_cb), std::move(status_cb));
    });

    return;
  }

  if (options.anonymous) {
    bt_log(WARN, "hci-le", "anonymous advertising not supported");
    status_callback(Status(HostError::kNotSupported));
    return;
  }

  if (IsAdvertising(address)) {
    bt_log(DEBUG, "hci-le", "updating existing advertisement for %s", bt_str(address));
  }

  // If the TX Power Level is requested, ensure both buffers have enough space.
  size_t size_limit = GetSizeLimit();
  if (options.include_tx_power_level) {
    size_limit -= kTLVTxPowerLevelSize;
  }

  if (size_t size = data.CalculateBlockSize(/*include_flags=*/true); size > size_limit) {
    bt_log(WARN, "hci-le", "advertising data too large (actual: %zu, max: %zu)", size, size_limit);
    status_callback(Status(HostError::kAdvertisingDataTooLong));
    return;
  }

  if (size_t size = scan_rsp.CalculateBlockSize(/*include_flags=*/false); size > size_limit) {
    bt_log(WARN, "hci-le", "scan response too large (actual: %zu, max: %zu)", size, size_limit);
    status_callback(Status(HostError::kScanResponseTooLong));
    return;
  }

  std::memset(&staged_advertising_parameters_, 0, sizeof(staged_advertising_parameters_));
  staged_advertising_parameters_.include_tx_power_level = options.include_tx_power_level;

  // Core Spec, Volume 4, Part E, Section 7.8.58: "the number of advertising sets that can be
  // supported is not fixed and the Controller can change it at any time. The memory used to store
  // advertising sets can also be used for other purposes."
  //
  // Depending on the memory profile of the controller, a new advertising set may or may not be
  // accepted. We could use HCI_LE_Read_Number_of_Supported_Advertising_Sets to check if the
  // controller has space for another advertising set. However, the value may change after the read
  // and before the addition of the advertising set. Furthermore, sending an extra HCI command
  // increases the latency of our stack. Instead, we simply attempt to add. If the controller is
  // unable to support another advertising set, it will respond with a memory capacity exceeded
  // error.
  StartAdvertisingInternal(address, data, scan_rsp, options.interval, options.flags,
                           std::move(connect_callback), std::move(status_callback));
}

void ExtendedLowEnergyAdvertiser::StopAdvertising() {
  LowEnergyAdvertiser::StopAdvertising();
  advertising_handle_map_.Clear();

  // std::queue doesn't have a clear method so we have to resort to this tomfoolery :(
  decltype(op_queue_) empty;
  std::swap(op_queue_, empty);
}

void ExtendedLowEnergyAdvertiser::StopAdvertising(const DeviceAddress& address) {
  if (!IsAdvertising(address)) {
    // not advertising, or not advertising on this address.
    return;
  }

  // if there is an operation currently in progress, enqueue this operation and we will get to it
  // the next time we have a chance
  if (!hci_cmd_runner().IsReady()) {
    bt_log(INFO, "hci-le", "hci cmd runner not ready, queueing stop advertising command for now");
    op_queue_.push([this, address]() { StopAdvertising(address); });
    return;
  }

  LowEnergyAdvertiser::StopAdvertisingInternal(address);
  advertising_handle_map_.RemoveAddress(address);
}

void ExtendedLowEnergyAdvertiser::OnIncomingConnection(ConnectionHandle handle,
                                                       Connection::Role role,
                                                       const DeviceAddress& peer_address,
                                                       const LEConnectionParameters& conn_params) {
  // Core Spec Volume 4, Part E, Section 7.8.56: Incoming connections to LE Extended Advertising
  // occur through two events: HCI_LE_Connection_Complete and HCI_LE_Advertising_Set_Terminated.
  // This method is called as a result of the HCI_LE_Connection_Complete event. At this point, we
  // only have a connection handle but don't know the locally advertised address that the
  // connection is for. Until we receive the HCI_LE_Advertising_Set_Terminated event, we stage
  // these parameters.
  connection_handle_map_[handle] = {role, peer_address, conn_params};
}

// The HCI_LE_Advertising_Set_Terminated event contains the mapping between connection handle and
// advertising handle. After the HCI_LE_Advertising_Set_Terminated event, we have all the
// information necessary to create a connection object within the Host layer.
CommandChannel::EventCallbackResult ExtendedLowEnergyAdvertiser::OnAdvertisingSetTerminatedEvent(
    const EventPacket& event) {
  ZX_ASSERT(event.event_code() == kLEMetaEventCode);
  ZX_ASSERT(event.params<LEMetaEventParams>().subevent_code ==
            kLEAdvertisingSetTerminatedSubeventCode);

  Status status = event.ToStatus();
  if (bt_is_error(status, ERROR, "hci-le", "advertising set terminated event, error received %s",
                  bt_str(status))) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  auto params = event.le_event_params<LEAdvertisingSetTerminatedSubeventParams>();
  ZX_ASSERT(params);

  ConnectionHandle connection_handle = params->connection_handle;
  auto sp_node = connection_handle_map_.extract(connection_handle);

  if (sp_node.empty()) {
    bt_log(ERROR, "hci-le",
           "advertising set terminated event, staged params not available "
           "(handle: %d)",
           params->adv_handle);
    return CommandChannel::EventCallbackResult::kContinue;
  }

  AdvertisingHandle adv_handle = params->adv_handle;
  std::optional<DeviceAddress> opt_local_address = advertising_handle_map_.GetAddress(adv_handle);

  // We use the identity address as the local address if we aren't advertising or otherwise don't
  // know about this advertising set. This is obviously wrong. However, the link will be
  // disconnected in that case before it can propagate to higher layers.
  static DeviceAddress identity_address = DeviceAddress(DeviceAddress::Type::kLEPublic, {0});
  DeviceAddress local_address = identity_address;
  if (opt_local_address) {
    local_address = opt_local_address.value();
  }

  StagedConnectionParameters staged = sp_node.mapped();

  CompleteIncomingConnection(connection_handle, staged.role, local_address, staged.peer_address,
                             staged.conn_params);

  std::memset(&staged_advertising_parameters_, 0, sizeof(staged_advertising_parameters_));
  return CommandChannel::EventCallbackResult::kContinue;
}

void ExtendedLowEnergyAdvertiser::OnCurrentOperationComplete() {
  if (op_queue_.empty()) {
    return;  // no more queued operations so nothing to do
  }

  fit::closure closure = std::move(op_queue_.front());
  op_queue_.pop();
  closure();
}

}  // namespace bt::hci
