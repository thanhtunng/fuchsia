// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_COORDINATOR_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_COORDINATOR_H_

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <fidl/fuchsia.driver.development/cpp/wire.h>
#include <fidl/fuchsia.driver.registrar/cpp/wire.h>
#include <fidl/fuchsia.fshost/cpp/wire.h>
#include <fidl/fuchsia.power.manager/cpp/wire.h>
#include <lib/async/cpp/wait.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/device.h>
#include <lib/fidl/llcpp/server.h>
#include <lib/stdcompat/optional.h>
#include <lib/svc/outgoing.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/status.h>
#include <lib/zx/vmo.h>
#include <zircon/types.h>

#include <memory>
#include <string_view>
#include <utility>

#include <fbl/intrusive_double_list.h>
#include <fbl/string.h>
#include <fbl/vector.h>

#include "composite_device.h"
#include "devfs.h"
#include "device.h"
#include "driver.h"
#include "driver_host.h"
#include "driver_loader.h"
#include "fbl/auto_lock.h"
#include "fidl/fuchsia.device.manager/cpp/wire.h"
#include "fidl/fuchsia.driver.framework/cpp/wire.h"
#include "fidl/fuchsia.hardware.power.statecontrol/cpp/wire.h"
#include "init_task.h"
#include "inspect.h"
#include "metadata.h"
#include "package_resolver.h"
#include "resume_task.h"
#include "suspend_handler.h"
#include "suspend_task.h"
#include "system_state_manager.h"
#include "unbind_task.h"
#include "vmo_writer.h"

namespace statecontrol_fidl = fuchsia_hardware_power_statecontrol;
using statecontrol_fidl::wire::SystemPowerState;
namespace fdf = fuchsia_driver_framework;

class DriverHostLoaderService;
class FsProvider;
class SystemStateManager;

constexpr zx::duration kDefaultResumeTimeout = zx::sec(30);
constexpr zx::duration kDefaultSuspendTimeout = zx::sec(30);

// Tracks the global resume state that is currently in progress.
class ResumeContext {
 public:
  enum class Flags : uint32_t {
    kResume = 0u,
    kSuspended = 1u,
  };
  ResumeContext() = default;

  ResumeContext(Flags flags, SystemPowerState resume_state)
      : target_state_(resume_state), flags_(flags) {}

  ~ResumeContext() {}

  ResumeContext(ResumeContext&&) = default;
  ResumeContext& operator=(ResumeContext&&) = default;

  Flags flags() const { return flags_; }
  void set_flags(Flags flags) { flags_ = flags; }
  void push_pending_task(fbl::RefPtr<ResumeTask> task) {
    pending_resume_tasks_.push_back(std::move(task));
  }
  void push_completed_task(fbl::RefPtr<ResumeTask> task) {
    completed_resume_tasks_.push_back(std::move(task));
  }

  bool pending_tasks_is_empty() { return pending_resume_tasks_.is_empty(); }
  bool completed_tasks_is_empty() { return completed_resume_tasks_.is_empty(); }

  std::optional<fbl::RefPtr<ResumeTask>> take_pending_task(Device& dev) {
    for (size_t i = 0; i < pending_resume_tasks_.size(); i++) {
      if (&pending_resume_tasks_[i]->device() == &dev) {
        auto task = pending_resume_tasks_.erase(i);
        return std::move(task);
      }
    }
    return {};
  }

  void reset_completed_tasks() { completed_resume_tasks_.reset(); }

  SystemPowerState target_state() const { return target_state_; }

 private:
  fbl::Vector<fbl::RefPtr<ResumeTask>> pending_resume_tasks_;
  fbl::Vector<fbl::RefPtr<ResumeTask>> completed_resume_tasks_;
  SystemPowerState target_state_;
  Flags flags_ = Flags::kSuspended;
};

using ResumeCallback = std::function<void(zx_status_t)>;

// The action to take when we witness a driver host crash.
enum class DriverHostCrashPolicy {
  // Restart the driver host, with exponential backoff, up to 3 times.
  // This will only be triggered if the driver host which host's the driver which created the
  // parent device being bound to doesn't also crash.
  // TODO(fxbug.dev/66442): Handle composite devices better (they don't seem to restart with this
  // policy set).
  kRestartDriverHost,
  // Reboot the system via the power manager.
  kRebootSystem,
  // Don't take any action, other than cleaning up some internal driver manager state.
  kDoNothing,
};

struct CoordinatorConfig {
  // Initial root resource from the kernel.
  zx::resource root_resource;
  // Job for all driver_hosts.
  zx::job driver_host_job;
  // Event that is signaled by the kernel in OOM situation.
  zx::event oom_event;
  // Client for the Arguments service.
  fidl::WireSyncClient<fuchsia_boot::Arguments>* boot_args;
  // Client for the DriverIndex.
  fidl::WireSharedClient<fdf::DriverIndex> driver_index;
  // Whether we require /system.
  bool require_system = false;
  // Whether to reboot the device when suspend does not finish on time.
  bool suspend_fallback = false;
  // Whether to output logs to debuglog.
  bool log_to_debuglog = false;
  // Whether to enable verbose logging.
  bool verbose = false;
  // Whether to allow loading drivers ephemerally. This should only be enabled on eng builds.
  bool enable_ephemeral = false;
  // Timeout for system wide suspend
  zx::duration suspend_timeout = kDefaultSuspendTimeout;
  // Timeout for system wide resume
  zx::duration resume_timeout = kDefaultResumeTimeout;
  // System will be transitioned to this system power state during
  // component shutdown.
  SystemPowerState default_shutdown_system_state = SystemPowerState::kReboot;
  // Something to clone a handle from the environment to pass to a Devhost.
  FsProvider* fs_provider = nullptr;
  // The path prefix to find binaries, drivers, etc. Typically this is "/boot/", but in test
  // environments this might be different.
  std::string path_prefix = "/boot/";
  // The decision to make when we encounter a driver host crash.
  DriverHostCrashPolicy crash_policy = DriverHostCrashPolicy::kRestartDriverHost;
};

class Coordinator : public fidl::WireServer<fuchsia_driver_development::DriverDevelopment>,
                    public fidl::WireServer<fuchsia_device_manager::Administrator>,
                    public fidl::WireServer<fuchsia_device_manager::DebugDumper>,
                    public fidl::WireServer<fuchsia_driver_registrar::DriverRegistrar> {
 public:
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;
  Coordinator(Coordinator&&) = delete;
  Coordinator& operator=(Coordinator&&) = delete;

  Coordinator(CoordinatorConfig config, InspectManager* inspect_manager,
              async_dispatcher_t* dispatcher);
  ~Coordinator();

  zx_status_t InitOutgoingServices(const fbl::RefPtr<fs::PseudoDir>& svc_dir);
  zx_status_t InitCoreDevices(std::string_view sys_device_driver);
  zx::status<> InitInspect();
  bool InSuspend() const;
  bool InResume() const;

  // Start searching the system for non-boot drivers.
  // This will start a new thread to load non-boot drivers asynchronously.
  void StartLoadingNonBootDrivers();

  void BindFallbackDrivers();
  void AddAndBindDrivers(fbl::DoublyLinkedList<std::unique_ptr<Driver>> drivers);
  void BindDrivers();
  void DriverAdded(Driver* drv, const char* version);
  void DriverAddedInit(Driver* drv, const char* version);
  zx_status_t LibnameToVmo(const fbl::String& libname, zx::vmo* out_vmo) const;
  const Driver* LibnameToDriver(std::string_view libname) const;

  // Function that is invoked to request a driver try to bind to a device
  using AttemptBindFunc =
      fit::function<zx_status_t(const Driver* drv, const fbl::RefPtr<Device>& dev)>;

  // Attempts to bind the given driver to the given device.  Returns ZX_OK on
  // success, ZX_ERR_ALREADY_BOUND if there is a driver bound to the device
  // and the device is not allowed to be bound multiple times, ZX_ERR_NEXT if
  // the driver is not capable of binding to the device, and a different error
  // if the driver was capable of binding but failed to bind.
  zx_status_t BindDriverToDevice(const fbl::RefPtr<Device>& dev, const Driver* drv, bool autobind) {
    return BindDriverToDevice(dev, drv, autobind,
                              fit::bind_member(this, &Coordinator::AttemptBind));
  }

  // The same as above, but the given function is called to perform the
  // bind attempt.
  zx_status_t BindDriverToDevice(const fbl::RefPtr<Device>& dev, const Driver* drv, bool autobind,
                                 const AttemptBindFunc& attempt_bind);

  // Used to implement fuchsia::device::manager::Coordinator.
  // TODO(fxbug.dev/43370): remove |always_init| once init tasks can be enabled for all devices.
  zx_status_t AddDevice(const fbl::RefPtr<Device>& parent,
                        fidl::ClientEnd<fuchsia_device_manager::DeviceController> device_controller,
                        fidl::ServerEnd<fuchsia_device_manager::Coordinator> coordinator,
                        const fuchsia_device_manager::wire::DeviceProperty* props_data,
                        size_t props_count,
                        const fuchsia_device_manager::wire::DeviceStrProperty* str_props_data,
                        size_t str_props_count, std::string_view name, uint32_t protocol_id,
                        std::string_view driver_path, std::string_view args, bool skip_autobind,
                        bool has_init, bool always_init, zx::vmo inspect, zx::channel client_remote,
                        fbl::RefPtr<Device>* new_device);
  // Begin scheduling for removal of the device and unbinding of its children.
  void ScheduleRemove(const fbl::RefPtr<Device>& dev);
  // This is for scheduling the initial unbind task as a result of a driver_host's |ScheduleRemove|
  // request.
  // If |do_unbind| is true, unbinding is also requested for |dev|.
  void ScheduleDriverHostRequestedRemove(const fbl::RefPtr<Device>& dev, bool do_unbind = false);
  void ScheduleDriverHostRequestedUnbindChildren(const fbl::RefPtr<Device>& parent);
  zx_status_t RemoveDevice(const fbl::RefPtr<Device>& dev, bool forced);
  zx_status_t MakeVisible(const fbl::RefPtr<Device>& dev);
  // Try binding a driver to the device. Returns ZX_ERR_ALREADY_BOUND if there
  // is a driver bound to the device and the device is not allowed to be bound multiple times.
  zx_status_t BindDevice(const fbl::RefPtr<Device>& dev, std::string_view drvlibname,
                         bool new_device);
  zx_status_t GetTopologicalPath(const fbl::RefPtr<const Device>& dev, char* out, size_t max) const;
  zx_status_t LoadFirmware(const fbl::RefPtr<Device>& dev, const char* driver_libname,
                           const char* path, zx::vmo* vmo, size_t* size);

  zx_status_t GetMetadata(const fbl::RefPtr<Device>& dev, uint32_t type, void* buffer,
                          size_t buflen, size_t* size);
  zx_status_t GetMetadataSize(const fbl::RefPtr<Device>& dev, uint32_t type, size_t* size) {
    return GetMetadata(dev, type, nullptr, 0, size);
  }
  zx_status_t AddMetadata(const fbl::RefPtr<Device>& dev, uint32_t type, const void* data,
                          uint32_t length);
  zx_status_t PublishMetadata(const fbl::RefPtr<Device>& dev, const char* path, uint32_t type,
                              const void* data, uint32_t length);
  zx_status_t AddCompositeDevice(const fbl::RefPtr<Device>& dev, std::string_view name,
                                 fuchsia_device_manager::wire::CompositeDeviceDescriptor comp_desc);

  void DmMexec(zx::vmo kernel, zx::vmo bootdata);

  void HandleNewDevice(const fbl::RefPtr<Device>& dev);
  zx_status_t PrepareProxy(const fbl::RefPtr<Device>& dev,
                           fbl::RefPtr<DriverHost> target_driver_host);

  void DumpState(VmoWriter* vmo) const;

  async_dispatcher_t* dispatcher() const { return dispatcher_; }
  const zx::resource& root_resource() const { return config_.root_resource; }
  fidl::WireSyncClient<fuchsia_boot::Arguments>* boot_args() const { return config_.boot_args; }
  bool require_system() const { return config_.require_system; }
  bool suspend_fallback() const { return config_.suspend_fallback; }
  SystemPowerState shutdown_system_state() const { return shutdown_system_state_; }
  SystemPowerState default_shutdown_system_state() const {
    return config_.default_shutdown_system_state;
  }
  void set_shutdown_system_state(SystemPowerState state) { shutdown_system_state_ = state; }

  void set_running(bool running) { running_ = running; }
  void set_power_manager_registered(bool registered) { power_manager_registered_ = registered; }
  bool power_manager_registered() { return power_manager_registered_; }

  void set_loader_service_connector(LoaderServiceConnector loader_service_connector) {
    loader_service_connector_ = std::move(loader_service_connector);
  }

  void set_system_state_manager(std::unique_ptr<SystemStateManager> system_state_mgr) {
    system_state_manager_ = std::move(system_state_mgr);
  }

  fbl::DoublyLinkedList<std::unique_ptr<Driver>>& drivers() { return drivers_; }
  const fbl::DoublyLinkedList<std::unique_ptr<Driver>>& drivers() const { return drivers_; }
  fbl::TaggedDoublyLinkedList<fbl::RefPtr<Device>, Device::AllDevicesListTag>& devices() {
    return devices_;
  }
  const fbl::TaggedDoublyLinkedList<fbl::RefPtr<Device>, Device::AllDevicesListTag>& devices()
      const {
    return devices_;
  }

  void AppendPublishedMetadata(std::unique_ptr<Metadata> metadata) {
    published_metadata_.push_back(std::move(metadata));
  }

  const fbl::RefPtr<Device>& root_device() { return root_device_; }
  const fbl::RefPtr<Device>& sys_device() { return sys_device_; }

  void Suspend(
      uint32_t flags, SuspendCallback = [](zx_status_t status) {});

  void Resume(
      SystemPowerState target_state, ResumeCallback callback = [](zx_status_t) {});

  SuspendHandler& suspend_handler() { return suspend_handler_; }
  const SuspendHandler& suspend_handler() const { return suspend_handler_; }

  ResumeContext& resume_context() { return resume_context_; }
  const ResumeContext& resume_context() const { return resume_context_; }

  const Driver* fragment_driver() const { return fragment_driver_; }

  InspectManager& inspect_manager() { return *inspect_manager_; }
  DriverLoader& driver_loader() { return driver_loader_; }

  // This method is public only for the test suite.
  zx_status_t BindDriver(Driver* drv, const AttemptBindFunc& attempt_bind);

  // This method is public only for the LoadDriverPackageTest.
  zx_status_t LoadEphemeralDriver(internal::PackageResolverInterface* resolver,
                                  const std::string& package_url);

  uint32_t GetSuspendFlagsFromSystemPowerState(SystemPowerState state);

  // These methods are used by the DriverHost class to register in the coordinator's bookkeeping
  void RegisterDriverHost(DriverHost* dh) { driver_hosts_.push_back(dh); }
  void UnregisterDriverHost(DriverHost* dh) { driver_hosts_.erase(*dh); }

  // Returns path to driver that should be bound to fragments of composite devices.
  std::string GetFragmentDriverPath() const;

  using RegisterWithPowerManagerCompletion = fit::callback<void(zx_status_t)>;
  void RegisterWithPowerManager(fidl::ClientEnd<fuchsia_io::Directory> devfs,
                                RegisterWithPowerManagerCompletion completion);
  void RegisterWithPowerManager(
      fidl::ClientEnd<fuchsia_power_manager::DriverManagerRegistration> power_manager,
      fidl::ClientEnd<fuchsia_device_manager::SystemStateTransition> system_state_transition,
      fidl::ClientEnd<fuchsia_io::Directory> devfs, RegisterWithPowerManagerCompletion completion);
  void ScheduleBaseDriverLoading();

 private:
  CoordinatorConfig config_;
  async_dispatcher_t* const dispatcher_;
  bool running_ = false;
  bool launched_first_driver_host_ = false;
  bool power_manager_registered_ = false;
  LoaderServiceConnector loader_service_connector_;
  fidl::WireSharedClient<fuchsia_power_manager::DriverManagerRegistration> power_manager_client_;

  internal::BasePackageResolver base_resolver_;
  DriverLoader driver_loader_;

  // All Drivers
  fbl::DoublyLinkedList<std::unique_ptr<Driver>> drivers_;

  // Drivers to try last
  fbl::DoublyLinkedList<std::unique_ptr<Driver>> fallback_drivers_;

  // All DriverHosts
  fbl::DoublyLinkedList<DriverHost*> driver_hosts_;

  // All Devices (excluding static immortal devices)
  fbl::TaggedDoublyLinkedList<fbl::RefPtr<Device>, Device::AllDevicesListTag> devices_;

  // All composite devices
  fbl::DoublyLinkedList<std::unique_ptr<CompositeDevice>> composite_devices_;

  fbl::RefPtr<Device> root_device_;
  fbl::RefPtr<Device> sys_device_;

  SuspendHandler suspend_handler_;
  ResumeContext resume_context_;

  InspectManager* const inspect_manager_;
  std::unique_ptr<SystemStateManager> system_state_manager_;
  SystemPowerState shutdown_system_state_;

  void BindAllDevicesDriverIndex(const DriverLoader::MatchDeviceConfig& config);
  zx_status_t MatchAndBindDeviceDriverIndex(const fbl::RefPtr<Device>& dev,
                                            const DriverLoader::MatchDeviceConfig& config);

  // Given a device, return all of the Drivers whose bind programs match with the device.
  // The returned vector is organized by priority, so if only one driver is being bound it
  // should be the first in the vector.
  // If `drvlibname` is not empty then the device will only be checked against the driver
  // with that specific name.
  zx::status<std::vector<const Driver*>> MatchDevice(const fbl::RefPtr<Device>& dev,
                                                     std::string_view drvlibname);
  zx_status_t MatchDeviceToDriver(const fbl::RefPtr<Device>& dev, const Driver* driver,
                                  bool autobind);

  zx::status<std::vector<fuchsia_driver_development::wire::DriverInfo>> GetDriverInfo(
      fidl::AnyArena& allocator, const std::vector<const Driver*>& drivers);
  zx::status<std::vector<fuchsia_driver_development::wire::DeviceInfo>> GetDeviceInfo(
      fidl::AnyArena& allocator, const std::vector<fbl::RefPtr<Device>>& devices);

  // fuchsia.driver.development/DriverDevelopment interface
  void RestartDriverHosts(RestartDriverHostsRequestView request,
                          RestartDriverHostsCompleter::Sync& completer) override;
  void GetDriverInfo(GetDriverInfoRequestView request,
                     GetDriverInfoCompleter::Sync& completer) override;
  void GetDeviceInfo(GetDeviceInfoRequestView request,
                     GetDeviceInfoCompleter::Sync& completer) override;

  // fuchsia.device.manager/Administrator interface
  void Suspend(SuspendRequestView request, SuspendCompleter::Sync& completer) override;
  void UnregisterSystemStorageForShutdown(
      UnregisterSystemStorageForShutdownRequestView request,
      UnregisterSystemStorageForShutdownCompleter::Sync& completer) override;

  // fuchsia.device.manager/DebugDumper interface
  void DumpTree(DumpTreeRequestView request, DumpTreeCompleter::Sync& completer) override;
  void DumpDrivers(DumpDriversRequestView request, DumpDriversCompleter::Sync& completer) override;
  void DumpBindingProperties(DumpBindingPropertiesRequestView request,
                             DumpBindingPropertiesCompleter::Sync& completer) override;

  // Driver registrar interface
  void Register(RegisterRequestView request, RegisterCompleter::Sync& completer) override;

  void OnOOMEvent(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                  const zx_packet_signal_t* signal);
  async::WaitMethod<Coordinator, &Coordinator::OnOOMEvent> wait_on_oom_event_{this};

  fbl::DoublyLinkedList<std::unique_ptr<Metadata>> published_metadata_;

  // Once the special fragment driver is loaded, this will refer to it.  This
  // driver is used for binding against fragments of composite devices
  const Driver* fragment_driver_ = nullptr;

  cpp17::optional<fidl::ServerBindingRef<fuchsia_driver_registrar::DriverRegistrar>>
      driver_registrar_binding_;
  internal::PackageResolver package_resolver_;

  void DumpDevice(VmoWriter* vmo, const Device* dev, size_t indent) const;
  void DumpDeviceProps(VmoWriter* vmo, const Device* dev) const;

  void BuildSuspendList();
  void Resume(ResumeContext ctx, std::function<void(zx_status_t)> callback);

  std::unique_ptr<Driver> ValidateDriver(std::unique_ptr<Driver> drv);

  zx_status_t NewDriverHost(const char* name, fbl::RefPtr<DriverHost>* out);

  zx_status_t BindDriver(Driver* drv) {
    return BindDriver(drv, fit::bind_member(this, &Coordinator::AttemptBind));
  }
  zx_status_t AttemptBind(const Driver* drv, const fbl::RefPtr<Device>& dev);

  zx_status_t GetMetadataRecurse(const fbl::RefPtr<Device>& dev, uint32_t type, void* buffer,
                                 size_t buflen, size_t* size);

  // Schedule unbind and remove tasks for all devices in |driver_host|.
  // Used as part of RestartDriverHosts().
  void ScheduleUnbindRemoveAllDevices(fbl::RefPtr<DriverHost> driver_host);
};

bool driver_is_bindable(const Driver* drv, uint32_t protocol_id,
                        const fbl::Array<const zx_device_prop_t>& props,
                        const fbl::Array<const StrProperty>& str_props, bool autobind);

zx_status_t fidl_DirectoryWatch(void* ctx, uint32_t mask, uint32_t options, zx_handle_t raw_watcher,
                                fidl_txn_t* txn);

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_COORDINATOR_H_
