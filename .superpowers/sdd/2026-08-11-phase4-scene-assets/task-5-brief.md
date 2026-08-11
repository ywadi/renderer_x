### Task 5: Public log sink (seed 13, card #17)

**Files:** Modify `src/rx_material/include/rx_material/rx_api.h` (C types: `RxLogSeverity` enum, `RxLogCallback` fn-ptr typedef, `extern "C" RxResult rxSetLogCallback(RxLogCallback cb, void* userData)`), `api_impl.cpp` + new spdlog forwarding sink `src/rx_core/log_forward_sink.{h,cpp}`; tests.
**Rules:** callback receives (severity, category cstring, message cstring, userData); invocation wrapped in catch-all (a throwing callback is swallowed + disabled with one console warning); may fire from any thread (documented); nullptr cb restores console-only; header self-containment test extended.
**Steps:** device-free tests (install/uninstall, capture of a logged message incl. from a worker thread via rx_task, throwing-callback disable path) → implement → both presets → commit.

