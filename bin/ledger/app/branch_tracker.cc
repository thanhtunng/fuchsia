// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/branch_tracker.h"

#include <vector>

#include "apps/ledger/src/app/diff_utils.h"
#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/callback/waiter.h"
#include "lib/ftl/functional/make_copyable.h"

namespace ledger {
class BranchTracker::PageWatcherContainer {
 public:
  PageWatcherContainer(PageWatcherPtr watcher,
                       PageManager* page_manager,
                       storage::PageStorage* storage,
                       std::unique_ptr<const storage::Commit> base_commit)
      : change_in_flight_(false),
        last_commit_(std::move(base_commit)),
        manager_(page_manager),
        storage_(storage),
        interface_(std::move(watcher)) {}

  ~PageWatcherContainer() {
    if (on_drained_) {
      on_drained_();
    }
  }

  void set_on_empty(ftl::Closure on_empty_callback) {
    interface_.set_connection_error_handler(std::move(on_empty_callback));
  }

  void UpdateCommit(std::unique_ptr<const storage::Commit> commit) {
    current_commit_ = std::move(commit);
    SendCommit();
  }

  // Sets a callback to be called when all pending updates are sent. If all
  // updates are already sent, the callback will be called immediately. This
  // callback will only be called once; |SetOnDrainedCallback| should be called
  // again to set a new callback after the first one is called. Setting a
  // callback while a previous one is still active will execute the previous
  // callback.
  void SetOnDrainedCallback(ftl::Closure on_drained) {
    // If a transaction is committed or rolled back before all watchers have
    // been drained, we do not want to continue blocking until they drain. Thus,
    // we declare them drained right away and proceed.
    if (on_drained_) {
      on_drained_();
      on_drained_ = nullptr;
    }
    on_drained_ = on_drained;
    if (Drained() && on_drained_) {
      on_drained();
      on_drained_ = nullptr;
    }
  }

 private:
  // Returns true if all changes have been sent to the watcher client, false
  // otherwise.
  bool Drained() {
    return !current_commit_ ||
           last_commit_->GetId() == current_commit_->GetId();
  }

  // Sends a commit to the watcher if needed.
  void SendCommit() {
    if (change_in_flight_) {
      return;
    }

    if (Drained()) {
      if (on_drained_) {
        on_drained_();
        on_drained_ = nullptr;
      }
      return;
    }

    change_in_flight_ = true;

    // TODO(etiennej): See LE-74: clean object ownership
    diff_utils::ComputePageChange(
        storage_, *last_commit_, *current_commit_,
        ftl::MakeCopyable([ this, new_commit = std::move(current_commit_) ](
            Status status, PageChangePtr page_change_ptr) mutable {
          if (status != Status::OK) {
            // This change notification is abandonned. At the next commit,
            // we will try again (but not before). The next notification
            // will cover both this change and the next.
            FTL_LOG(ERROR) << "Unable to compute PageChange for Watch update.";
            change_in_flight_ = false;
            return;
          }

          if (!page_change_ptr) {
            change_in_flight_ = false;
            last_commit_.swap(new_commit);
            SendCommit();
            return;
          }

          interface_->OnChange(
              std::move(page_change_ptr), ResultState::FINISHED,
              ftl::MakeCopyable([
                this, new_commit = std::move(new_commit)
              ](fidl::InterfaceRequest<PageSnapshot> snapshot_request) mutable {
                if (snapshot_request) {
                  manager_->BindPageSnapshot(new_commit->Clone(),
                                             std::move(snapshot_request));
                }
                change_in_flight_ = false;
                last_commit_.swap(new_commit);
                SendCommit();
              }));
        }));
  }

  ftl::Closure on_drained_ = nullptr;
  bool change_in_flight_;
  std::unique_ptr<const storage::Commit> last_commit_;
  std::unique_ptr<const storage::Commit> current_commit_;
  PageManager* manager_;
  storage::PageStorage* storage_;
  PageWatcherPtr interface_;
};

BranchTracker::BranchTracker(PageManager* manager,
                             storage::PageStorage* storage)
    : manager_(manager), storage_(storage), transaction_in_progress_(false) {
  watchers_.set_on_empty([this] { CheckEmpty(); });
  std::vector<storage::CommitId> commit_ids;
  // TODO(etiennej): Fail more nicely.
  FTL_CHECK(storage_->GetHeadCommitIds(&commit_ids) == storage::Status::OK);
  FTL_DCHECK(commit_ids.size() > 0);
  // current_commit_ will be updated to have a correct value after the first
  // Commit received in OnNewCommits or StopTransaction.
  current_commit_ = nullptr;
  current_commit_id_ = commit_ids[0];
  storage_->AddCommitWatcher(this);
}

BranchTracker::~BranchTracker() {
  storage_->RemoveCommitWatcher(this);
}

void BranchTracker::set_on_empty(ftl::Closure on_empty_callback) {
  on_empty_callback_ = on_empty_callback;
}

const storage::CommitId& BranchTracker::GetBranchHeadId() {
  return current_commit_id_;
}

void BranchTracker::OnNewCommits(
    const std::vector<std::unique_ptr<const storage::Commit>>& commits,
    storage::ChangeSource source) {
  bool changed = false;
  const std::unique_ptr<const storage::Commit>* new_current_commit = nullptr;
  for (const auto& commit : commits) {
    if (commit->GetId() == current_commit_id_) {
      continue;
    }
    // This assumes commits are received in (partial) order. If the commit
    // doesn't have current_commit_id_ as a parent it is not part of this branch
    // and should be ignored.
    std::vector<storage::CommitIdView> parent_ids = commit->GetParentIds();
    if (std::find(parent_ids.begin(), parent_ids.end(), current_commit_id_) ==
        parent_ids.end()) {
      continue;
    }
    changed = true;
    current_commit_id_ = commit->GetId();
    new_current_commit = &commit;
  }
  if (changed) {
    current_commit_ = (*new_current_commit)->Clone();
  }

  if (!changed || transaction_in_progress_) {
    return;
  }
  for (auto& watcher : watchers_) {
    watcher.UpdateCommit(current_commit_->Clone());
  }
}

void BranchTracker::StartTransaction(ftl::Closure watchers_drained_callback) {
  FTL_DCHECK(!transaction_in_progress_);
  transaction_in_progress_ = true;
  auto waiter = callback::CompletionWaiter::Create();
  for (auto it = watchers_.begin(); it != watchers_.end(); ++it) {
    it->SetOnDrainedCallback(waiter->NewCallback());
  }
  waiter->Finalize(std::move(watchers_drained_callback));
}

void BranchTracker::StopTransaction(
    std::unique_ptr<const storage::Commit> commit) {
  FTL_DCHECK(transaction_in_progress_ || !commit);

  if (!transaction_in_progress_) {
    return;
  }
  transaction_in_progress_ = false;

  if (commit) {
    current_commit_id_ = commit->GetId();
    current_commit_ = std::move(commit);
  }

  if (!current_commit_) {
    // current_commit_ has a null value only if OnNewCommits has neven been
    // called. Here we are in the case where a transaction stops, but no new
    // commits have arrived in between: there is no need to update the watchers.
    return;
  }

  for (auto& watcher : watchers_) {
    watcher.SetOnDrainedCallback(nullptr);
    watcher.UpdateCommit(current_commit_->Clone());
  }
}

void BranchTracker::RegisterPageWatcher(
    PageWatcherPtr page_watcher_ptr,
    std::unique_ptr<const storage::Commit> base_commit) {
  watchers_.emplace(std::move(page_watcher_ptr), manager_, storage_,
                    std::move(base_commit));
}

bool BranchTracker::IsEmpty() {
  return watchers_.empty();
}

void BranchTracker::CheckEmpty() {
  if (on_empty_callback_ && IsEmpty())
    on_empty_callback_();
}

}  // namespace ledger
