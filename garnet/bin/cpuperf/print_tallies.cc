// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <lib/fxl/command_line.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/log_settings.h>
#include <lib/fxl/log_settings_command_line.h>
#include <lib/fxl/logging.h>

#include <lib/zircon-internal/device/cpu-trace/cpu-perf.h>
#include <zircon/syscalls.h>

#include "garnet/lib/cpuperf/controller.h"
#include "garnet/lib/cpuperf/events.h"
#include "garnet/lib/debugger_utils/util.h"

#include "session_spec.h"
#include "session_result_spec.h"

// Allow space for 999,999,999.
// This is |int| as it is used as the width arg to fprintf.
constexpr int kMinColumnWidth = 11;

// Width of the first column, which is trace names.
// This is |int| as it is used as the width arg to fprintf.
constexpr int kTraceNameColumnWidth = sizeof("Trace NNN:") - 1;

struct EventColumn {
  const char* name;
  int width;
};

using SessionColumns = std::unordered_map<cpuperf_event_id_t, EventColumn>;

struct EventResult {
  uint64_t value_or_count;
};

using TraceResults = std::unordered_map<cpuperf_event_id_t, EventResult>;

// Indexed by trace number.
using SessionResults = std::vector<TraceResults>;

template<typename T>
void IterateOverEventIds(const cpuperf::SessionSpec& spec, T func) {
  for (size_t i = 0; i < CPUPERF_MAX_EVENTS; ++i) {
    cpuperf_event_id_t id = spec.cpuperf_config.events[i];
    if (id == 0) {
      // End of present events.
      break;
    }

    const cpuperf::EventDetails* details = nullptr;
    if (!EventIdToEventDetails(id, &details)) {
      // This shouldn't happen, but let |func| decide what to do.
    }

    func(i, id, details);
  }
}

static SessionColumns BuildSessionColumns(const cpuperf::SessionSpec& spec) {
  SessionColumns columns;

  IterateOverEventIds(spec, [&columns] (size_t index, cpuperf_event_id_t id,
                                        const cpuperf::EventDetails* details) {
    const char* name;
    if (details) {
      name = details->name;
    } else {
      // This shouldn't happen, but better to print what we have.
      name = "Unknown";
    }
    size_t len = strlen(name);
    FXL_DCHECK(len <= std::numeric_limits<int>::max());
    int int_len = static_cast<int>(len);
    int width = std::max(int_len, kMinColumnWidth);

    columns[id] = EventColumn{ .name = name, .width = width };
  });

  return columns;
}

// Data is printed in the order it appears in |spec|.
static void PrintColumnTitles(FILE* f, const cpuperf::SessionSpec& spec,
                              const SessionColumns& columns) {
  fprintf(f, "%*s", kTraceNameColumnWidth, "");

  IterateOverEventIds(spec, [&] (size_t index, cpuperf_event_id_t id,
                                 const cpuperf::EventDetails* details) {
    auto iter = columns.find(id);
    FXL_DCHECK(iter != columns.end());
    const EventColumn& column = iter->second;
    fprintf(f, "|%*s", column.width, column.name);
  });

  fprintf(f, "\n");
}

static void PrintTrace(FILE* f, const cpuperf::SessionSpec& spec,
                       const cpuperf::SessionResultSpec& result_spec,
                       const SessionColumns& columns, uint32_t trace_num,
                       const TraceResults& results) {
  char label[kTraceNameColumnWidth + 1];
  snprintf(&label[0], sizeof(label), "Trace %u:", trace_num);
  fprintf(f, "%-*s", kTraceNameColumnWidth, label);

  IterateOverEventIds(spec, [&] (size_t index, cpuperf_event_id_t id,
                                 const cpuperf::EventDetails* details) {
    auto column_iter = columns.find(id);
    FXL_DCHECK(column_iter != columns.end());
    const EventColumn& column = column_iter->second;
    auto result_iter = results.find(id);
    if (result_iter != results.end()) {
      const EventResult& result = result_iter->second;
      std::stringstream ss;
      // Print 123456 as 123,456 (locale appropriate).
      struct threes : std::numpunct<char> {
        std::string do_grouping() const { return "\3"; }
      };
      ss.imbue(std::locale(ss.getloc(), new threes));
      ss << result.value_or_count;
      fprintf(f, "|%*s", column.width, ss.str().c_str());
    } else {
      // Misc events might not be present in all traces.
      // Just print blanks.
      // TODO(dje): Distinguish such properties in EventDetails?
      fprintf(f, "|%*s", column.width, "");
    }
  });

  fprintf(f, "\n");
}

void PrintTallyResults(FILE* f, const cpuperf::SessionSpec& spec,
                       const cpuperf::SessionResultSpec& result_spec,
                       cpuperf::Controller* controller) {
  std::unique_ptr<cpuperf::DeviceReader> reader = controller->GetReader();
  if (!reader) {
    return;
  }

  SessionColumns columns = BuildSessionColumns(spec);

  SessionResults results;
  for (size_t i = 0; i < result_spec.num_traces; ++i) {
    results.emplace_back(TraceResults{});
  }

  constexpr uint32_t kCurrentTraceUnset = ~0;
  uint32_t current_trace = kCurrentTraceUnset;

  uint32_t trace;
  cpuperf::SampleRecord record;
  cpuperf::ReaderStatus status;
  while ((status = reader->ReadNextRecord(&trace, &record)) ==
         cpuperf::ReaderStatus::kOk) {
    if (trace != current_trace) {
      current_trace = trace;
    }

    if (record.header->event == 0)
      continue;
    cpuperf_event_id_t id = record.header->event;
    const cpuperf::EventDetails* details;
    if (!EventIdToEventDetails(id, &details)) {
      FXL_LOG(WARNING) << "Unknown event: 0x" << std::hex
                       << record.header->event;
      continue;
    }

    switch (record.type()) {
    case CPUPERF_RECORD_COUNT:
      results[current_trace][id] = EventResult{record.count->count};
      break;
    case CPUPERF_RECORD_VALUE:
      results[current_trace][id] = EventResult{record.value->value};
      break;
    default:
      break;
    }
  }

  PrintColumnTitles(f, spec, columns);

  for (uint32_t i = 0; i < result_spec.num_traces; ++i) {
    PrintTrace(f, spec, result_spec, columns, i, results[i]);
  }
}
