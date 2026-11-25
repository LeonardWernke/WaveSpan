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
// Measured at game_scarlett_master.exe: 5 arguments (address, size, type, protect, attributes).
// Mask `type` to PC-supported allocation flags so console-only flags like MEM_LARGE_PAGES (0x20000000) and
// 0x00400000 don't cause VirtualAlloc to fail; `attributes` is ignored.
void* __stdcall XMemVirtualAlloc(void* address, size_t size, DWORD type, DWORD protect, uint64_t /*attributes*/) {
    constexpr DWORD kPcAllocFlags =
        MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_RESET | MEM_RESET_UNDO | MEM_WRITE_WATCH;
    constexpr DWORD kPlaceholderFlags = MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER;
    const DWORD pc_type = type & (kPcAllocFlags | kPlaceholderFlags);
    if (pc_type & kPlaceholderFlags) {
        // A regular reservation cannot be split by VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER).
        static const auto alloc2 = reinterpret_cast<decltype(&VirtualAlloc2)>(
            GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "VirtualAlloc2"));
        if (!alloc2) {
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            return nullptr;
        }
        const DWORD page_protection = (pc_type & MEM_COMMIT) ? protect : PAGE_NOACCESS;
        void* allocation = alloc2(GetCurrentProcess(), address, size, pc_type, page_protection, nullptr, 0);
        if (!allocation && (pc_type & MEM_COMMIT) && GetLastError() == ERROR_INVALID_PARAMETER)
            allocation = alloc2(GetCurrentProcess(), address, size, pc_type, PAGE_READWRITE, nullptr, 0);
        return allocation;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemVirtualAlloc(address=%p, size=0x%llx, type=0x%lx, protect=0x%lx) "
                     "-> VirtualAlloc(pc_type=0x%lx)\n",
                     address, static_cast<unsigned long long>(size), static_cast<unsigned long>(type),
                     static_cast<unsigned long>(protect), static_cast<unsigned long>(pc_type));
    }
    void* p = VirtualAlloc(address, size, pc_type, protect);
    if (!p && GetLastError() == ERROR_INVALID_PARAMETER) {
        const DWORD fallback_protect = (pc_type & MEM_COMMIT) ? PAGE_READWRITE : PAGE_NOACCESS;
        p = VirtualAlloc(address, size, pc_type, fallback_protect);
    }
    if (!p) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemVirtualAlloc FAILED(address=%p, size=0x%llx, type=0x%lx, protect=0x%lx, err=%lu)\n",
                     address, static_cast<unsigned long long>(size), static_cast<unsigned long>(type),
                     static_cast<unsigned long>(protect), GetLastError());
    }
    return p;
}

// XMemAllocatePhysicalPages / XMemFreePhysicalPages: allocate/free physical 64 KB page frame numbers
// (mirroring Win32 AllocateUserPhysicalPages / FreeUserPhysicalPages).
// Confirmed against game_scarlett_master.exe:
//   BOOL XMemAllocatePhysicalPages(uint64_t flags, ULONG_PTR* number_of_pages, ULONG_PTR* page_array)
constexpr size_t kXboxPhysicalPageSize = 65536;
std::atomic<ULONG_PTR> g_next_pfn{1};

BOOL __stdcall XMemAllocatePhysicalPages(uint64_t /*flags*/, ULONG_PTR* number_of_pages, ULONG_PTR* page_array) {
    if (!number_of_pages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const ULONG_PTR count = *number_of_pages;
    if (count > 0 && page_array) {
        ULONG_PTR base = g_next_pfn.fetch_add(count, std::memory_order_relaxed);
        for (ULONG_PTR i = 0; i < count; ++i) {
            page_array[i] = base + i;
        }
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemAllocatePhysicalPages(count=%llu) -> TRUE\n",
                     static_cast<unsigned long long>(count));
    return TRUE;
}

BOOL __stdcall XMemFreePhysicalPages(uint64_t /*flags*/, ULONG_PTR* /*number_of_pages*/, ULONG_PTR* /*page_array*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemFreePhysicalPages -> TRUE\n");
    return TRUE;
}

// XMemMapPhysicalPages: maps (or unmaps when page_array == nullptr) `number_of_pages` 64 KB pages at `va`.
BOOL __stdcall XMemMapPhysicalPages(void* va, ULONG_PTR number_of_pages, const ULONG_PTR* page_array) {
    if (!va || number_of_pages == 0) return TRUE;
    const SIZE_T bytes = static_cast<SIZE_T>(number_of_pages) * kXboxPhysicalPageSize;
    if (page_array) {
        if (!VirtualAlloc(va, bytes, MEM_COMMIT, PAGE_READWRITE)) return FALSE;
    } else {
        VirtualFree(va, bytes, MEM_DECOMMIT);
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] xmem: XMemMapPhysicalPages(va=%p, pages=%llu) -> TRUE\n", va,
                     static_cast<unsigned long long>(number_of_pages));
    return TRUE;
}

// XMemMapPhysicalPagesScatter: maps (or unmaps) 64 KB physical pages into an array of CPU VAs.
BOOL __stdcall XMemMapPhysicalPagesScatter(void** va_array, ULONG_PTR number_of_pages, const ULONG_PTR* page_array) {
    if (!va_array || number_of_pages == 0) return TRUE;
    for (ULONG_PTR i = 0; i < number_of_pages; ++i) {
        if (!va_array[i]) continue;
        if (page_array) {
            if (!VirtualAlloc(va_array[i], kXboxPhysicalPageSize, MEM_COMMIT, PAGE_READWRITE)) return FALSE;
        } else {
            VirtualFree(va_array[i], kXboxPhysicalPageSize, MEM_DECOMMIT);
        }
    }
    return TRUE;
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
