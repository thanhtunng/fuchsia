// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_QUEUE_H_
#define SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_QUEUE_H_

#include <lib/async/dispatcher.h>

#include <map>
#include <unordered_map>
#include <vector>

#include "src/developer/forensics/crash_reports/crash_server.h"
#include "src/developer/forensics/crash_reports/info/info_context.h"
#include "src/developer/forensics/crash_reports/info/queue_info.h"
#include "src/developer/forensics/crash_reports/log_tags.h"
#include "src/developer/forensics/crash_reports/network_watcher.h"
#include "src/developer/forensics/crash_reports/report.h"
#include "src/developer/forensics/crash_reports/report_id.h"
#include "src/developer/forensics/crash_reports/settings.h"
#include "src/developer/forensics/crash_reports/store.h"
#include "src/lib/fxl/macros.h"

namespace forensics {
namespace crash_reports {

// Queues pending reports and processes them according to its internal State.
class Queue {
 public:
  Queue(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
        std::shared_ptr<InfoContext> info_context, LogTags* tags, CrashServer* crash_server,
        SnapshotManager* snapshot_manager);

  // Allows the queue's functionality to change based on the upload policy.
  void WatchSettings(Settings* settings);

  // Allows the queue's functionality to change based on the network status.
  void WatchNetwork(NetworkWatcher* network_watcher);

  // Add a report to the queue.
  bool Add(Report report);

  // Processes the pending reports based on the queue's internal state. Returns the number of
  // reports successfully processed.
  //
  // If a report is left as pending, it is not counted as being successfully processed.
  size_t ProcessAll();

  uint64_t Size() const { return pending_reports_.size(); }
  bool IsEmpty() const { return pending_reports_.empty(); }
  bool Contains(ReportId report_id) const;
  ReportId LatestReport() { return pending_reports_.back(); }

 private:
  // How the queue should handle processing existing pending reports and new reports.
  enum class State {
    Archive,
    Upload,
    LeaveAsPending,
  };

  // Archives all pending reports and clears the queue. Returns the number of reports successfully
  // archived.
  size_t ArchiveAll();

  // Attempts to upload all pending reports and removes the successfully uploaded reports from the
  // queue. Returns the number of reports successfully uploaded.
  size_t UploadAll();

  // Attempts to upload a report
  //
  // Returns false if another upload attempt should be made in the future.
  bool Upload(const Report& report);

  void GarbageCollect(ReportId report_id);

  // Free resources associated with a report, e.g., snapshot or log tags.
  void FreeResources(ReportId report_id);
  void FreeResources(const Report& report);

  // Schedules ProcessAll() to run every 15 minutes.
  void ProcessAllEveryFifteenMinutes();

  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  LogTags* tags_;
  Store store_;
  CrashServer* crash_server_;
  SnapshotManager* snapshot_manager_;
  QueueInfo info_;

  State state_ = State::LeaveAsPending;

  std::vector<ReportId> pending_reports_;

  // Number of upload attempts within the current instance of the component. These get reset across
  // restarts and reboots.
  std::unordered_map<ReportId, uint64_t> upload_attempts_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Queue);
};

}  // namespace crash_reports
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_QUEUE_H_
