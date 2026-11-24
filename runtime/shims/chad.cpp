// chad.dll: Xbox DSP (XDsp) command-submission interface.
//
// The Xbox Series "chad" subsystem is the firmware/DSP co-processor used for audio effects processing.
// Its five exported functions submit command buffers to the DSP and manage its activation lifecycle.
// All are offline stubs: they log once and return E_NOTIMPL or no-op.
//
// ABI measured from the game_scarlett_master.exe import table (SHA-256 e95020b2…). No public SDK headers
// exist for these; signatures are inferred from the console GDK DSP documentation and naming conventions.
//
// ASSUMPTION: Command buffers are described by a pointer + size pair (consistent with how XAudio2 passes
// effect chains on Xbox). The context handle is an opaque pointer allocated by XDspActivate. Revisit when
// a measured call site shows the struct layout.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "shims.h"

namespace {

// XDspActivate: initialises the DSP, allocating a context and an internal channel table.
// params is an opaque descriptor (size unknown). out_ctx receives the context handle on success.
// Offline: zero the output, return E_NOTIMPL.
HRESULT WINAPI XDspActivate(const void* /*params*/, void** out_ctx) {
    if (out_ctx) *out_ctx = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] chad: XDspActivate stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// XDspDeactivate: tears down the DSP context. No-op offline (nothing was allocated).
void WINAPI XDspDeactivate(void* /*ctx*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] chad: XDspDeactivate stubbed -> no-op\n");
}

// XDspConnectWithMaximumStreamLimit: connects the DSP context with a cap on simultaneous streams.
// out receives a stream handle. Offline: zero and E_NOTIMPL.
HRESULT WINAPI XDspConnectWithMaximumStreamLimit(void* /*ctx*/, uint32_t /*limit*/, void** out) {
    if (out) *out = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] chad: XDspConnectWithMaximumStreamLimit stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// XDspDisconnect: closes a stream connection opened by XDspConnectWithMaximumStreamLimit.
// No-op offline.
void WINAPI XDspDisconnect(void* /*ctx*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] chad: XDspDisconnect stubbed -> no-op\n");
}

// XDspSubmitCommand: submits a command buffer of `size` bytes to the DSP for processing.
// Offline: E_NOTIMPL (the DSP is not present).
HRESULT WINAPI XDspSubmitCommand(void* /*ctx*/, const void* /*cmd*/, uint32_t /*size*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] chad: XDspSubmitCommand stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

}  // namespace

SHIM_TABLE(chad,
    {"chad.dll", "XDspActivate",                       reinterpret_cast<void*>(&XDspActivate)},
    {"chad.dll", "XDspDeactivate",                     reinterpret_cast<void*>(&XDspDeactivate)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit",  reinterpret_cast<void*>(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspDisconnect",                     reinterpret_cast<void*>(&XDspDisconnect)},
    {"chad.dll", "XDspSubmitCommand",                  reinterpret_cast<void*>(&XDspSubmitCommand)},
);
