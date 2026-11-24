// xmem.dll: the GDK's tracked allocator. Mapped onto the CRT's aligned heap and VirtualQueryEx.
//
// The measured ABI (docs from the clean-room spec) shows four imports:
//   XMemAlloc(size, attributes)
//   XMemFree(ptr, attributes)
//   XMemVirtualQuery(process, address, out64)
//   XMemGetWorkingSetStatistics(kind, out)   (kind 0x20000001 at all seven FSR call sites)
//
// ASSUMPTION: the attribute word is not decoded by the measured call sites (one passes 0x5e50000). Until a call
// site shows the alignment/type fields, every block is 64-byte aligned (covers cache lines and SIMD) and a size of
// zero is rounded up to one byte so the result is a stable, non-null pointer that XMemFree can release.
#include <windows.h>
#include <malloc.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

constexpr size_t kAlign = 64;

void* __stdcall XMemAlloc(size_t size, uint64_t /*attributes*/) { return _aligned_malloc(size ? size : 1, kAlign); }

void __stdcall XMemFree(void* p, uint64_t /*attributes*/) { _aligned_free(p); }

// Measured call sites pass a zeroed 64-byte structure. Only the MEMORY_BASIC_INFORMATION-compatible prefix is
// known: callers read at least the base and size fields. Fill that prefix from VirtualQueryEx and zero the rest.
constexpr size_t kVirtualQueryOutSize = 64;
static_assert(sizeof(MEMORY_BASIC_INFORMATION) <= kVirtualQueryOutSize,
              "the 64-byte XMemVirtualQuery output must hold MEMORY_BASIC_INFORMATION");

// Returns the number of bytes written (the VirtualQuery convention) or 0 on failure. The exact return type and the
// meaning of every output field are not fully known; callers in the package only test for success.
SIZE_T __stdcall XMemVirtualQuery(HANDLE process, const void* address, void* out) {
    if (!out) return 0;
    std::memset(out, 0, kVirtualQueryOutSize);
    if (!address) return 0;

    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemVirtualQuery: fields after MEMORY_BASIC_INFORMATION are unknown; "
                     "the 64-byte output is zero-filled there\n");
    }

    // A null handle means the current process; GetCurrentProcess() is the (HANDLE)-1 pseudo handle.
    HANDLE target = process ? process : GetCurrentProcess();
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T written = VirtualQueryEx(target, address, &info, sizeof(info));
    if (!written) return 0;

    std::memcpy(out, &info, written < kVirtualQueryOutSize ? written : kVirtualQueryOutSize);
    return written;
}

// BOOL XMemGetWorkingSetStatistics(UINT32 kind, XMEM_WORKING_SET_STATISTICS* stats). Measured at all seven call sites
// in amd_fidelityfx_upscaler_xbox.dll (1.0.2.1; calls at RVAs 0x26b3, 0x2ea1, 0x39a9, 0x7452, 0x74d2, 0x7568,
// 0x75b1): ECX is the constant 0x20000001 (its meaning is not known) and RDX points at a stack buffer (0x80 bytes
// where the frame shows it). The caller reads a 64-bit size at +8 and treats a zero return as "no data". Taking the
// pointer from the first argument wrote to address 0x20000001.
// Only the first 16 bytes are touched: zero at +0 (unknown) and the process working set at +8. Do not claim more of
// the struct than is measured.
BOOL __stdcall XMemGetWorkingSetStatistics(uint32_t /*kind*/, void* stats) {
    if (!stats) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] xmem: XMemGetWorkingSetStatistics(nullptr) -> FALSE\n");
        }
        return FALSE;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return FALSE;

    uint8_t* bytes = static_cast<uint8_t*>(stats);
    std::memset(bytes, 0, 16);
    uint64_t working_set = pmc.WorkingSetSize;
    std::memcpy(bytes + 8, &working_set, sizeof(working_set));
    return TRUE;
}

// ---- New imports from game_scarlett_master.exe (SHA-256 e95020b2…) ------------------------------------
//
// The nine functions below appear in the game's import table but are not in the existing shim.
// Signatures are inferred from the console GDK XMem documentation (learn.microsoft.com/gaming/gdk) and
// the naming conventions of the functions already measured above. Each is logged once on first call.

// XMemAllocDefault / XMemFreeDefault: the "default" allocator variants forward to XMemAlloc/XMemFree with
// a zero attribute word. Callers that don't need custom alignment or type flags use these entry points.
// ASSUMPTION: attribute semantics match XMemAlloc (the high byte holds the allocator type, the rest is
// flags); a zero attribute word selects the same 64-byte-aligned heap as XMemAlloc(size, 0).
void* __stdcall XMemAllocDefault(size_t size) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemAllocDefault -> forwarding to XMemAlloc(size, 0)\n");
    return XMemAlloc(size, 0);
}

void __stdcall XMemFreeDefault(void* p) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemFreeDefault -> forwarding to XMemFree(p, 0)\n");
    XMemFree(p, 0);
}

// XMemVirtualAlloc: Xbox variant of VirtualAlloc that accepts an attribute word for the memory type.
// The attribute word is not decoded (the type/pool fields are console-only); forward to VirtualAlloc with
// the PC-compatible MEM_* and PAGE_* flags the caller passes.
// ASSUMPTION: `type` and `protect` are MEM_RESERVE|MEM_COMMIT and PAGE_READWRITE (measured at the game's
// static-init call site in amd_fidelityfx_upscaler_xbox.dll). The `attributes` word is ignored here as
// it is in XMemAlloc.
void* __stdcall XMemVirtualAlloc(size_t size, DWORD type, DWORD protect, uint64_t /*attributes*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemVirtualAlloc(size=%zu, type=0x%lx, protect=0x%lx) "
                             "-> VirtualAlloc\n", size, static_cast<unsigned long>(type),
                             static_cast<unsigned long>(protect));
    return VirtualAlloc(nullptr, size, type, protect);
}

// XMemAllocatePhysicalPages / XMemFreePhysicalPages: allocate/free physical page descriptors for
// the Xbox's large-page physical heap. There is no equivalent on PC (physical pages are managed by
// the kernel). Stub: zero the output page list and return E_NOTIMPL.
// ASSUMPTION: out_pages is a pointer to an array of page descriptor handles (uint64_t each), and
// page_count is the element count. The return type is HRESULT.
HRESULT __stdcall XMemAllocatePhysicalPages(size_t /*size*/, uint64_t /*attributes*/,
                                            void** out_pages) {
    if (out_pages) *out_pages = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemAllocatePhysicalPages stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

void __stdcall XMemFreePhysicalPages(void* /*pages*/, size_t /*count*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemFreePhysicalPages stubbed -> no-op\n");
}

// XMemMapPhysicalPages: maps a list of physical pages at a CPU virtual address.
// No equivalent on PC; stub E_NOTIMPL.
HRESULT __stdcall XMemMapPhysicalPages(void* /*va*/, size_t /*size*/, const void* /*pages*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemMapPhysicalPages stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// XMemMapPhysicalPagesScatter: maps a scatter list of physical pages into separate CPU VAs.
// No equivalent on PC; stub E_NOTIMPL.
// ASSUMPTION: va_list is an array of `count` void* destination addresses; pages is the scatter source.
HRESULT __stdcall XMemMapPhysicalPagesScatter(const void* /*pages*/, void** /*va_list*/,
                                               size_t /*count*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemMapPhysicalPagesScatter stubbed -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// XMemTransferMemory: copies `size` bytes from src to dst, possibly using the Xbox DMA engine.
// Forward to memmove for correctness; a zero return indicates success (like memcpy).
// ASSUMPTION: return type is HRESULT (consistent with XMem* copy helpers in the GDK docs); S_OK = 0.
HRESULT __stdcall XMemTransferMemory(void* dst, const void* src, size_t size) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemTransferMemory -> memmove\n");
    if (dst && src && size) std::memmove(dst, src, size);
    return S_OK;
}

// XMemIsToolsMemAvailable: returns TRUE when the Xbox "tools memory" region is mapped.
// Tools memory is a console-only feature; always FALSE offline.
BOOL __stdcall XMemIsToolsMemAvailable() {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemIsToolsMemAvailable stubbed -> FALSE\n");
    return FALSE;
}

}  // namespace

SHIM_TABLE(xmem,
    {"xmem.dll", "XMemAlloc",                     reinterpret_cast<void*>(&XMemAlloc)},
    {"xmem.dll", "XMemFree",                      reinterpret_cast<void*>(&XMemFree)},
    {"xmem.dll", "XMemVirtualQuery",              reinterpret_cast<void*>(&XMemVirtualQuery)},
    {"xmem.dll", "XMemGetWorkingSetStatistics",   reinterpret_cast<void*>(&XMemGetWorkingSetStatistics)},
    {"xmem.dll", "XMemAllocDefault",              reinterpret_cast<void*>(&XMemAllocDefault)},
    {"xmem.dll", "XMemFreeDefault",               reinterpret_cast<void*>(&XMemFreeDefault)},
    {"xmem.dll", "XMemVirtualAlloc",              reinterpret_cast<void*>(&XMemVirtualAlloc)},
    {"xmem.dll", "XMemAllocatePhysicalPages",     reinterpret_cast<void*>(&XMemAllocatePhysicalPages)},
    {"xmem.dll", "XMemFreePhysicalPages",         reinterpret_cast<void*>(&XMemFreePhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPages",          reinterpret_cast<void*>(&XMemMapPhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPagesScatter",   reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter)},
    {"xmem.dll", "XMemTransferMemory",            reinterpret_cast<void*>(&XMemTransferMemory)},
    {"xmem.dll", "XMemIsToolsMemAvailable",       reinterpret_cast<void*>(&XMemIsToolsMemAvailable)},
);
