// Checks the registered xgameruntime shim the way a GDK title uses it at boot.
//
// Every function is resolved through rt::shims::find("xgameruntime.dll", name), the same table the loader binds
// imports to. The sequence follows a typical title: initialize the runtime, query the device, create a queue whose
// work runs on the thread pool and whose completions the game loop dispatches by hand, run async work on it and on
// the process queue, drive a custom XAsyncBegin provider to a result, then uninitialize. No game data; nothing
// online.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <class Fn>
Fn get(const char* name) {
    return reinterpret_cast<Fn>(rt::shims::find("xgameruntime.dll", name));
}

using InitFn = HRESULT(__stdcall*)();
using UninitFn = void(__stdcall*)();
using DeviceTypeFn = uint32_t(__stdcall*)();
using ConsoleIdFn = HRESULT(__stdcall*)(size_t, char*, size_t*);
using QueueCreateFn = HRESULT(__stdcall*)(XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*);
using QueueCloseFn = void(__stdcall*)(XTaskQueueHandle);
using QueueDispatchFn = bool(__stdcall*)(XTaskQueueHandle, XTaskQueuePort, uint32_t);
using QueueProcessFn = bool(__stdcall*)(XTaskQueueHandle*);
using AsyncRunFn = HRESULT(__stdcall*)(XAsyncBlock*, XAsyncWork*);
using AsyncStatusFn = HRESULT(__stdcall*)(XAsyncBlock*, bool);
using AsyncBeginFn = HRESULT(__stdcall*)(XAsyncBlock*, void*, const void*, const char*, XAsyncProvider*);
using AsyncScheduleFn = HRESULT(__stdcall*)(XAsyncBlock*, uint32_t);
using AsyncCompleteFn = void(__stdcall*)(XAsyncBlock*, HRESULT, size_t);
using AsyncResultSizeFn = HRESULT(__stdcall*)(XAsyncBlock*, size_t*);
using AsyncResultFn = HRESULT(__stdcall*)(XAsyncBlock*, const void*, size_t, void*, size_t*);

struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};
static_assert(sizeof(XVersion) == 8, "XVersion must be 8 bytes per GDK spec");

struct XSystemAnalyticsInfo {
    XVersion osVersion;
    XVersion hostingOsVersion;
    char family[64];
    char form[64];
};
static_assert(sizeof(XSystemAnalyticsInfo) == 144, "XSystemAnalyticsInfo must be 144 bytes per GDK spec");

using AnalyticsInfoFn = XSystemAnalyticsInfo(__stdcall*)();
using PackageIdentifierFn = HRESULT(__stdcall*)(size_t, char*);
using PackageMountPathFn = HRESULT(__stdcall*)(void*, size_t, char*);
using PackagedProcessFn = bool(__stdcall*)();

const char* const kNames[] = {
    "XGameRuntimeInitialize", "XGameRuntimeUninitialize", "XSystemGetDeviceType", "XSystemGetConsoleId",
    "XSystemGetAnalyticsInfo", "XSystemGetXboxLiveSandboxId", "XSystemGetRuntimeInfo",
    "XPackageGetCurrentProcessPackageIdentifier", "XPackageGetMountPath",
    "XPackageIsPackagedProcess",
    "XUserAddAsync", "XUserAddResult", "XUserAddByIdWithUiAsync", "XUserAddByIdWithUiResult",
    "XUserDuplicateHandle", "XUserCloseHandle", "XUserGetId", "XUserGetLocalId", "XUserGetState",
    "XUserGetGamertag", "XUserCompare",
    "XGameSaveInitializeProvider", "XGameSaveInitializeProviderAsync", "XGameSaveInitializeProviderResult",
    "XGameSaveCloseProvider", "XGameSaveGetRemainingQuota", "XGameSaveGetRemainingQuotaAsync",
    "XGameSaveGetRemainingQuotaResult", "XGameSaveCreateContainer", "XGameSaveCloseContainer",
    "XGameSaveDeleteContainer", "XGameSaveDeleteContainerAsync", "XGameSaveDeleteContainerResult",
    "XGameSaveEnumerateContainerInfo", "XGameSaveEnumerateContainerInfoByName", "XGameSaveGetContainerInfo",
    "XGameSaveEnumerateBlobInfo", "XGameSaveEnumerateBlobInfoByName",
    "XGameSaveReadBlobData", "XGameSaveReadBlobDataAsync", "XGameSaveReadBlobDataResult",
    "XGameSaveCreateUpdate", "XGameSaveCloseUpdate", "XGameSaveSubmitBlobWrite", "XGameSaveSubmitBlobDelete",
    "XGameSaveSubmitUpdate", "XGameSaveSubmitUpdateAsync", "XGameSaveSubmitUpdateResult",
    "XTaskQueueCreate", "XTaskQueueCreateComposite", "XTaskQueueGetPort", "XTaskQueueDuplicateHandle",
    "XTaskQueueCloseHandle", "XTaskQueueTerminate", "XTaskQueueDispatch", "XTaskQueueSubmitCallback",
    "XTaskQueueSubmitDelayedCallback", "XTaskQueueRegisterWaiter", "XTaskQueueUnregisterWaiter",
    "XTaskQueueRegisterMonitor", "XTaskQueueUnregisterMonitor", "XTaskQueueGetCurrentProcessTaskQueue",
    "XTaskQueueSetCurrentProcessTaskQueue", "XAsyncGetStatus", "XAsyncGetResultSize", "XAsyncCancel", "XAsyncRun",
    "XAsyncBegin", "XAsyncSchedule", "XAsyncComplete", "XAsyncGetResult",
};

std::atomic<int> g_work_runs{0};
std::atomic<DWORD> g_work_thread{0};
std::atomic<int> g_completions{0};
std::atomic<DWORD> g_completion_thread{0};

HRESULT CALLBACK work(XAsyncBlock* b) {
    g_work_thread = GetCurrentThreadId();
    ++g_work_runs;
    *static_cast<int*>(b->context) = 42;
    return S_OK;
}

void CALLBACK completed(XAsyncBlock*) {
    g_completion_thread = GetCurrentThreadId();
    ++g_completions;
}

// A provider that produces a 4-byte result on the queue's work port: the shape GDK async APIs use internally.
struct ProviderState {
    AsyncCompleteFn complete;
    std::atomic<int> ops{0};
};

HRESULT CALLBACK provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* st = static_cast<ProviderState*>(data->context);
    st->ops |= 1 << static_cast<int>(op);
    switch (op) {
        case XAsyncOp::DoWork:
            st->complete(data->async, S_OK, sizeof(uint32_t));
            return S_OK;
        case XAsyncOp::GetResult:
            *static_cast<uint32_t*>(data->buffer) = 0x1EA0;
            return S_OK;
        default:
            return S_OK;
    }
}

HMODULE g_teardown_rt_mod = nullptr;
FARPROC(WINAPI* g_teardown_get_proc)(HMODULE, LPCSTR) = nullptr;
BOOL(WINAPI* g_teardown_free_lib)(HMODULE) = nullptr;

// Registered with std::atexit before check_run_time_load() constructs any function-local statics in library_loader.
// LIFO atexit order means this runs after those statics would have been destroyed, simulating mapped-DLL
// DLL_PROCESS_DETACH or earlier atexit handlers calling GetProcAddress/FreeLibrary on a pseudo-handle during exit.
void check_late_teardown() {
    if (!g_teardown_rt_mod || !g_teardown_get_proc || !g_teardown_free_lib) return;
    if (g_teardown_get_proc(g_teardown_rt_mod, "UninitializeApiImpl") == nullptr) {
        std::fprintf(stderr, "FAIL late teardown GetProcAddress on pseudo-handle\n");
        std::_Exit(1);
    }
    SetLastError(0);
    if (g_teardown_get_proc(g_teardown_rt_mod, "LateTeardownMissingSymbol") != nullptr ||
        GetLastError() != ERROR_PROC_NOT_FOUND) {
        std::fprintf(stderr, "FAIL late teardown unserved GetProcAddress on pseudo-handle\n");
        std::_Exit(1);
    }
    if (g_teardown_free_lib(g_teardown_rt_mod) != TRUE) {
        std::fprintf(stderr, "FAIL late teardown FreeLibrary on pseudo-handle\n");
        std::_Exit(1);
    }
}

// xgameruntime.lib's thunks don't import the runtime: they LoadLibraryW(L"xgameruntime.dll") and GetProcAddress five
// names (measured in the package's libHttpClient.GDK.dll and XCurl.dll). Drive that path through the loader shims,
// bound by the api set XCurl.dll imports them from, as a mapped image would be.
void check_run_time_load() {
    using LoadW = HMODULE(WINAPI*)(LPCWSTR);
    using LoadExA = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
    using GetModW = HMODULE(WINAPI*)(LPCWSTR);
    using GetProc = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    using Free = BOOL(WINAPI*)(HMODULE);
    const char* set = "api-ms-win-core-libraryloader-l1-2-0.dll";
    auto load_w = reinterpret_cast<LoadW>(rt::shims::find(set, "LoadLibraryW"));
    auto load_ex_a = reinterpret_cast<LoadExA>(rt::shims::find("kernel32.dll", "LoadLibraryExA"));
    auto get_mod_w = reinterpret_cast<GetModW>(rt::shims::find(set, "GetModuleHandleW"));
    auto get_proc = reinterpret_cast<GetProc>(rt::shims::find(set, "GetProcAddress"));
    auto free_lib = reinterpret_cast<Free>(rt::shims::find(set, "FreeLibrary"));
    check(load_w && load_ex_a && get_mod_w && get_proc && free_lib, "the loader functions are registered");
    if (!(load_w && load_ex_a && get_mod_w && get_proc && free_lib)) return;

    check(rt::shims::owns_dll("xgameruntime.dll") && !rt::shims::owns_dll("kernel32.dll") &&
              !rt::shims::owns_dll(set) && !rt::shims::owns_dll("nosuch.dll"),
          "owns_dll: xgameruntime.dll yes; kernel32, api sets and unknown modules no");

    HMODULE rt_mod = load_w(L"xgameruntime.dll");
    check(rt_mod != nullptr, "LoadLibraryW(xgameruntime.dll) is served without the host runtime");
    check(load_w(L"XGameRuntime") == rt_mod && load_ex_a("C:\\Some\\Dir\\XGAMERUNTIME.DLL", nullptr, 0) == rt_mod &&
              get_mod_w(L"xgameruntime.dll") == rt_mod,
          "the same module for a bare name, a full path in any case, LoadLibraryExA and GetModuleHandleW");

    const char* entry_points[] = {"InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2",
                                  "UninitializeApiImpl", "QueryApiImpl"};
    bool all = true;
    for (const char* n : entry_points)
        all = all && get_proc(rt_mod, n) &&
              reinterpret_cast<void*>(get_proc(rt_mod, n)) == rt::shims::find("xgameruntime.dll", n);
    check(all, "GetProcAddress serves the five entry points xgameruntime.lib looks up");
    SetLastError(0);
    check(get_proc(rt_mod, "NoSuchRuntimeFunction") == nullptr && GetLastError() == ERROR_PROC_NOT_FOUND,
          "an unserved name is NULL with ERROR_PROC_NOT_FOUND, as from a real module");
    SetLastError(0);
    check(get_proc(rt_mod, nullptr) == nullptr && GetLastError() == ERROR_PROC_NOT_FOUND,
          "a null proc name is NULL with ERROR_PROC_NOT_FOUND");

    struct Options {
        uint32_t unknown;
        BOOL is_inline;
        const char* game_config;
    } options{1, FALSE, nullptr};
    using InitEx2 = HRESULT(WINAPI*)(ULONG, ULONG, char, const Options*);
    using Query = HRESULT(WINAPI*)(const GUID*, REFIID, void**);
    using Uninit = HRESULT(WINAPI*)();
    auto init_ex2 = reinterpret_cast<InitEx2>(get_proc(rt_mod, "InitializeApiImplEx2"));
    auto query = reinterpret_cast<Query>(get_proc(rt_mod, "QueryApiImpl"));
    auto uninit_api = reinterpret_cast<Uninit>(get_proc(rt_mod, "UninitializeApiImpl"));
    check(init_ex2(0x2410, 0x0, 0x02, &options) == S_OK, "InitializeApiImplEx2 succeeds offline");
    const GUID some_class = {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
    void* out = reinterpret_cast<void*>(1);
    check(query(&some_class, some_class, &out) == static_cast<HRESULT>(0x80070032) && out == nullptr,
          "QueryApiImpl refuses a class it doesn't provide yet (HRESULT 0x80070032) and clears the out pointer");

    // XThreadingImpl and XErrorImpl, driven the way xgameruntime.lib's thunks in the package's DLLs do: QueryApiImpl
    // with the class id as both ids, one vtable slot with `this` first, then Release (slot 2).
    const GUID threading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
    const GUID error_cls = {0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};
    using Fn = void*;
    auto slot = [](void* obj, int index) { return (*static_cast<Fn* const*>(obj))[index]; };
    auto release = [&](void* obj) { reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(slot(obj, 2))(obj); };
    auto get = [&](const GUID& cls) {
        void* p = nullptr;
        return query(&cls, cls, &p) == S_OK ? p : nullptr;
    };
    void* t = get(threading);
    check(t != nullptr, "QueryApiImpl hands out XThreadingImpl for its class id");
    if (t) {
        void* again = get(threading);
        check(again == t, "XThreadingImpl is one process-wide object");
        release(again);
        XTaskQueueHandle queue = nullptr;
        using Create = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueDispatchMode, XTaskQueueDispatchMode,
                                                  XTaskQueueHandle*);
        check(reinterpret_cast<Create>(slot(t, 12))(t, XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual,
                                                    &queue) == S_OK && queue,
              "slot 12 is XTaskQueueCreate");
        static std::atomic<int> s_ran{0};
        using Submit = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, void*,
                                                  XTaskQueueCallback*);
        XTaskQueueCallback* cb = [](void* ctx, bool canceled) {
            if (!canceled) static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
        };
        check(reinterpret_cast<Submit>(slot(t, 18))(t, queue, XTaskQueuePort::Work, &s_ran, cb) == S_OK,
              "slot 18 is XTaskQueueSubmitCallback");
        using Dispatch = bool(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, uint32_t);
        check(reinterpret_cast<Dispatch>(slot(t, 16))(t, queue, XTaskQueuePort::Work, 0) && s_ran.load() == 1,
              "slot 16 is XTaskQueueDispatch: the callback ran on the work port");
        XTaskQueueHandle dup = nullptr;
        using Dup = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueueHandle*);
        check(reinterpret_cast<Dup>(slot(t, 15))(t, queue, &dup) == S_OK && dup,
              "slot 15 is XTaskQueueDuplicateHandle");
        using Close = void(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle);
        reinterpret_cast<Close>(slot(t, 17))(t, dup);
        reinterpret_cast<Close>(slot(t, 17))(t, queue);
        using SetSensitive = HRESULT(STDMETHODCALLTYPE*)(void*, bool);
        using IsSensitive = bool(STDMETHODCALLTYPE*)(void*);
        check(reinterpret_cast<SetSensitive>(slot(t, 27))(t, true) == S_OK &&
                  reinterpret_cast<IsSensitive>(slot(t, 30))(t) &&
                  reinterpret_cast<SetSensitive>(slot(t, 27))(t, false) == S_OK &&
                  !reinterpret_cast<IsSensitive>(slot(t, 30))(t),
              "slots 27 and 30 set and read this thread's time-sensitive flag");
        void* unk = nullptr;
        const GUID iunknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
        using QI = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
        check(reinterpret_cast<QI>(slot(t, 0))(t, iunknown, &unk) == S_OK && unk == t &&
                  reinterpret_cast<QI>(slot(t, 0))(t, error_cls, &unk) == E_NOINTERFACE && !unk,
              "QueryInterface answers IUnknown and the class id, not another class");
        release(t);
        release(t);
    }
    void* e = get(error_cls);
    check(e != nullptr, "QueryApiImpl hands out XErrorImpl for its class id");
    if (e) {
        struct Seen {
            HRESULT hr;
            char msg[32];
        } seen{};
        using Callback = bool(CALLBACK*)(HRESULT, const char*, void*);
        Callback on_error = [](HRESULT hr, const char* msg, void* ctx) {
            auto* s = static_cast<Seen*>(ctx);
            s->hr = hr;
            strncpy_s(s->msg, msg ? msg : "", _TRUNCATE);
            return true;
        };
        using SetCallback = void(STDMETHODCALLTYPE*)(void*, Callback, void*);
        using Report = void(STDMETHODCALLTYPE*)(void*, HRESULT, const char*);
        reinterpret_cast<SetCallback>(slot(e, 4))(e, on_error, &seen);
        reinterpret_cast<Report>(slot(e, 3))(e, E_FAIL, "offline test");
        check(seen.hr == E_FAIL && std::strcmp(seen.msg, "offline test") == 0,
              "slot 3 is XErrorReport and reaches the callback slot 4 (XErrorSetCallback) set");
        reinterpret_cast<SetCallback>(slot(e, 4))(e, nullptr, nullptr);
        using SetOptions = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, uint32_t);
        check(reinterpret_cast<SetOptions>(slot(e, 5))(e, 0, 0) == S_OK, "slot 5 is XErrorSetOptions");
        release(e);
    }
    check(uninit_api() == S_OK, "UninitializeApiImpl succeeds");

    HMODULE k32 = load_w(L"kernel32.dll");
    check(k32 == GetModuleHandleW(L"kernel32.dll"), "a host module still loads from the host");
    check(reinterpret_cast<void*>(get_proc(k32, "GetTickCount")) ==
              reinterpret_cast<void*>(GetProcAddress(k32, "GetTickCount")),
          "GetProcAddress on a host module is the host's");
    check(free_lib(rt_mod) == TRUE && load_w(L"xgameruntime.dll") == rt_mod,
          "FreeLibrary on the runtime succeeds and keeps it loaded");

    g_teardown_rt_mod = rt_mod;
    g_teardown_get_proc = get_proc;
    g_teardown_free_lib = free_lib;
}

void check_xsystem_extra() {
    using SandboxIdFn = HRESULT(__stdcall*)(size_t, char*, size_t*);
    struct XSystemRuntimeInfo {
        XVersion runtimeVersion;
        XVersion availableVersion;
    };
    using RuntimeInfoFn = HRESULT(__stdcall*)(XSystemRuntimeInfo*);

    auto sandbox = get<SandboxIdFn>("XSystemGetXboxLiveSandboxId");
    char sb[32] = {};
    size_t used = 0;
    check(sandbox(16, nullptr, &used) == E_INVALIDARG, "XSystemGetXboxLiveSandboxId rejects null buffer");
    check(sandbox(3, sb, &used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
          "XSystemGetXboxLiveSandboxId rejects insufficient buffer");
    check(sandbox(sizeof(sb), sb, &used) == S_OK && std::strcmp(sb, "RETAIL") == 0 && used == 7,
          "XSystemGetXboxLiveSandboxId returns RETAIL by default");
    SetEnvironmentVariableA("WAVESPAN_SANDBOX_ID", "XDKS.1");
    check(sandbox(sizeof(sb), sb, &used) == S_OK && std::strcmp(sb, "XDKS.1") == 0 && used == 7,
          "XSystemGetXboxLiveSandboxId respects WAVESPAN_SANDBOX_ID");
    SetEnvironmentVariableA("WAVESPAN_SANDBOX_ID", nullptr);

    auto rt_info = get<RuntimeInfoFn>("XSystemGetRuntimeInfo");
    check(rt_info(nullptr) == E_INVALIDARG, "XSystemGetRuntimeInfo rejects null pointer");
    XSystemRuntimeInfo rti{};
    check(rt_info(&rti) == S_OK && rti.runtimeVersion.major == 10 && rti.availableVersion.build == 22000,
          "XSystemGetRuntimeInfo returns 10.0.22000.0");
}

void check_xuser() {
    struct XUser;
    using XUserHandle = XUser*;
    enum class XUserAddOptions : uint32_t { AddDefaultUserSilently = 0x01 };
    enum class XUserState : uint32_t { SignedIn = 0 };
    enum class XUserGetGamertagComponent : uint32_t { Classic = 0, Modern = 1, ModernSuffix = 2, UniqueModern = 3 };
    struct XUserLocalId { uint64_t value; };

    auto add_async = get<HRESULT(__stdcall*)(XUserAddOptions, XAsyncBlock*)>("XUserAddAsync");
    auto add_result = get<HRESULT(__stdcall*)(XAsyncBlock*, XUserHandle*)>("XUserAddResult");
    auto add_by_id_async = get<HRESULT(__stdcall*)(uint64_t, XAsyncBlock*)>("XUserAddByIdWithUiAsync");
    auto add_by_id_result = get<HRESULT(__stdcall*)(XAsyncBlock*, XUserHandle*)>("XUserAddByIdWithUiResult");
    auto dup = get<HRESULT(__stdcall*)(XUserHandle, XUserHandle*)>("XUserDuplicateHandle");
    auto close_u = get<void(__stdcall*)(XUserHandle)>("XUserCloseHandle");
    auto get_id = get<HRESULT(__stdcall*)(XUserHandle, uint64_t*)>("XUserGetId");
    auto get_local = get<HRESULT(__stdcall*)(XUserHandle, XUserLocalId*)>("XUserGetLocalId");
    auto get_state = get<HRESULT(__stdcall*)(XUserHandle, XUserState*)>("XUserGetState");
    auto get_tag = get<HRESULT(__stdcall*)(XUserHandle, XUserGetGamertagComponent, size_t, char*, size_t*)>("XUserGetGamertag");
    auto cmp = get<int32_t(__stdcall*)(XUserHandle, XUserHandle)>("XUserCompare");
    auto status = get<AsyncStatusFn>("XAsyncGetStatus");

    XAsyncBlock ab{};
    XUserHandle user = nullptr;
    check(add_async(XUserAddOptions::AddDefaultUserSilently, &ab) == S_OK &&
              status(&ab, true) == S_OK && add_result(&ab, &user) == S_OK && user != nullptr,
          "XUserAddAsync/XUserAddResult returns an offline user handle");

    uint64_t xuid = 0;
    XUserLocalId lid{0};
    XUserState state = static_cast<XUserState>(99);
    char tag[64] = {};
    size_t tag_used = 0;
    check(get_id(user, &xuid) == S_OK && xuid != 0, "XUserGetId returns a non-zero offline XUID");
    check(get_local(user, &lid) == S_OK && lid.value == 1, "XUserGetLocalId returns 1");
    check(get_state(user, &state) == S_OK && state == XUserState::SignedIn, "XUserGetState reports SignedIn");
    check(get_tag(user, XUserGetGamertagComponent::Classic, 4, tag, &tag_used) ==
              HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
          "XUserGetGamertag rejects insufficient buffer");
    check(get_tag(user, XUserGetGamertagComponent::Classic, sizeof(tag), tag, &tag_used) == S_OK &&
              std::strcmp(tag, "WaveSpanUser") == 0,
          "XUserGetGamertag returns WaveSpanUser");
    check(get_tag(user, XUserGetGamertagComponent::ModernSuffix, sizeof(tag), tag, &tag_used) == S_OK &&
              tag[0] == '\0' && tag_used == 1,
          "XUserGetGamertag ModernSuffix returns empty string");

    XUserHandle dup_user = nullptr;
    check(dup(user, &dup_user) == S_OK && dup_user == user && cmp(user, dup_user) == 0,
          "XUserDuplicateHandle shares handle and XUserCompare returns 0");
    close_u(dup_user);
    close_u(user);
    close_u(nullptr);

    XAsyncBlock ab2{};
    XUserHandle user2 = nullptr;
    uint64_t xuid2 = 0;
    check(add_by_id_async(0x123456789ABCDEF0ULL, &ab2) == S_OK && status(&ab2, true) == S_OK &&
              add_by_id_result(&ab2, &user2) == S_OK && user2 != nullptr &&
              get_id(user2, &xuid2) == S_OK && xuid2 == 0x123456789ABCDEF0ULL,
          "XUserAddByIdWithUiAsync preserves requested XUID");
    close_u(user2);
}

void remove_tree_best_effort(const std::string& dir) {
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        std::string p = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_tree_best_effort(p);
        } else {
            DeleteFileA(p.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir.c_str());
}

void check_xgamesave() {
    struct XGameSaveProvider;
    struct XGameSaveContainer;
    struct XGameSaveUpdate;
    using ProviderH = XGameSaveProvider*;
    using ContainerH = XGameSaveContainer*;
    using UpdateH = XGameSaveUpdate*;
    struct XGameSaveBlobInfo { const char* name; uint32_t size; };
    struct XGameSaveBlob { XGameSaveBlobInfo info; uint8_t* data; };
    struct XGameSaveContainerInfo {
        const char* name;
        const char* displayName;
        uint32_t blobCount;
        uint64_t totalSize;
        int64_t lastModifiedTime;
        bool needsSync;
    };
    using BlobInfoCb = bool(CALLBACK*)(const XGameSaveBlobInfo*, void*);
    using ContainerInfoCb = bool(CALLBACK*)(const XGameSaveContainerInfo*, void*);

    char tmp_base[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp_base);
    std::string save_dir = std::string(tmp_base) + "wavespan_xgs_test_" + std::to_string(GetCurrentProcessId());
    remove_tree_best_effort(save_dir);
    SetEnvironmentVariableA("WAVESPAN_SAVE_DIR", save_dir.c_str());

    auto init_sync = get<HRESULT(__stdcall*)(void*, const char*, bool, ProviderH*)>("XGameSaveInitializeProvider");
    auto init_async = get<HRESULT(__stdcall*)(void*, const char*, bool, XAsyncBlock*)>("XGameSaveInitializeProviderAsync");
    auto init_res = get<HRESULT(__stdcall*)(XAsyncBlock*, ProviderH*)>("XGameSaveInitializeProviderResult");
    auto close_prov = get<void(__stdcall*)(ProviderH)>("XGameSaveCloseProvider");
    auto quota_sync = get<HRESULT(__stdcall*)(ProviderH, int64_t*)>("XGameSaveGetRemainingQuota");
    auto quota_async = get<HRESULT(__stdcall*)(ProviderH, XAsyncBlock*)>("XGameSaveGetRemainingQuotaAsync");
    auto quota_res = get<HRESULT(__stdcall*)(XAsyncBlock*, int64_t*)>("XGameSaveGetRemainingQuotaResult");
    auto create_cont = get<HRESULT(__stdcall*)(ProviderH, const char*, ContainerH*)>("XGameSaveCreateContainer");
    auto close_cont = get<void(__stdcall*)(ContainerH)>("XGameSaveCloseContainer");
    auto del_cont = get<HRESULT(__stdcall*)(ProviderH, const char*)>("XGameSaveDeleteContainer");
    auto enum_cont = get<HRESULT(__stdcall*)(ProviderH, void*, ContainerInfoCb)>("XGameSaveEnumerateContainerInfo");
    auto get_cont = get<HRESULT(__stdcall*)(ProviderH, const char*, void*, ContainerInfoCb)>("XGameSaveGetContainerInfo");
    auto enum_blob = get<HRESULT(__stdcall*)(ContainerH, void*, BlobInfoCb)>("XGameSaveEnumerateBlobInfo");
    auto read_sync = get<HRESULT(__stdcall*)(ContainerH, const char**, uint32_t*, size_t, XGameSaveBlob*)>("XGameSaveReadBlobData");
    auto read_async = get<HRESULT(__stdcall*)(ContainerH, const char**, uint32_t, XAsyncBlock*)>("XGameSaveReadBlobDataAsync");
    auto read_res = get<HRESULT(__stdcall*)(XAsyncBlock*, size_t, XGameSaveBlob*, uint32_t*)>("XGameSaveReadBlobDataResult");
    auto create_upd = get<HRESULT(__stdcall*)(ContainerH, const char*, UpdateH*)>("XGameSaveCreateUpdate");
    auto close_upd = get<void(__stdcall*)(UpdateH)>("XGameSaveCloseUpdate");
    auto sub_write = get<HRESULT(__stdcall*)(UpdateH, const char*, const uint8_t*, size_t)>("XGameSaveSubmitBlobWrite");
    auto sub_del = get<HRESULT(__stdcall*)(UpdateH, const char*)>("XGameSaveSubmitBlobDelete");
    auto sub_upd = get<HRESULT(__stdcall*)(UpdateH)>("XGameSaveSubmitUpdate");
    auto sub_upd_async = get<HRESULT(__stdcall*)(UpdateH, XAsyncBlock*)>("XGameSaveSubmitUpdateAsync");
    auto sub_upd_res = get<HRESULT(__stdcall*)(XAsyncBlock*)>("XGameSaveSubmitUpdateResult");
    auto status = get<AsyncStatusFn>("XAsyncGetStatus");
    auto result_size = get<AsyncResultSizeFn>("XAsyncGetResultSize");

    ProviderH prov = nullptr;
    check(init_sync(nullptr, "cfg", false, &prov) == S_OK && prov != nullptr,
          "XGameSaveInitializeProvider succeeds offline");
    int64_t quota = 0;
    check(quota_sync(prov, &quota) == S_OK && quota > 0, "XGameSaveGetRemainingQuota returns positive offline quota");

    XAsyncBlock q_ab{};
    int64_t quota2 = 0;
    check(quota_async(prov, &q_ab) == S_OK && status(&q_ab, true) == S_OK &&
              quota_res(&q_ab, &quota2) == S_OK && quota2 == quota,
          "XGameSaveGetRemainingQuotaAsync matches sync quota");

    ContainerH cont = nullptr;
    check(create_cont(prov, "slot 1", &cont) == S_OK && cont != nullptr,
          "XGameSaveCreateContainer creates handle for container with space");

    UpdateH upd = nullptr;
    const uint8_t payload_a[] = {0x4A, 0x53, 0x41, 0x56, 0x03};
    const uint8_t payload_b[] = {1, 2, 3, 4, 5, 6};
    check(create_upd(cont, "Slot 1", &upd) == S_OK && upd != nullptr, "XGameSaveCreateUpdate succeeds");
    check(sub_write(upd, "main.sav", payload_a, sizeof(payload_a)) == S_OK, "XGameSaveSubmitBlobWrite queues blob");
    check(sub_write(upd, ".hidden", payload_b, sizeof(payload_b)) == S_OK,
          "XGameSaveSubmitBlobWrite queues dot-prefixed blob");
    check(sub_upd(upd) == S_OK, "XGameSaveSubmitUpdate writes blobs to disk");
    check(sub_upd(upd) == static_cast<HRESULT>(0x8083000B), "resubmitting an update returns E_GS_HANDLE_EXPIRED");
    close_upd(upd);

    // Verify %-encoding on disk matches tools/saves.py (space -> %20, leading dot -> %2E).
    DWORD attr = GetFileAttributesA((save_dir + "\\local-user\\slot%201\\%2Ehidden").c_str());
    check(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY),
          "XGameSave encodes container and blob names with tools/saves.py %-encoding");

    // Enumerate containers and blobs.
    struct SeenCont { std::string name; uint32_t count; uint64_t size; };
    std::vector<SeenCont> conts;
    check(enum_cont(prov, &conts, [](const XGameSaveContainerInfo* info, void* ctx) {
        static_cast<std::vector<SeenCont>*>(ctx)->push_back({info->name, info->blobCount, info->totalSize});
        return true;
    }) == S_OK && conts.size() == 1 && conts[0].name == "slot 1" && conts[0].count == 2 && conts[0].size == 11,
          "XGameSaveEnumerateContainerInfo decodes name and reports blobCount=2, totalSize=11");

    std::vector<SeenCont> one_cont;
    check(get_cont(prov, "slot 1", &one_cont, [](const XGameSaveContainerInfo* info, void* ctx) {
        static_cast<std::vector<SeenCont>*>(ctx)->push_back({info->name, info->blobCount, info->totalSize});
        return true;
    }) == S_OK && one_cont.size() == 1 && one_cont[0].name == "slot 1",
          "XGameSaveGetContainerInfo finds exact container");

    std::vector<std::string> blob_names;
    check(enum_blob(cont, &blob_names, [](const XGameSaveBlobInfo* info, void* ctx) {
        static_cast<std::vector<std::string>*>(ctx)->emplace_back(info->name);
        return true;
    }) == S_OK && blob_names.size() == 2,
          "XGameSaveEnumerateBlobInfo finds both blobs");

    // Read sync + buffer-too-small + missing blob.
    const char* req[] = {"main.sav", ".hidden"};
    uint32_t req_count = 2;
    std::vector<uint8_t> buf(256);
    check(read_sync(cont, req, &req_count, 4, reinterpret_cast<XGameSaveBlob*>(buf.data())) ==
              static_cast<HRESULT>(0x80830007),
          "XGameSaveReadBlobData returns E_GS_PROVIDED_BUFFER_TOO_SMALL when buffer is too small");
    const char* missing_req[] = {"no_such_blob"};
    uint32_t missing_count = 1;
    check(read_sync(cont, missing_req, &missing_count, buf.size(), reinterpret_cast<XGameSaveBlob*>(buf.data())) ==
              static_cast<HRESULT>(0x80830008),
          "XGameSaveReadBlobData returns E_GS_BLOB_NOT_FOUND for missing blob");
    check(read_sync(cont, req, &req_count, buf.size(), reinterpret_cast<XGameSaveBlob*>(buf.data())) == S_OK,
          "XGameSaveReadBlobData reads both blobs");
    auto* out_blobs = reinterpret_cast<XGameSaveBlob*>(buf.data());
    check(std::strcmp(out_blobs[0].info.name, "main.sav") == 0 && out_blobs[0].info.size == sizeof(payload_a) &&
              std::memcmp(out_blobs[0].data, payload_a, sizeof(payload_a)) == 0 &&
              std::strcmp(out_blobs[1].info.name, ".hidden") == 0 && out_blobs[1].info.size == sizeof(payload_b) &&
              std::memcmp(out_blobs[1].data, payload_b, sizeof(payload_b)) == 0,
          "XGameSaveReadBlobData returns byte-identical payloads");

    // Read async.
    XAsyncBlock r_ab{};
    size_t need = 0;
    check(read_async(cont, req, 2, &r_ab) == S_OK && status(&r_ab, true) == S_OK &&
              result_size(&r_ab, &need) == S_OK && need > sizeof(XGameSaveBlob) * 2,
          "XGameSaveReadBlobDataAsync completes and reports required buffer size");
    std::vector<uint8_t> abuf(need);
    uint32_t got_count = 0;
    check(read_res(&r_ab, abuf.size(), reinterpret_cast<XGameSaveBlob*>(abuf.data()), &got_count) == S_OK &&
              got_count == 2 &&
              std::memcmp(reinterpret_cast<XGameSaveBlob*>(abuf.data())[0].data, payload_a, sizeof(payload_a)) == 0,
          "XGameSaveReadBlobDataResult unpacks blobs and countOfBlobs");

    // Delete blob via async update.
    UpdateH upd2 = nullptr;
    XAsyncBlock u_ab{};
    check(create_upd(cont, nullptr, &upd2) == S_OK && sub_del(upd2, ".hidden") == S_OK &&
              sub_upd_async(upd2, &u_ab) == S_OK && status(&u_ab, true) == S_OK && sub_upd_res(&u_ab) == S_OK,
          "XGameSaveSubmitBlobDelete + XGameSaveSubmitUpdateAsync removes blob");
    close_upd(upd2);
    check(read_sync(cont, req, &req_count, buf.size(), reinterpret_cast<XGameSaveBlob*>(buf.data())) ==
              static_cast<HRESULT>(0x80830008),
          "deleted blob is no longer readable");

    close_cont(cont);
    check(del_cont(prov, "slot 1") == S_OK, "XGameSaveDeleteContainer succeeds");
    conts.clear();
    check(enum_cont(prov, &conts, [](const XGameSaveContainerInfo*, void*) { return true; }) == S_OK && conts.empty(),
          "deleted container is no longer enumerated");
    close_prov(prov);

    // Async provider init.
    XAsyncBlock p_ab{};
    ProviderH prov2 = nullptr;
    check(init_async(nullptr, nullptr, false, &p_ab) == S_OK && status(&p_ab, true) == S_OK &&
              init_res(&p_ab, &prov2) == S_OK && prov2 != nullptr,
          "XGameSaveInitializeProviderAsync/Result succeeds");
    close_prov(prov2);

    SetEnvironmentVariableA("WAVESPAN_SAVE_DIR", nullptr);
    remove_tree_best_effort(save_dir);
}

}  // namespace

int main() {
    std::printf("xgameruntime_test: the offline GDK runtime shim, through the shim registry\n");

    int missing = 0;
    for (const char* n : kNames)
        if (!rt::shims::find("xgameruntime.dll", n)) {
            std::printf("     not registered: %s\n", n);
            ++missing;
        }
    check(missing == 0, "every xgameruntime.dll function in the table resolves through rt::shims::find");
    if (missing) {
        std::printf("xgameruntime_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }

    auto init = get<InitFn>("XGameRuntimeInitialize");
    auto uninit = get<UninitFn>("XGameRuntimeUninitialize");
    check(init() == S_OK, "XGameRuntimeInitialize returns S_OK");
    check(init() == S_OK, "a second XGameRuntimeInitialize also returns S_OK (reference counted)");

    check(get<DeviceTypeFn>("XSystemGetDeviceType")() == 7, "XSystemGetDeviceType reports XboxScarlettAnaconda (7)");

    auto console_id = get<ConsoleIdFn>("XSystemGetConsoleId");
    char id[64] = {};
    size_t used = 0;
    check(console_id(8, id, &used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
          "XSystemGetConsoleId rejects a buffer smaller than XSystemConsoleIdBytes");
    check(console_id(sizeof(id), id, &used) == S_OK && used == 39 && std::strlen(id) == 38,
          "XSystemGetConsoleId fills 39 bytes (38 characters and the terminator)");

    auto analytics_info = get<AnalyticsInfoFn>("XSystemGetAnalyticsInfo");
    XSystemAnalyticsInfo info = analytics_info();
    check(info.osVersion.major == 10 && info.osVersion.minor == 0 && info.osVersion.build == 22000 &&
              info.osVersion.revision == 0,
          "XSystemGetAnalyticsInfo returns osVersion 10.0.22000.0");
    check(info.hostingOsVersion.major == 10 && info.hostingOsVersion.minor == 0 &&
              info.hostingOsVersion.build == 22000 && info.hostingOsVersion.revision == 0,
          "XSystemGetAnalyticsInfo returns hostingOsVersion 10.0.22000.0");
    check(std::strcmp(info.family, "Xbox") == 0, "XSystemGetAnalyticsInfo returns family 'Xbox'");
    check(std::strcmp(info.form, "Xbox Series X") == 0, "XSystemGetAnalyticsInfo returns form 'Xbox Series X'");

    auto package_id = get<PackageIdentifierFn>("XPackageGetCurrentProcessPackageIdentifier");
    check(package_id(0, nullptr) == E_INVALIDARG, "XPackageGetCurrentProcessPackageIdentifier rejects null buffer and size 0");
    char pkg_id[64] = {};
    check(package_id(0, pkg_id) == E_INVALIDARG, "XPackageGetCurrentProcessPackageIdentifier rejects size 0 with buffer");
    check(package_id(10, nullptr) == E_INVALIDARG, "XPackageGetCurrentProcessPackageIdentifier rejects null pointer with size");
    check(package_id(16, pkg_id) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
          "XPackageGetCurrentProcessPackageIdentifier rejects insufficient buffer");
    check(package_id(sizeof(pkg_id), pkg_id) == S_OK && std::strlen(pkg_id) == 32,
          "XPackageGetCurrentProcessPackageIdentifier fills 33-byte package identifier");

    SetEnvironmentVariableA("WAVESPAN_PACKAGE_ID", "CustomTestPackageId_123456789012");
    check(package_id(sizeof(pkg_id), pkg_id) == S_OK &&
              std::strcmp(pkg_id, "CustomTestPackageId_123456789012") == 0,
          "XPackageGetCurrentProcessPackageIdentifier respects WAVESPAN_PACKAGE_ID override");
    SetEnvironmentVariableA("WAVESPAN_PACKAGE_ID", nullptr);

    auto mount_path = get<PackageMountPathFn>("XPackageGetMountPath");
    check(mount_path(nullptr, 0, nullptr) == E_INVALIDARG, "XPackageGetMountPath rejects null buffer and size 0");
    char mnt_path[MAX_PATH] = {};
    check(mount_path(nullptr, 0, mnt_path) == E_INVALIDARG, "XPackageGetMountPath rejects size 0 with buffer");
    check(mount_path(nullptr, 10, nullptr) == E_INVALIDARG, "XPackageGetMountPath rejects null pointer with size");

    SetEnvironmentVariableA("WAVESPAN_GAME_DIR", "C:\\WaveSpanGameDir");
    check(mount_path(nullptr, 5, mnt_path) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
          "XPackageGetMountPath rejects insufficient buffer");
    check(mount_path(nullptr, sizeof(mnt_path), mnt_path) == S_OK &&
              std::strcmp(mnt_path, "C:\\WaveSpanGameDir") == 0,
          "XPackageGetMountPath returns configured game directory");
    SetEnvironmentVariableA("WAVESPAN_GAME_DIR", nullptr);

    auto is_packaged = get<PackagedProcessFn>("XPackageIsPackagedProcess");
    check(is_packaged() == true, "XPackageIsPackagedProcess returns true by default");
    SetEnvironmentVariableA("WAVESPAN_PACKAGED", "0");
    check(is_packaged() == false, "XPackageIsPackagedProcess respects WAVESPAN_PACKAGED=0 override");
    SetEnvironmentVariableA("WAVESPAN_PACKAGED", nullptr);

    // Work on the thread pool, completions dispatched by the "game loop" on this thread.
    auto create = get<QueueCreateFn>("XTaskQueueCreate");
    auto close = get<QueueCloseFn>("XTaskQueueCloseHandle");
    auto dispatch = get<QueueDispatchFn>("XTaskQueueDispatch");
    auto run = get<AsyncRunFn>("XAsyncRun");
    auto status = get<AsyncStatusFn>("XAsyncGetStatus");
    XTaskQueueHandle q = nullptr;
    check(create(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, &q) == S_OK && q,
          "XTaskQueueCreate(ThreadPool work, Manual completion)");
    int result = 0;
    XAsyncBlock block{};
    block.queue = q;
    block.context = &result;
    block.callback = completed;
    check(run(&block, work) == S_OK, "XAsyncRun accepts work on that queue");
    for (int i = 0; i < 500 && g_completions == 0; ++i) {
        while (dispatch(q, XTaskQueuePort::Completion, 0)) {}
        if (!g_completions) Sleep(2);
    }
    check(g_work_runs == 1 && result == 42, "the work callback ran once and wrote its result");
    check(g_work_thread != GetCurrentThreadId(), "the work callback ran on a thread-pool thread");
    check(g_completions == 1 && g_completion_thread == GetCurrentThreadId(),
          "the completion callback ran once, on the thread that dispatched the completion port");
    check(status(&block, false) == S_OK, "XAsyncGetStatus reports S_OK after completion");
    close(q);

    // The process task queue, used when XAsyncBlock::queue is null.
    XTaskQueueHandle pq = nullptr;
    check(get<QueueProcessFn>("XTaskQueueGetCurrentProcessTaskQueue")(&pq) && pq,
          "XTaskQueueGetCurrentProcessTaskQueue returns a queue");
    if (pq) close(pq);
    int result2 = 0;
    XAsyncBlock block2{};
    block2.context = &result2;
    check(run(&block2, work) == S_OK && status(&block2, true) == S_OK && result2 == 42,
          "XAsyncRun with a null queue completes on the process queue");

    // A custom provider: XAsyncBegin, XAsyncSchedule, XAsyncComplete, XAsyncGetResultSize, XAsyncGetResult.
    ProviderState st{get<AsyncCompleteFn>("XAsyncComplete")};
    XAsyncBlock block3{};
    static const int kIdentity = 0;
    HRESULT hr = get<AsyncBeginFn>("XAsyncBegin")(&block3, &st, &kIdentity, "xgameruntime_test", provider);
    check(hr == S_OK, "XAsyncBegin starts a provider");
    check(get<AsyncScheduleFn>("XAsyncSchedule")(&block3, 0) == S_OK, "XAsyncSchedule queues its work");
    check(status(&block3, true) == S_OK, "the provider completes with S_OK");
    size_t size = 0;
    check(get<AsyncResultSizeFn>("XAsyncGetResultSize")(&block3, &size) == S_OK && size == sizeof(uint32_t),
          "XAsyncGetResultSize reports the 4-byte result");
    uint32_t value = 0;
    size_t got = 0;
    check(get<AsyncResultFn>("XAsyncGetResult")(&block3, &kIdentity, sizeof(value), &value, &got) == S_OK &&
              value == 0x1EA0 && got == sizeof(value),
          "XAsyncGetResult returns the provider's result");
    // Cleanup comes when the async state's last reference goes, which can be on the work thread just after the
    // result is read, so allow it a moment.
    const int cleanup_bit = 1 << static_cast<int>(XAsyncOp::Cleanup);
    for (int i = 0; i < 500 && !(st.ops & cleanup_bit); ++i) Sleep(2);
    check((st.ops & (1 << static_cast<int>(XAsyncOp::Begin))) && (st.ops & (1 << static_cast<int>(XAsyncOp::DoWork))) &&
              (st.ops & (1 << static_cast<int>(XAsyncOp::GetResult))) && (st.ops & (1 << static_cast<int>(XAsyncOp::Cleanup))),
          "the provider saw Begin, DoWork, GetResult and Cleanup");

    uninit();
    uninit();
    check(init() == S_OK, "XGameRuntimeInitialize works again after balanced uninitialize calls");
    uninit();

    std::atexit(check_late_teardown);
    check_run_time_load();
    check_xsystem_extra();
    check_xuser();
    check_xgamesave();

    if (g_failures) {
        std::printf("xgameruntime_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("xgameruntime_test: OK (%d checks)\n", g_checks);
    return 0;
}
