// AcpHal.dll / xapu.dll: Xbox audio shape-context HAL (ACP) and APU command queue.
//
// AcpHal.dll exports nine functions that front the Xbox audio DSP/APU hardware. None of the shapes or heaps
// exist on PC; the shims log once and return E_NOTIMPL or no-op as appropriate.
//
// ABI measured from the game_scarlett_master.exe import table (SHA-256 e95020b2…). No SDK headers for these
// functions are available; signatures are inferred from their names and the console GDK ACP documentation.
// Every out-pointer parameter is zeroed before returning so callers that test *out != null don't crash.
//
// ASSUMPTION: The shape-context count and heap-size arguments are 32-bit integers (consistent with the GDK
// APU heap APIs); the heap pointer argument to Apu* calls is a void*. Revisit when a measured call site
// shows the struct layout.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "shims.h"

namespace {

// ---- AcpHal.dll ---------------------------------------------------------------------------------------

// AcpHalCreate: allocates the ACP HAL object.
// out_hal receives the opaque HAL pointer on success. Offline stub: return E_NOTIMPL, zero the output.
HRESULT WINAPI AcpHalCreate(void** out_hal) {
    if (out_hal) *out_hal = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: AcpHalCreate stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// AcpHalAllocateShapeContexts: allocates `count` shape context objects on the HAL.
// out receives the array pointer on success. Offline stub: zero the output and return E_NOTIMPL.
HRESULT WINAPI AcpHalAllocateShapeContexts(void* /*hal*/, uint32_t /*count*/, void** out) {
    if (out) *out = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: AcpHalAllocateShapeContexts stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// AcpHalReleaseShapeContexts: releases a previously allocated shape-context array.
// No-op offline (nothing was allocated).
void WINAPI AcpHalReleaseShapeContexts(void* /*hal*/, void* /*ctx*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: AcpHalReleaseShapeContexts stubbed -> no-op\n");
}

// ---- xapu.dll — APU heap and address mapping ----------------------------------------------------------
//
// The APU (audio processing unit) on Xbox Series has its own GDDR6 heap distinct from VRAM. These functions
// manage allocation and virtual-address mapping into it. All are no-ops offline.

// ApuCreateHeap: creates an APU heap of `size` bytes with optional `flags`.
// out_heap receives the heap handle on success. Offline: zero and E_NOTIMPL.
HRESULT WINAPI ApuCreateHeap(uint64_t /*size*/, uint32_t /*flags*/, void** out_heap) {
    if (out_heap) *out_heap = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: ApuCreateHeap stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// ApuAlloc: allocates `size` bytes with `align` alignment from an APU heap.
// out receives the APU virtual address on success. Offline: zero and E_NOTIMPL.
HRESULT WINAPI ApuAlloc(void* /*heap*/, uint64_t /*size*/, uint32_t /*align*/, void** out) {
    if (out) *out = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: ApuAlloc stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// ApuFree: releases a previously allocated APU block. No-op offline.
void WINAPI ApuFree(void* /*heap*/, void* /*ptr*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: ApuFree stubbed -> no-op\n");
}

// ApuIsVirtualAddressValid: returns non-zero when the pointer lies in an APU-mapped region.
// Offline: always FALSE (nothing is APU-mapped).
BOOL WINAPI ApuIsVirtualAddressValid(const void* /*ptr*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: ApuIsVirtualAddressValid stubbed -> FALSE\n");
    return FALSE;
}

// ApuMapVirtualAddress: maps a CPU virtual address range into the APU's address space.
// Offline: E_NOTIMPL.
HRESULT WINAPI ApuMapVirtualAddress(void* /*va*/, uint64_t /*size*/, uint32_t /*flags*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: ApuMapVirtualAddress stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// ApuMapApuAddress: maps an APU virtual address range into the CPU address space.
// Offline: E_NOTIMPL.
HRESULT WINAPI ApuMapApuAddress(void* /*apu_va*/, uint64_t /*size*/, uint32_t /*flags*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] acphal: ApuMapApuAddress stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

}  // namespace

// AcpHal.dll and xapu.dll are registered as separate owned modules so LoadLibrary intercepts both names.
SHIM_TABLE(acphal,
    {"AcpHal.dll", "AcpHalCreate",                reinterpret_cast<void*>(&AcpHalCreate)},
    {"AcpHal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"AcpHal.dll", "AcpHalReleaseShapeContexts",  reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"xapu.dll",   "ApuCreateHeap",               reinterpret_cast<void*>(&ApuCreateHeap)},
    {"xapu.dll",   "ApuAlloc",                    reinterpret_cast<void*>(&ApuAlloc)},
    {"xapu.dll",   "ApuFree",                     reinterpret_cast<void*>(&ApuFree)},
    {"xapu.dll",   "ApuIsVirtualAddressValid",    reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"xapu.dll",   "ApuMapVirtualAddress",        reinterpret_cast<void*>(&ApuMapVirtualAddress)},
    {"xapu.dll",   "ApuMapApuAddress",            reinterpret_cast<void*>(&ApuMapApuAddress)},
);
