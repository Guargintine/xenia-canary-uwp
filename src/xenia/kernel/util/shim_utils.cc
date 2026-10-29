/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"

#include <algorithm>
#include <atomic>

#include "xenia/base/crash_report.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/xthread.h"
namespace xe {
namespace kernel {
namespace shim {

thread_local StringBuffer string_buffer_;

StringBuffer* thread_local_string_buffer() { return &string_buffer_; }

XThread* ContextParam::CurrentXThread() const {
  return XThread::GetCurrentThread();
}

namespace {

struct KernelCallRecord {
  const cpu::Export* export_entry;
  uint32_t caller;
  uint32_t args[4];
};

constexpr uint32_t kKernelCallTraceLength = 32;

struct KernelCallTrace {
  uint32_t host_thread_id;
  uint32_t guest_thread_handle;
  // Written only by the owning thread.
  std::atomic<uint32_t> count;
  KernelCallRecord records[kKernelCallTraceLength];
};

// Traces live as long as the process: a report may look at any of them.
constexpr uint32_t kMaxKernelCallTraces = 4096;
std::atomic<KernelCallTrace*> kernel_call_traces[kMaxKernelCallTraces];
std::atomic<uint32_t> kernel_call_trace_count{0};
thread_local KernelCallTrace* thread_kernel_call_trace = nullptr;
thread_local bool thread_kernel_call_trace_unavailable = false;

KernelCallTrace* CreateThreadTrace() {
  const uint32_t index =
      kernel_call_trace_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= kMaxKernelCallTraces) {
    thread_kernel_call_trace_unavailable = true;
    return nullptr;
  }
  auto trace = new KernelCallTrace();
  trace->host_thread_id = xe::threading::current_thread_system_id();
  XThread* thread = XThread::TryGetCurrentThread();
  trace->guest_thread_handle = thread ? thread->handle() : 0;
  trace->count.store(0, std::memory_order_relaxed);
  kernel_call_traces[index].store(trace, std::memory_order_release);
  thread_kernel_call_trace = trace;
  return trace;
}

}  // namespace

void TraceKernelCall(const cpu::Export* export_entry,
                     const PPCContext* ppc_context) {
  KernelCallTrace* trace = thread_kernel_call_trace;
  if (!trace) {
    if (thread_kernel_call_trace_unavailable) {
      return;
    }
    trace = CreateThreadTrace();
    if (!trace) {
      return;
    }
  }
  const uint32_t count = trace->count.load(std::memory_order_relaxed);
  KernelCallRecord& record = trace->records[count % kKernelCallTraceLength];
  record.export_entry = export_entry;
  record.caller = uint32_t(ppc_context->lr);
  for (uint32_t i = 0; i < 4; ++i) {
    record.args[i] = uint32_t(ppc_context->r[3 + i]);
  }
  trace->count.store(count + 1, std::memory_order_release);
}

void DescribeRecentKernelCalls(uint32_t host_thread_id,
                               crash_report::Writer& out) {
  const uint32_t trace_count =
      std::min(kernel_call_trace_count.load(std::memory_order_acquire),
               kMaxKernelCallTraces);
  const KernelCallTrace* trace = nullptr;
  // Thread IDs are reused; the newest trace is this thread's.
  for (uint32_t i = trace_count; i-- > 0;) {
    const KernelCallTrace* candidate =
        kernel_call_traces[i].load(std::memory_order_acquire);
    if (candidate && candidate->host_thread_id == host_thread_id) {
      trace = candidate;
      break;
    }
  }
  if (!trace) {
    out.AppendText("No kernel calls recorded on this thread.\n");
    return;
  }
  const uint32_t count = trace->count.load(std::memory_order_acquire);
  const uint32_t shown = std::min(count, kKernelCallTraceLength);
  out.Append(
      "Last {} of {} kernel calls on this thread (guest thread {:08X}), oldest "
      "first:\n",
      shown, count, trace->guest_thread_handle);
  for (uint32_t i = count - shown; i < count; ++i) {
    const KernelCallRecord& record = trace->records[i % kKernelCallTraceLength];
    out.Append("  {}({:08X}, {:08X}, {:08X}, {:08X}) from {:08X}\n",
               record.export_entry ? record.export_entry->name : "?",
               record.args[0], record.args[1], record.args[2], record.args[3],
               record.caller);
  }
}

void LogGuestEvent(std::string_view what, const PPCContext* ppc_context) {
  char buffer[8192];
  crash_report::Writer out(buffer, sizeof(buffer));
  out.Append("{} (guest caller {:08X})\n", what, uint32_t(ppc_context->lr));
  DescribeRecentKernelCalls(xe::threading::current_thread_system_id(), out);
  xe::logging::AppendLogLine(xe::LogLevel::Warning,
                             xe::logging::kPrefixCharWarning, out.view());
  xe::FlushLog();
}

}  // namespace shim
}  // namespace kernel
}  // namespace xe
