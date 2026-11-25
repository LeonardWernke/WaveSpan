#include "crash_dump.h"

#include <dbghelp.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>

namespace rt::crash_dump {

namespace {

std::filesystem::path g_error_dir;
CRITICAL_SECTION g_lock;
std::once_flag g_lock_init;
std::atomic<unsigned long> g_seq{0};

void ensure_lock() {
    std::call_once(g_lock_init, []() { InitializeCriticalSection(&g_lock); });
}

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        default: return "UNKNOWN_EXCEPTION";
    }
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE,
                                          DWORD,
                                          HANDLE,
                                          MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymCleanupFn = BOOL(WINAPI*)(HANDLE);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
using StackWalk64Fn = BOOL(WINAPI*)(DWORD,
                                    HANDLE,
                                    HANDLE,
                                    LPSTACKFRAME64,
                                    PVOID,
                                    PREAD_PROCESS_MEMORY_ROUTINE64,
                                    PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                    PGET_MODULE_BASE_ROUTINE64,
                                    PTRANSLATE_ADDRESS_ROUTINE64);
using SymFunctionTableAccess64Fn = PVOID(WINAPI*)(HANDLE, DWORD64);
using SymGetModuleBase64Fn = DWORD64(WINAPI*)(HANDLE, DWORD64);

HANDLE create_unique_file(const std::filesystem::path& path) {
    return CreateFileW(path.c_str(),
                       GENERIC_WRITE,
                       FILE_SHARE_READ,
                       nullptr,
                       CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

void write_str(HANDLE h, const char* s) {
    DWORD written = 0;
    WriteFile(h, s, static_cast<DWORD>(std::strlen(s)), &written, nullptr);
}

void write_stack_walk(HANDLE h, HMODULE dbghelp, EXCEPTION_POINTERS* info) {
    if (!dbghelp || !info || !info->ContextRecord) return;
    auto sym_init = reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
    auto sym_cleanup = reinterpret_cast<SymCleanupFn>(GetProcAddress(dbghelp, "SymCleanup"));
    auto sym_set_options = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
    auto sym_from_addr = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
    auto stack_walk = reinterpret_cast<StackWalk64Fn>(GetProcAddress(dbghelp, "StackWalk64"));
    auto ftable_access = reinterpret_cast<SymFunctionTableAccess64Fn>(
        GetProcAddress(dbghelp, "SymFunctionTableAccess64"));
    auto mod_base = reinterpret_cast<SymGetModuleBase64Fn>(GetProcAddress(dbghelp, "SymGetModuleBase64"));
    if (!stack_walk || !ftable_access || !mod_base) return;

    const HANDLE proc = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    bool syms_ok = false;
    if (sym_init) {
        if (sym_set_options) sym_set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        syms_ok = sym_init(proc, nullptr, TRUE) != FALSE;
    }

    // DbgHelp's SymFunctionTableAccess64/SymGetModuleBase64 only know host modules; fall back to
    // RtlLookupFunctionEntry so StackWalk64 can unwind through manually mapped images (RtlAddFunctionTable).
    struct WalkCtx {
        HANDLE proc;
        SymFunctionTableAccess64Fn ftable;
        SymGetModuleBase64Fn mbase;
    } walk_ctx{proc, ftable_access, mod_base};

    static WalkCtx* s_walk_ctx = nullptr;
    s_walk_ctx = &walk_ctx;

    auto custom_ftable = [](HANDLE h, DWORD64 addr) -> PVOID {
        if (PVOID p = s_walk_ctx->ftable(h, addr)) return p;
        DWORD64 base = 0;
        return RtlLookupFunctionEntry(addr, &base, nullptr);
    };
    auto custom_mbase = [](HANDLE h, DWORD64 addr) -> DWORD64 {
        if (DWORD64 b = s_walk_ctx->mbase(h, addr)) return b;
        DWORD64 base = 0;
        if (RtlLookupFunctionEntry(addr, &base, nullptr)) return base;
        return 0;
    };

    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    write_str(h, "\r\nCall stack:\r\n");
    char line[512];
    alignas(SYMBOL_INFO) unsigned char sym_buf[sizeof(SYMBOL_INFO) + 256]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < 64; ++i) {
        if (!stack_walk(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx,
                        nullptr, custom_ftable, custom_mbase, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        DWORD64 disp = 0;
        if (syms_ok && sym_from_addr && sym_from_addr(proc, frame.AddrPC.Offset, &disp, sym)) {
            std::snprintf(line, sizeof(line), "  #%02d  0x%016llX  %s + 0x%llX\r\n",
                          i, static_cast<unsigned long long>(frame.AddrPC.Offset),
                          sym->Name, static_cast<unsigned long long>(disp));
        } else {
            PVOID img_base = nullptr;
            RtlPcToFileHeader(reinterpret_cast<PVOID>(frame.AddrPC.Offset), &img_base);
            if (img_base) {
                wchar_t mname[MAX_PATH]{};
                GetModuleFileNameW(static_cast<HMODULE>(img_base), mname, MAX_PATH);
                const wchar_t* base_name = std::wcsrchr(mname, L'\\');
                base_name = base_name ? base_name + 1 : mname;
                const uint64_t rva = frame.AddrPC.Offset - reinterpret_cast<uint64_t>(img_base);
                std::snprintf(line, sizeof(line), "  #%02d  0x%016llX  %ls + 0x%llX\r\n",
                              i, static_cast<unsigned long long>(frame.AddrPC.Offset),
                              base_name, static_cast<unsigned long long>(rva));
            } else {
                std::snprintf(line, sizeof(line), "  #%02d  0x%016llX\r\n",
                              i, static_cast<unsigned long long>(frame.AddrPC.Offset));
            }
        }
        write_str(h, line);
    }

    if (syms_ok && sym_cleanup) sym_cleanup(proc);
}

void write_log_file(HANDLE h, HMODULE dbghelp, EXCEPTION_POINTERS* info) {
    char buf[1024];
    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(buf, sizeof(buf),
                  "WaveSpan gta6rt crash report\r\n"
                  "Time: %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
                  "PID: %lu  TID: %lu\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  static_cast<unsigned long>(pid), static_cast<unsigned long>(tid));
    write_str(h, buf);

    if (info && info->ExceptionRecord) {
        const auto* er = info->ExceptionRecord;
        std::snprintf(buf, sizeof(buf),
                      "Exception: %s (0x%08lX) flags=0x%08lX addr=0x%p\r\n",
                      exception_name(er->ExceptionCode),
                      static_cast<unsigned long>(er->ExceptionCode),
                      static_cast<unsigned long>(er->ExceptionFlags),
                      er->ExceptionAddress);
        write_str(h, buf);

        if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            er->NumberParameters >= 2) {
            const char* op = er->ExceptionInformation[0] == 0   ? "read"
                           : er->ExceptionInformation[0] == 1   ? "write"
                           : er->ExceptionInformation[0] == 8   ? "execute"
                                                                : "other";
            std::snprintf(buf, sizeof(buf), "Access: %s at VA=0x%016llX\r\n",
                          op, static_cast<unsigned long long>(er->ExceptionInformation[1]));
            write_str(h, buf);
        }
    }

    if (info && info->ContextRecord) {
        const auto* ctx = info->ContextRecord;
        std::snprintf(buf, sizeof(buf),
                      "\r\nRegisters:\r\n"
                      "  RIP=0x%016llX  RSP=0x%016llX  RBP=0x%016llX  EFlags=0x%08lX\r\n"
                      "  RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX\r\n"
                      "  RSI=0x%016llX  RDI=0x%016llX  R8 =0x%016llX  R9 =0x%016llX\r\n"
                      "  R10=0x%016llX  R11=0x%016llX  R12=0x%016llX  R13=0x%016llX\r\n"
                      "  R14=0x%016llX  R15=0x%016llX\r\n",
                      static_cast<unsigned long long>(ctx->Rip),
                      static_cast<unsigned long long>(ctx->Rsp),
                      static_cast<unsigned long long>(ctx->Rbp),
                      static_cast<unsigned long>(ctx->EFlags),
                      static_cast<unsigned long long>(ctx->Rax),
                      static_cast<unsigned long long>(ctx->Rbx),
                      static_cast<unsigned long long>(ctx->Rcx),
                      static_cast<unsigned long long>(ctx->Rdx),
                      static_cast<unsigned long long>(ctx->Rsi),
                      static_cast<unsigned long long>(ctx->Rdi),
                      static_cast<unsigned long long>(ctx->R8),
                      static_cast<unsigned long long>(ctx->R9),
                      static_cast<unsigned long long>(ctx->R10),
                      static_cast<unsigned long long>(ctx->R11),
                      static_cast<unsigned long long>(ctx->R12),
                      static_cast<unsigned long long>(ctx->R13),
                      static_cast<unsigned long long>(ctx->R14),
                      static_cast<unsigned long long>(ctx->R15));
        write_str(h, buf);
    }

    write_stack_walk(h, dbghelp, info);

    // List loaded host modules so faulting addresses can be mapped to module + RVA.
    HMODULE mods[256]{};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD count = (needed / sizeof(HMODULE)) < 256 ? (needed / sizeof(HMODULE)) : 256;
        write_str(h, "\r\nHost modules:\r\n");
        for (DWORD i = 0; i < count; ++i) {
            MODULEINFO mi{};
            wchar_t wname[MAX_PATH]{};
            GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
            GetModuleFileNameW(mods[i], wname, MAX_PATH);
            std::snprintf(buf, sizeof(buf), "  0x%p +0x%08lX  %ls\r\n",
                          mi.lpBaseOfDll,
                          static_cast<unsigned long>(mi.SizeOfImage),
                          wname);
            write_str(h, buf);
        }
    }
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
    std::filesystem::path dmp_path, log_path;
    if (write_report(info, &dmp_path, &log_path)) {
        std::fprintf(stderr, "crash report written:\n  log: %ls\n  dmp: %ls\n",
                     log_path.c_str(), dmp_path.c_str());
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI trace_startup_fault(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // Capture each kind once even if the title handles the fault before the unhandled filter.
    static std::atomic<bool> breakpoint_recorded{false};
    static std::atomic<bool> access_violation_recorded{false};
    auto& recorded = code == EXCEPTION_BREAKPOINT ? breakpoint_recorded : access_violation_recorded;
    if (!recorded.exchange(true, std::memory_order_relaxed)) {
        const char* kind = code == EXCEPTION_BREAKPOINT ? "breakpoint" : "access violation";
        std::fprintf(stderr, "[rt] first-chance %s at %p (thread %lu)\n", kind,
                     info->ExceptionRecord->ExceptionAddress, static_cast<unsigned long>(GetCurrentThreadId()));
        unhandled_filter(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminate_handler() {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er{};
    er.ExceptionCode = 0xE0000001;  // C++ std::terminate
    er.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS info{&er, &ctx};
    std::filesystem::path dmp_path, log_path;
    if (write_report(&info, &dmp_path, &log_path)) {
        std::fprintf(stderr, "std::terminate crash report written:\n  log: %ls\n  dmp: %ls\n",
                     log_path.c_str(), dmp_path.c_str());
        std::fflush(stderr);
    }
    std::abort();
}

}  // namespace

std::filesystem::path default_error_dir(const std::filesystem::path& overlay_dir) {
    wchar_t env_buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"WAVESPAN_CRASH_DIR", env_buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(env_buf);

    return overlay_dir / L"errors";
}

void install(const std::filesystem::path& error_dir, bool trace_startup_faults) {
    ensure_lock();
    EnterCriticalSection(&g_lock);
    g_error_dir = error_dir;
    LeaveCriticalSection(&g_lock);
    SetUnhandledExceptionFilter(unhandled_filter);
    if (trace_startup_faults && !AddVectoredExceptionHandler(1, trace_startup_fault))
        std::fprintf(stderr, "[rt] Could not register the startup fault trace handler\n");
    std::set_terminate(terminate_handler);
}

bool write_report(EXCEPTION_POINTERS* info,
                  std::filesystem::path* out_dmp,
                  std::filesystem::path* out_log) {
    ensure_lock();
    EnterCriticalSection(&g_lock);
    const std::filesystem::path dir = g_error_dir;
    LeaveCriticalSection(&g_lock);

    if (dir.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();
    const unsigned long seq = g_seq.fetch_add(1, std::memory_order_relaxed);

    wchar_t stem[128]{};
    std::swprintf(stem, std::size(stem),
                  L"gta6rt_%04u%02u%02u_%02u%02u%02u_%lu_%lu_%lu",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(pid),
                  static_cast<unsigned long>(tid),
                  seq);

    const std::filesystem::path log_path = dir / (std::wstring(stem) + L".log");
    const std::filesystem::path dmp_path = dir / (std::wstring(stem) + L".dmp");

    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    bool wrote_any = false;
    HANDLE h_log = create_unique_file(log_path);
    if (h_log != INVALID_HANDLE_VALUE) {
        write_log_file(h_log, dbghelp, info);
        CloseHandle(h_log);
        wrote_any = true;
        if (out_log) *out_log = log_path;
    }

    if (dbghelp) {
        auto write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
            GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        if (write_dump) {
            HANDLE h_dmp = create_unique_file(dmp_path);
            if (h_dmp != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = tid;
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                const auto dump_type = static_cast<MINIDUMP_TYPE>(
                    MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
                if (write_dump(GetCurrentProcess(), pid, h_dmp, dump_type,
                               info ? &mei : nullptr, nullptr, nullptr)) {
                    wrote_any = true;
                    if (out_dmp) *out_dmp = dmp_path;
                }
                CloseHandle(h_dmp);
            }
        }
        FreeLibrary(dbghelp);
    }

    return wrote_any;
}

}  // namespace rt::crash_dump
