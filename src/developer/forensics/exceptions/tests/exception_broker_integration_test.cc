// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <fuchsia/exception/cpp/fidl.h>
#include <fuchsia/feedback/testing/cpp/fidl.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <gtest/gtest.h>

#include "src/developer/forensics/exceptions/exception_broker.h"
#include "src/developer/forensics/exceptions/tests/crasher_wrapper.h"
#include "src/lib/fxl/test/test_settings.h"

namespace forensics {
namespace exceptions {
namespace {

using fuchsia::exception::ExceptionInfo;
using fuchsia::exception::ExceptionType;

bool GetExceptionContext(ExceptionContext* pe) {
  // Create a process that crashes and obtain the relevant handles and exception.
  // By the time |SpawnCrasher| has returned, the process has already thrown an exception.
  if (!SpawnCrasher(pe))
    return false;

  // We mark the exception to be handled. We need this because we pass on the exception to the
  // handler, which will resume it before we get the control back. If we don't mark it as handled,
  // the exception will bubble out of our environment.
  uint32_t state = ZX_EXCEPTION_STATE_HANDLED;
  if (zx_status_t res = pe->exception.set_property(ZX_PROP_EXCEPTION_STATE, &state, sizeof(state));
      res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Could not set handled state to exception.";
    return false;
  }

  return true;
}

ExceptionInfo ExceptionContextToExceptionInfo(const ExceptionContext& pe) {
  // Translate the exception to the fidl format.
  ExceptionInfo exception_info;
  exception_info.process_koid = pe.exception_info.pid;
  exception_info.thread_koid = pe.exception_info.tid;
  exception_info.type = static_cast<ExceptionType>(pe.exception_info.type);

  return exception_info;
}

TEST(ExceptionBrokerIntegrationTest, DISABLED_OnExceptionSmokeTest) {
  constexpr size_t kNumExceptions = 5;
  std::vector<ExceptionContext> exceptions(kNumExceptions);

  fuchsia::exception::HandlerSyncPtr exception_handler;

  auto environment_services = sys::ServiceDirectory::CreateFromNamespace();
  environment_services->Connect(exception_handler.NewRequest());

  for (size_t i = 0; i < kNumExceptions; ++i) {
    auto& exception = exceptions[i];
    ASSERT_TRUE(GetExceptionContext(&exception));

    ASSERT_EQ(exception_handler->OnException(std::move(exception.exception),
                                             ExceptionContextToExceptionInfo(exception)),
              ZX_OK);
  }

  fuchsia::feedback::testing::FakeCrashReporterQuerierSyncPtr crash_reporter;
  environment_services->Connect(crash_reporter.NewRequest());

  size_t num_crashreports{0};
  for (size_t i = 0; i < kNumExceptions; ++i) {
    ASSERT_EQ(crash_reporter->WatchFile(&num_crashreports), ZX_OK);
    if (num_crashreports == kNumExceptions) {
      break;
    }
  }

  EXPECT_EQ(num_crashreports, kNumExceptions);

  for (auto& exception : exceptions) {
    // Kill the job so that the exception that will be freed here doesn't bubble out of the test
    // environment.
    exception.job.kill();
  }
}

TEST(ExceptionBrokerIntegrationTest, GetProcessesOnExceptionSmokeTest) {
  ExceptionContext pe;
  ASSERT_TRUE(GetExceptionContext(&pe));

  fuchsia::exception::ProcessLimboSyncPtr limbo;

  auto environment_services = sys::ServiceDirectory::CreateFromNamespace();
  environment_services->Connect(limbo.NewRequest());

  fuchsia::exception::ProcessLimbo_WatchProcessesWaitingOnException_Result result;
  zx_status_t status = limbo->WatchProcessesWaitingOnException(&result);
  ASSERT_EQ(status, ZX_OK) << zx_status_get_string(status);

  // Kill the job so that the exception that will be freed here doesn't bubble out of the test
  // environment.
  pe.job.kill();
}

}  // namespace
}  // namespace exceptions
}  // namespace forensics

int main(int argc, char* argv[]) {
  if (!fxl::SetTestSettings(argc, argv))
    return EXIT_FAILURE;

  testing::InitGoogleTest(&argc, argv);
  syslog::SetTags({"exception-broker", "integration-test"});

  return RUN_ALL_TESTS();
}
