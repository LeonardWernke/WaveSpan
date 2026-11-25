// xgameruntime: the GDK runtime surface a title calls before anything else, offline.
//
// Every GDK title starts with XGameRuntimeInitialize and builds its async work on XTaskQueue/XAsync. This table
// serves those from third_party/xtaskqueue (Microsoft's MIT-licensed implementation from libHttpClient), plus the
// XGameRuntime and XSystem calls a title makes at boot. Signatures and enum values follow Microsoft's public GDK
// documentation (learn.microsoft.com/gaming/gdk); no GDK header or library code is used.
//
// Nothing here goes online.
//
// ASSUMPTION: the console executable is not available, so the DLL name it imports these through is unknown.
// "xgameruntime.dll" is the name the PC GDK uses. If the executable's import table names another module, register
// the same functions under that name too; nothing else changes.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "shims.h"

namespace {

// ---- XGameRuntime ----

std::atomic<int> g_runtime_refs{0};

// ---- The run-time entry points xgameruntime.lib looks up ----
//
// A title linked against xgameruntime.lib doesn't import XGameRuntimeInitialize: the static library's thunks load
// "xgameruntime.dll" and GetProcAddress these five names (measured in the package's libHttpClient.GDK.dll and
// XCurl.dll, docs/runtime_imports.md). Signatures, the options struct and the error for an unknown class follow
// xodus-gaming/xgameruntime-docs (GPL-3.0; facts only, nothing copied): InitializeApiImplEx2 is the one
// XGameRuntimeInitialize[WithOptions] calls, and QueryApiImpl hands out the runtime's COM classes. XThreadingImpl and
// XErrorImpl are served (their layouts are measured, below); for any other class QueryApiImpl logs the class and
// interface id and refuses it, so the first boot with the executable lists the interfaces to implement next.

struct ApiImplOptions {    // InitializeApiImplEx2's fourth argument
    uint32_t unknown;      // documented as always 1
    BOOL is_inline;        // 1 when XGameRuntimeOptions::gameConfigSource is Inline
    const char* game_config;
};

constexpr HRESULT kClassNotProvided = static_cast<HRESULT>(0x80070032);  // HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)

std::atomic<int> g_api_refs{0};

HRESULT api_initialize(const char* which, ULONG gdk_ver, ULONG gs_ver, int mode, const ApiImplOptions* options) {
    if (g_api_refs.fetch_add(1) == 0)
        std::fprintf(stderr, "[rt] xgameruntime: %s(gdk=%lu, gs=%lu, mode=%#x, options=%s) (offline)\n", which,
                     gdk_ver, gs_ver, mode & 0xFF,
                     options ? (options->is_inline ? "inline game config" : "default game config") : "none");
    return S_OK;
}

HRESULT WINAPI InitializeApiImpl(ULONG gdk_ver, ULONG gs_ver) {
    return api_initialize("InitializeApiImpl", gdk_ver, gs_ver, -1, nullptr);
}

HRESULT WINAPI InitializeApiImplEx(ULONG gdk_ver, ULONG gs_ver, char mode) {
    return api_initialize("InitializeApiImplEx", gdk_ver, gs_ver, static_cast<unsigned char>(mode), nullptr);
}

HRESULT WINAPI InitializeApiImplEx2(ULONG gdk_ver, ULONG gs_ver, char mode, const ApiImplOptions* options) {
    return api_initialize("InitializeApiImplEx2", gdk_ver, gs_ver, static_cast<unsigned char>(mode), options);
}

HRESULT WINAPI UninitializeApiImpl() {
    int prev = g_api_refs.load();
    while (prev > 0 && !g_api_refs.compare_exchange_weak(prev, prev - 1)) {}
    return S_OK;
}

void print_guid(char (&out)[40], const GUID* g) {
    if (!g) {
        std::snprintf(out, sizeof out, "(null)");
        return;
    }
    std::snprintf(out, sizeof out, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2,
                  g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                  g->Data4[6], g->Data4[7]);
}

// ---- The runtime classes QueryApiImpl hands out ----
//
// xgameruntime.lib's thunk for each function asks QueryApiImpl for the class (passing the class id as the interface
// id too), calls one slot of the object's vtable with the function's own arguments after `this`, and Releases it.
// The slot order is xgameruntime-docs' "Layout" list after IUnknown's three slots, and it is measured: the thunks
// statically linked into the package's libHttpClient.GDK.dll and XCurl.dll (disassembled, their code not run)
// call IXThreadingImpl slots 3-7 and 9-30, each with the argument shape of the documented function in that
// position (XAsyncBegin's five arguments at slot 7, XTaskQueueCreate's two modes and a handle pointer at 12,
// XTaskQueueTerminate's bool at 22, XThreadSetTimeSensitive's bool at 27, ...), and IXErrorImpl slot 3 with an
// HRESULT and a pointer. docs/runtime_imports.md has the table. Each function is served by the implementation the
// named export already uses (third_party/xtaskqueue), so the two ways in behave the same.

constexpr GUID kThreadingClass = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
constexpr GUID kErrorClass = {0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};

// A function's arguments after `this`, forwarded unchanged (x64 has one calling convention).
// Specialised on the function's exact type, so a noexcept function matches only one form.
template <auto F, class T = decltype(F)> struct Method;
template <auto F, class R, class... A> struct Method<F, R (*)(A...)> {
    static R STDMETHODCALLTYPE call(void*, A... a) { return F(a...); }
};
template <auto F, class R, class... A> struct Method<F, R (*)(A...) noexcept> {
    static R STDMETHODCALLTYPE call(void*, A... a) noexcept { return F(a...); }
};

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// The runtime's objects are process-wide singletons: AddRef/Release count but never free.
struct RuntimeObject {
    const void* const* vtable;
    const GUID* class_id;
    std::atomic<long> refs{1};
};

HRESULT STDMETHODCALLTYPE object_query_interface(void* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    auto* o = static_cast<RuntimeObject*>(self);
    if (iid == kIUnknown || iid == *o->class_id) {
        o->refs.fetch_add(1);
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE object_add_ref(void* self) { return static_cast<ULONG>(static_cast<RuntimeObject*>(self)->refs.fetch_add(1) + 1); }
ULONG STDMETHODCALLTYPE object_release(void* self) {
    long prev = static_cast<RuntimeObject*>(self)->refs.load();
    while (prev > 1 && !static_cast<RuntimeObject*>(self)->refs.compare_exchange_weak(prev, prev - 1)) {}
    return static_cast<ULONG>(prev > 1 ? prev - 1 : 1);  // the singleton's own reference stays
}

// A private slot whose signature isn't public: logged once, answers 0 (S_OK / false / nothing to do).
template <int Slot> uint64_t STDMETHODCALLTYPE private_slot(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: XThreadingImpl private slot %d called; answering 0\n", Slot);
    return 0;
}

// XThreadSetTimeSensitive / XThreadIsTimeSensitive / XThreadAssertNotTimeSensitive. The flag is per thread, as in
// the GDK; nothing is enforced offline (a time-sensitive thread may still block).
thread_local bool t_time_sensitive = false;
HRESULT STDMETHODCALLTYPE thread_set_time_sensitive(void*, bool value) {
    t_time_sensitive = value;
    return S_OK;
}
void STDMETHODCALLTYPE thread_assert_not_time_sensitive(void*) {}
bool STDMETHODCALLTYPE thread_is_time_sensitive(void*) { return t_time_sensitive; }

#define XM(fn) reinterpret_cast<const void*>(&Method<&fn>::call)
const void* const kThreadingVtable[] = {
    reinterpret_cast<const void*>(&object_query_interface), reinterpret_cast<const void*>(&object_add_ref),
    reinterpret_cast<const void*>(&object_release),
    XM(XAsyncGetStatus),                    // 3
    XM(XAsyncGetResultSize),                // 4
    XM(XAsyncCancel),                       // 5
    XM(XAsyncRun),                          // 6
    XM(XAsyncBegin),                        // 7
    reinterpret_cast<const void*>(&private_slot<8>),  // 8  XAsyncBeginAlloc (private, not called by the package)
    XM(XAsyncSchedule),                     // 9
    XM(XAsyncComplete),                     // 10
    XM(XAsyncGetResult),                    // 11
    XM(XTaskQueueCreate),                   // 12
    XM(XTaskQueueCreateComposite),          // 13
    XM(XTaskQueueGetPort),                  // 14
    XM(XTaskQueueDuplicateHandle),          // 15
    XM(XTaskQueueDispatch),                 // 16
    XM(XTaskQueueCloseHandle),              // 17
    XM(XTaskQueueSubmitCallback),           // 18
    XM(XTaskQueueSubmitDelayedCallback),    // 19
    XM(XTaskQueueRegisterWaiter),           // 20
    XM(XTaskQueueUnregisterWaiter),         // 21
    XM(XTaskQueueTerminate),                // 22
    XM(XTaskQueueRegisterMonitor),          // 23
    XM(XTaskQueueUnregisterMonitor),        // 24
    XM(XTaskQueueGetCurrentProcessTaskQueue),  // 25
    XM(XTaskQueueSetCurrentProcessTaskQueue),  // 26
    reinterpret_cast<const void*>(&thread_set_time_sensitive),         // 27
    reinterpret_cast<const void*>(&private_slot<28>),                  // 28 XTaskQueueIsEmpty (private; no arguments)
    reinterpret_cast<const void*>(&thread_assert_not_time_sensitive),  // 29
    reinterpret_cast<const void*>(&thread_is_time_sensitive),          // 30
};
#undef XM
static_assert(std::size(kThreadingVtable) == 31, "IUnknown + IXThreadingImpl's 28 methods");

// IXErrorImpl: XErrorReport(HRESULT, const char*) at slot 3 (the docs' "_unknown_"; XCurl calls it with an HRESULT
// and a string), XErrorSetCallback(XErrorCallback*, void*) and XErrorSetOptions(options, options). Offline, a report
// is logged and handed to the callback the title set, if any.
using XErrorCallback = bool(CALLBACK*)(HRESULT hr, const char* message, void* context);
std::atomic<XErrorCallback> g_error_callback{nullptr};
std::atomic<void*> g_error_context{nullptr};

void STDMETHODCALLTYPE error_report(void*, HRESULT hr, const char* message) {
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) < 64)
        std::fprintf(stderr, "[rt] xgameruntime: XErrorReport(0x%08lx, %s)\n", static_cast<unsigned long>(hr),
                     message ? message : "(null)");
    if (XErrorCallback cb = g_error_callback.load()) cb(hr, message, g_error_context.load());
}
void STDMETHODCALLTYPE error_set_callback(void*, XErrorCallback callback, void* context) {
    g_error_context = context;
    g_error_callback = callback;
}
HRESULT STDMETHODCALLTYPE error_set_options(void*, uint32_t, uint32_t) { return S_OK; }

const void* const kErrorVtable[] = {
    reinterpret_cast<const void*>(&object_query_interface), reinterpret_cast<const void*>(&object_add_ref),
    reinterpret_cast<const void*>(&object_release),
    reinterpret_cast<const void*>(&error_report),        // 3
    reinterpret_cast<const void*>(&error_set_callback),  // 4
    reinterpret_cast<const void*>(&error_set_options),   // 5
};

RuntimeObject g_threading{kThreadingVtable, &kThreadingClass};
RuntimeObject g_error{kErrorVtable, &kErrorClass};

HRESULT WINAPI QueryApiImpl(const GUID* class_id, REFIID interface_id, void** out) {
    if (out) *out = nullptr;
    if (class_id && out) {
        for (RuntimeObject* o : {&g_threading, &g_error})
            if (*class_id == *o->class_id) return object_query_interface(o, interface_id, out);
    }
    char cls[40], iid[40];
    print_guid(cls, class_id);
    print_guid(iid, &interface_id);
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) < 256)   // one line per request, bounded
        std::fprintf(stderr, "[rt] xgameruntime: QueryApiImpl(class %s, interface %s): not provided yet\n", cls, iid);
    return kClassNotProvided;
}

// HRESULT XGameRuntimeInitialize(). Reference counted, as in the GDK: each call needs an XGameRuntimeUninitialize.
HRESULT __stdcall XGameRuntimeInitialize() {
    if (g_runtime_refs.fetch_add(1) == 0) std::fprintf(stderr, "[rt] xgameruntime: initialized (offline)\n");
    return S_OK;
}

// void XGameRuntimeUninitialize()
void __stdcall XGameRuntimeUninitialize() {
    int prev = g_runtime_refs.load();
    while (prev > 0 && !g_runtime_refs.compare_exchange_weak(prev, prev - 1)) {}
}

// ---- XSystem ----

// XSystemDeviceType, in the order of the public documentation: Unknown, Pc, XboxOne, XboxOneS, XboxOneX,
// XboxOneXDevkit, XboxScarlettLockhart, XboxScarlettAnaconda, XboxScarlettDevkit.
enum class XSystemDeviceType : uint32_t {
    Unknown = 0, Pc = 1, XboxOne = 2, XboxOneS = 3, XboxOneX = 4, XboxOneXDevkit = 5,
    XboxScarlettLockhart = 6, XboxScarlettAnaconda = 7, XboxScarlettDevkit = 8,
};

// XSystemDeviceType XSystemGetDeviceType(). The package is the Xbox Series build, so report Series X (Anaconda).
// WAVESPAN_DEVICE_TYPE=<0..8> overrides it, for trying other code paths on day one.
uint32_t __stdcall XSystemGetDeviceType() {
    char s[16];
    const DWORD n = GetEnvironmentVariableA("WAVESPAN_DEVICE_TYPE", s, sizeof(s));
    if (n > 0 && n < sizeof(s)) {
        int v = std::atoi(s);
        if (v >= 0 && v <= 8) return static_cast<uint32_t>(v);
    }
    return static_cast<uint32_t>(XSystemDeviceType::XboxScarlettAnaconda);
}

// HRESULT XSystemGetConsoleId(size_t consoleIdSize, char* consoleId, size_t* consoleIdUsed).
// XSystemConsoleIdBytes is 39 (including the terminator). An offline PC has no console id, so this returns a
// fixed placeholder of that length.
constexpr size_t kConsoleIdBytes = 39;
constexpr char kConsoleId[kConsoleIdBytes] = "00000000.00000000.00000000.00000000.00";
static_assert(sizeof(kConsoleId) == kConsoleIdBytes, "placeholder console id must fill XSystemConsoleIdBytes");

HRESULT __stdcall XSystemGetConsoleId(size_t size, char* out, size_t* used) {
    if (!out) return E_INVALIDARG;
    if (size < kConsoleIdBytes) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(out, kConsoleId, kConsoleIdBytes);
    if (used) *used = kConsoleIdBytes;
    return S_OK;
}

// uint64_t XSystemGetAppMemoryUsageLimit(). Reports total physical RAM, or 12 GB fallback for Series X.
uint64_t __stdcall XSystemGetAppMemoryUsageLimit() {
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) return ms.ullTotalPhys;
    return 12ULL * 1024 * 1024 * 1024;
}

// uint64_t XSystemGetAppMemoryUsage()
uint64_t __stdcall XSystemGetAppMemoryUsage() {
    return 0;
}

// XVersion: 4-part version structure from GDK XGameRuntimeTypes.h (8 bytes)
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
static_assert(sizeof(XVersion) == 8, "XVersion must be 8 bytes");

// XSystemAnalyticsInfo: hardware and OS version telemetry (144 bytes)
struct XSystemAnalyticsInfo {
    XVersion osVersion;
    XVersion hostingOsVersion;
    char family[64];
    char form[64];
};
static_assert(sizeof(XSystemAnalyticsInfo) == 144, "XSystemAnalyticsInfo must be 144 bytes");

// XSystemAnalyticsInfo XSystemGetAnalyticsInfo()
// Returned by value per Microsoft GDK specification.
XSystemAnalyticsInfo __stdcall XSystemGetAnalyticsInfo() {
    XSystemAnalyticsInfo info{};
    info.osVersion = {10, 0, 22000, 0};
    info.hostingOsVersion = {10, 0, 22000, 0};
    std::strncpy(info.family, "Xbox", sizeof(info.family) - 1);

    const uint32_t dt = XSystemGetDeviceType();
    if (dt == static_cast<uint32_t>(XSystemDeviceType::XboxScarlettLockhart)) {
        std::strncpy(info.form, "Xbox Series S", sizeof(info.form) - 1);
    } else {
        std::strncpy(info.form, "Xbox Series X", sizeof(info.form) - 1);
    }

    char form_override[64];
    const DWORD n = GetEnvironmentVariableA("WAVESPAN_ANALYTICS_FORM", form_override, sizeof(form_override));
    if (n > 0 && n < sizeof(form_override)) {
        std::strncpy(info.form, form_override, sizeof(info.form) - 1);
    }
    return info;
}

// HRESULT XSystemGetXboxLiveSandboxId(size_t sandboxIdSize, char* sandboxId, size_t* sandboxIdUsed).
// XSystemXboxLiveSandboxIdMaxBytes is 16 (including the terminator).
constexpr size_t kSandboxIdMaxBytes = 16;

HRESULT __stdcall XSystemGetXboxLiveSandboxId(size_t sandboxIdSize, char* sandboxId, size_t* sandboxIdUsed) {
    if (!sandboxId) return E_INVALIDARG;
    constexpr char kDefaultSandbox[] = "RETAIL";
    char env_buf[kSandboxIdMaxBytes] = {};
    const DWORD n = GetEnvironmentVariableA("WAVESPAN_SANDBOX_ID", env_buf, sizeof(env_buf));
    const char* selected = (n > 0 && n < sizeof(env_buf)) ? env_buf : kDefaultSandbox;
    const size_t needed = std::strlen(selected) + 1;
    if (sandboxIdSize < needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(sandboxId, selected, needed);
    if (sandboxIdUsed) *sandboxIdUsed = needed;
    return S_OK;
}

struct XSystemRuntimeInfo {
    XVersion runtimeVersion;
    XVersion availableVersion;
};
static_assert(sizeof(XSystemRuntimeInfo) == 16, "XSystemRuntimeInfo must be 16 bytes");

HRESULT __stdcall XSystemGetRuntimeInfo(XSystemRuntimeInfo* runtimeInfo) {
    if (!runtimeInfo) return E_INVALIDARG;
    runtimeInfo->runtimeVersion = {10, 0, 22000, 0};
    runtimeInfo->availableVersion = {10, 0, 22000, 0};
    return S_OK;
}

// ---- XUser (offline local dummy user) ----

struct XUser {
    std::atomic<long> refs{1};
    uint64_t xuid{0x000901F400000001ULL};
    uint64_t local_id{1};
};
using XUserHandle = XUser*;

enum class XUserAddOptions : uint32_t {
    None = 0x00,
    AddDefaultUserSilently = 0x01,
    AllowGuests = 0x02,
    AddDefaultUserAllowingUI = 0x04,
};

enum class XUserState : uint32_t {
    SignedIn = 0,
    SigningOut = 1,
    SignedOut = 2,
};

enum class XUserGetGamertagComponent : uint32_t {
    Classic = 0,
    Modern = 1,
    ModernSuffix = 2,
    UniqueModern = 3,
};

struct XUserLocalId {
    uint64_t value;
};

constexpr int kUserAddIdentity = 0;

struct UserAddContext {
    uint64_t xuid;
    XUserHandle user{nullptr};
};

HRESULT CALLBACK user_add_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<UserAddContext*>(data->context);
    switch (op) {
        case XAsyncOp::DoWork: {
            auto* u = new XUser{};
            if (ctx->xuid != 0) u->xuid = ctx->xuid;
            ctx->user = u;
            XAsyncComplete(data->async, S_OK, sizeof(XUserHandle));
            return S_OK;
        }
        case XAsyncOp::GetResult:
            if (data->bufferSize < sizeof(XUserHandle)) return E_INVALIDARG;
            std::memcpy(data->buffer, &ctx->user, sizeof(XUserHandle));
            ctx->user = nullptr;  // ownership transferred to caller
            return S_OK;
        case XAsyncOp::Cleanup:
            if (ctx->user) delete ctx->user;
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT __stdcall XUserAddAsync(XUserAddOptions /*options*/, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    auto* ctx = new UserAddContext{0, nullptr};
    HRESULT hr = XAsyncBegin(async, ctx, &kUserAddIdentity, "XUserAddAsync", user_add_provider);
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XUserAddResult(XAsyncBlock* async, XUserHandle* newUser) {
    if (!async || !newUser) return E_INVALIDARG;
    return XAsyncGetResult(async, &kUserAddIdentity, sizeof(XUserHandle), newUser, nullptr);
}

HRESULT __stdcall XUserAddByIdWithUiAsync(uint64_t userId, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    auto* ctx = new UserAddContext{userId, nullptr};
    HRESULT hr = XAsyncBegin(async, ctx, &kUserAddIdentity, "XUserAddByIdWithUiAsync", user_add_provider);
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XUserAddByIdWithUiResult(XAsyncBlock* async, XUserHandle* newUser) {
    if (!async || !newUser) return E_INVALIDARG;
    return XAsyncGetResult(async, &kUserAddIdentity, sizeof(XUserHandle), newUser, nullptr);
}

HRESULT __stdcall XUserDuplicateHandle(XUserHandle handle, XUserHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) return E_INVALIDARG;
    handle->refs.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return S_OK;
}

void __stdcall XUserCloseHandle(XUserHandle user) {
    if (!user) return;
    if (user->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete user;
}

HRESULT __stdcall XUserGetId(XUserHandle user, uint64_t* userId) {
    if (!user || !userId) return E_INVALIDARG;
    *userId = user->xuid;
    return S_OK;
}

HRESULT __stdcall XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId) {
    if (!user || !userLocalId) return E_INVALIDARG;
    userLocalId->value = user->local_id;
    return S_OK;
}

HRESULT __stdcall XUserGetState(XUserHandle user, XUserState* state) {
    if (!user || !state) return E_INVALIDARG;
    *state = XUserState::SignedIn;
    return S_OK;
}

HRESULT __stdcall XUserGetGamertag(XUserHandle user, XUserGetGamertagComponent gamertagComponent, size_t gamertagSize,
                                   char* gamertag, size_t* gamertagUsed) {
    if (!user || !gamertag || gamertagSize == 0) return E_INVALIDARG;
    const char* tag = "WaveSpanUser";
    if (gamertagComponent == XUserGetGamertagComponent::ModernSuffix) {
        tag = "";
    } else if (gamertagComponent == XUserGetGamertagComponent::UniqueModern) {
        tag = "WaveSpanUser#0001";
    }
    const size_t needed = std::strlen(tag) + 1;
    if (gamertagSize < needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(gamertag, tag, needed);
    if (gamertagUsed) *gamertagUsed = needed;
    return S_OK;
}

int32_t __stdcall XUserCompare(XUserHandle user1, XUserHandle user2) {
    if (user1 == user2) return 0;
    if (!user1) return -1;
    if (!user2) return 1;
    if (user1->local_id < user2->local_id) return -1;
    if (user1->local_id > user2->local_id) return 1;
    return 0;
}

// ---- XGameSave (local folder-backed storage) ----
//
// Layout matches docs/saves.md and tools/saves.py:
//   <WAVESPAN_SAVE_DIR or %LOCALAPPDATA%\WaveSpan\saves>\local-user\<container>\<blob>
// Names are %-encoded (keeping [A-Za-z0-9_-] and '.' except as the first character).

constexpr HRESULT E_GS_INVALID_CONTAINER_NAME = static_cast<HRESULT>(0x80830001);
constexpr HRESULT E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<HRESULT>(0x80830007);
constexpr HRESULT E_GS_BLOB_NOT_FOUND = static_cast<HRESULT>(0x80830008);
constexpr HRESULT E_GS_HANDLE_EXPIRED = static_cast<HRESULT>(0x8083000B);
constexpr int64_t kOfflineQuotaBytes = 256LL * 1024 * 1024;

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string encode_name(const char* name) {
    std::string out;
    if (!name) return out;
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        const unsigned char c = *p;
        const bool is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (is_alnum || c == '_' || c == '-' || (c == '.' && !out.empty())) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string decode_name(const std::string& encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const int hi = hex_val(encoded[i + 1]);
            const int lo = hex_val(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(encoded[i]);
    }
    return out;
}

std::wstring get_save_user_dir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"WAVESPAN_SAVE_DIR", buf, MAX_PATH);
    std::wstring base;
    if (n > 0 && n < MAX_PATH) {
        base.assign(buf, n);
    } else {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            base.assign(buf, n);
        } else {
            base = L".";
        }
        base += L"\\WaveSpan\\saves";
    }
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    base += L"\\local-user";
    return base;
}

bool create_directories_recursive(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring cur;
    cur.reserve(dir.size());
    for (size_t i = 0; i < dir.size(); ++i) {
        const wchar_t ch = dir[i];
        cur.push_back(ch);
        if ((ch == L'\\' || ch == L'/') && cur.size() > 1) {
            if (cur.size() == 3 && cur[1] == L':') continue;  // drive root like "C:\"
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

int64_t filetime_to_unix(const FILETIME& ft) {
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr uint64_t kEpochDiff = 116444736000000000ULL;
    if (t < kEpochDiff) return 0;
    return static_cast<int64_t>((t - kEpochDiff) / 10000000ULL);
}

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    int64_t lastModifiedTime;
    bool needsSync;
};

using XGameSaveBlobInfoCallback = bool CALLBACK(const XGameSaveBlobInfo* info, void* context);
using XGameSaveContainerInfoCallback = bool CALLBACK(const XGameSaveContainerInfo* info, void* context);

struct XGameSaveProvider {
    std::atomic<long> refs{1};
    std::wstring user_dir;
    std::mutex io_mutex;
};
using XGameSaveProviderHandle = XGameSaveProvider*;

struct XGameSaveContainer {
    std::atomic<long> refs{1};
    XGameSaveProviderHandle provider;
    std::string name;
    std::wstring dir;
};
using XGameSaveContainerHandle = XGameSaveContainer*;

struct PendingBlobOp {
    bool is_delete{false};
    std::string blob_name;
    std::vector<uint8_t> data;
};

struct XGameSaveUpdate {
    std::atomic<long> refs{1};
    XGameSaveContainerHandle container;
    std::vector<PendingBlobOp> ops;
    bool submitted{false};
};
using XGameSaveUpdateHandle = XGameSaveUpdate*;

void retain_provider(XGameSaveProviderHandle p) {
    if (p) p->refs.fetch_add(1, std::memory_order_relaxed);
}
void release_provider(XGameSaveProviderHandle p) {
    if (p && p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete p;
}
void retain_container(XGameSaveContainerHandle c) {
    if (c) c->refs.fetch_add(1, std::memory_order_relaxed);
}
void release_container(XGameSaveContainerHandle c) {
    if (c && c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        release_provider(c->provider);
        delete c;
    }
}
void retain_update(XGameSaveUpdateHandle u) {
    if (u) u->refs.fetch_add(1, std::memory_order_relaxed);
}
void release_update(XGameSaveUpdateHandle u) {
    if (u && u->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        release_container(u->container);
        delete u;
    }
}

HRESULT __stdcall XGameSaveInitializeProvider(XUserHandle /*requestingUser*/, const char* /*configurationId*/,
                                              bool /*syncOnDemand*/, XGameSaveProviderHandle* provider) {
    if (!provider) return E_INVALIDARG;
    auto* p = new XGameSaveProvider{};
    p->user_dir = get_save_user_dir();
    create_directories_recursive(p->user_dir);
    *provider = p;
    return S_OK;
}

constexpr int kGsInitProviderId = 0;
struct GsInitProviderCtx {
    XGameSaveProviderHandle provider{nullptr};
};

HRESULT CALLBACK gs_init_provider_fn(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<GsInitProviderCtx*>(data->context);
    switch (op) {
        case XAsyncOp::DoWork: {
            HRESULT hr = XGameSaveInitializeProvider(nullptr, nullptr, false, &ctx->provider);
            XAsyncComplete(data->async, hr, SUCCEEDED(hr) ? sizeof(XGameSaveProviderHandle) : 0);
            return S_OK;
        }
        case XAsyncOp::GetResult:
            if (data->bufferSize < sizeof(XGameSaveProviderHandle)) return E_INVALIDARG;
            std::memcpy(data->buffer, &ctx->provider, sizeof(XGameSaveProviderHandle));
            ctx->provider = nullptr;
            return S_OK;
        case XAsyncOp::Cleanup:
            if (ctx->provider) release_provider(ctx->provider);
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT __stdcall XGameSaveInitializeProviderAsync(XUserHandle /*requestingUser*/, const char* /*configurationId*/,
                                                   bool /*syncOnDemand*/, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    auto* ctx = new GsInitProviderCtx{};
    HRESULT hr = XAsyncBegin(async, ctx, &kGsInitProviderId, "XGameSaveInitializeProviderAsync", gs_init_provider_fn);
    if (FAILED(hr)) {
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XGameSaveInitializeProviderResult(XAsyncBlock* async, XGameSaveProviderHandle* provider) {
    if (!async || !provider) return E_INVALIDARG;
    return XAsyncGetResult(async, &kGsInitProviderId, sizeof(XGameSaveProviderHandle), provider, nullptr);
}

void __stdcall XGameSaveCloseProvider(XGameSaveProviderHandle provider) { release_provider(provider); }

HRESULT __stdcall XGameSaveGetRemainingQuota(XGameSaveProviderHandle provider, int64_t* remainingQuota) {
    if (!provider || !remainingQuota) return E_INVALIDARG;
    *remainingQuota = kOfflineQuotaBytes;
    return S_OK;
}

constexpr int kGsQuotaId = 0;
struct GsQuotaCtx {
    XGameSaveProviderHandle provider;
    int64_t quota{0};
};

HRESULT CALLBACK gs_quota_fn(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<GsQuotaCtx*>(data->context);
    switch (op) {
        case XAsyncOp::DoWork: {
            HRESULT hr = XGameSaveGetRemainingQuota(ctx->provider, &ctx->quota);
            XAsyncComplete(data->async, hr, SUCCEEDED(hr) ? sizeof(int64_t) : 0);
            return S_OK;
        }
        case XAsyncOp::GetResult:
            if (data->bufferSize < sizeof(int64_t)) return E_INVALIDARG;
            std::memcpy(data->buffer, &ctx->quota, sizeof(int64_t));
            return S_OK;
        case XAsyncOp::Cleanup:
            release_provider(ctx->provider);
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT __stdcall XGameSaveGetRemainingQuotaAsync(XGameSaveProviderHandle provider, XAsyncBlock* async) {
    if (!provider || !async) return E_INVALIDARG;
    retain_provider(provider);
    auto* ctx = new GsQuotaCtx{provider, 0};
    HRESULT hr = XAsyncBegin(async, ctx, &kGsQuotaId, "XGameSaveGetRemainingQuotaAsync", gs_quota_fn);
    if (FAILED(hr)) {
        release_provider(provider);
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XGameSaveGetRemainingQuotaResult(XAsyncBlock* async, int64_t* remainingQuota) {
    if (!async || !remainingQuota) return E_INVALIDARG;
    return XAsyncGetResult(async, &kGsQuotaId, sizeof(int64_t), remainingQuota, nullptr);
}

HRESULT __stdcall XGameSaveCreateContainer(XGameSaveProviderHandle provider, const char* containerName,
                                           XGameSaveContainerHandle* containerContext) {
    if (!provider || !containerContext) return E_INVALIDARG;
    if (!containerName || !containerName[0]) return E_GS_INVALID_CONTAINER_NAME;
    auto* c = new XGameSaveContainer{};
    retain_provider(provider);
    c->provider = provider;
    c->name = containerName;
    c->dir = provider->user_dir + L"\\" + utf8_to_wide(encode_name(containerName));
    *containerContext = c;
    return S_OK;
}

void __stdcall XGameSaveCloseContainer(XGameSaveContainerHandle context) { release_container(context); }

HRESULT __stdcall XGameSaveDeleteContainer(XGameSaveProviderHandle provider, const char* containerName) {
    if (!provider) return E_INVALIDARG;
    if (!containerName || !containerName[0]) return E_GS_INVALID_CONTAINER_NAME;
    std::lock_guard<std::mutex> lock(provider->io_mutex);
    const std::wstring dir = provider->user_dir + L"\\" + utf8_to_wide(encode_name(containerName));
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        RemoveDirectoryW(dir.c_str());
    }
    return S_OK;
}

constexpr int kGsDeleteContainerId = 0;
struct GsDeleteContainerCtx {
    XGameSaveProviderHandle provider;
    std::string container_name;
};

HRESULT CALLBACK gs_delete_container_fn(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<GsDeleteContainerCtx*>(data->context);
    switch (op) {
        case XAsyncOp::DoWork: {
            HRESULT hr = XGameSaveDeleteContainer(ctx->provider, ctx->container_name.c_str());
            XAsyncComplete(data->async, hr, 0);
            return S_OK;
        }
        case XAsyncOp::Cleanup:
            release_provider(ctx->provider);
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT __stdcall XGameSaveDeleteContainerAsync(XGameSaveProviderHandle provider, const char* containerName,
                                                XAsyncBlock* async) {
    if (!provider || !async) return E_INVALIDARG;
    if (!containerName || !containerName[0]) return E_GS_INVALID_CONTAINER_NAME;
    retain_provider(provider);
    auto* ctx = new GsDeleteContainerCtx{provider, containerName};
    HRESULT hr = XAsyncBegin(async, ctx, &kGsDeleteContainerId, "XGameSaveDeleteContainerAsync", gs_delete_container_fn);
    if (FAILED(hr)) {
        release_provider(provider);
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XGameSaveDeleteContainerResult(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    return XAsyncGetStatus(async, false);
}

void scan_container_stats(const std::wstring& dir, uint32_t* out_count, uint64_t* out_size, int64_t* out_mtime) {
    uint32_t count = 0;
    uint64_t total = 0;
    int64_t latest = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.cFileName[0] == L'~') continue;
            ++count;
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const int64_t mt = filetime_to_unix(fd.ftLastWriteTime);
            if (mt > latest) latest = mt;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    *out_count = count;
    *out_size = total;
    *out_mtime = latest;
}

HRESULT enumerate_containers_impl(XGameSaveProviderHandle provider, const char* filter, bool exact_match,
                                    void* context, XGameSaveContainerInfoCallback* callback) {
    if (!provider || !callback) return E_INVALIDARG;
    struct Entry {
        std::string name;
        uint32_t blob_count;
        uint64_t total_size;
        int64_t mtime;
    };
    std::vector<Entry> entries;
    {
        std::lock_guard<std::mutex> lock(provider->io_mutex);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((provider->user_dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            const size_t filter_len = filter ? std::strlen(filter) : 0;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (fd.cFileName[0] == L'~') continue;
                const std::string decoded = decode_name(wide_to_utf8(fd.cFileName));
                if (filter) {
                    if (exact_match) {
                        if (decoded != filter) continue;
                    } else if (filter_len > 0 && decoded.compare(0, filter_len, filter) != 0) {
                        continue;
                    }
                }
                Entry e{decoded, 0, 0, 0};
                scan_container_stats(provider->user_dir + L"\\" + fd.cFileName, &e.blob_count, &e.total_size, &e.mtime);
                entries.push_back(std::move(e));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (const auto& e : entries) {
        XGameSaveContainerInfo info{};
        info.name = e.name.c_str();
        info.displayName = e.name.c_str();
        info.blobCount = e.blob_count;
        info.totalSize = e.total_size;
        info.lastModifiedTime = e.mtime;
        info.needsSync = false;
        if (!callback(&info, context)) break;
    }
    return S_OK;
}

HRESULT __stdcall XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle provider, void* context,
                                                  XGameSaveContainerInfoCallback* callback) {
    return enumerate_containers_impl(provider, nullptr, false, context, callback);
}

HRESULT __stdcall XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle provider, const char* containerNamePrefix,
                                                        void* context, XGameSaveContainerInfoCallback* callback) {
    return enumerate_containers_impl(provider, containerNamePrefix, false, context, callback);
}

HRESULT __stdcall XGameSaveGetContainerInfo(XGameSaveProviderHandle provider, const char* containerName, void* context,
                                            XGameSaveContainerInfoCallback* callback) {
    if (!containerName || !containerName[0]) return E_GS_INVALID_CONTAINER_NAME;
    return enumerate_containers_impl(provider, containerName, true, context, callback);
}

HRESULT enumerate_blobs_impl(XGameSaveContainerHandle container, const char* prefix, void* context,
                               XGameSaveBlobInfoCallback* callback) {
    if (!container || !callback) return E_INVALIDARG;
    struct BlobEntry {
        std::string name;
        uint32_t size;
    };
    std::vector<BlobEntry> entries;
    {
        std::lock_guard<std::mutex> lock(container->provider->io_mutex);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((container->dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            const size_t prefix_len = prefix ? std::strlen(prefix) : 0;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (fd.cFileName[0] == L'~') continue;
                const std::string decoded = decode_name(wide_to_utf8(fd.cFileName));
                if (prefix_len > 0 && decoded.compare(0, prefix_len, prefix) != 0) continue;
                entries.push_back({decoded, fd.nFileSizeLow});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (const auto& e : entries) {
        XGameSaveBlobInfo info{e.name.c_str(), e.size};
        if (!callback(&info, context)) break;
    }
    return S_OK;
}

HRESULT __stdcall XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle container, void* context,
                                             XGameSaveBlobInfoCallback* callback) {
    return enumerate_blobs_impl(container, nullptr, context, callback);
}

HRESULT __stdcall XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle container, const char* blobNamePrefix,
                                                   void* context, XGameSaveBlobInfoCallback* callback) {
    return enumerate_blobs_impl(container, blobNamePrefix, context, callback);
}

struct LoadedBlob {
    std::string name;
    std::vector<uint8_t> data;
};

HRESULT load_blobs_from_disk(XGameSaveContainerHandle container, const char* const* blobNames, uint32_t count,
                             std::vector<LoadedBlob>& out, size_t* out_required_size) {
    out.clear();
    out.reserve(count);
    size_t required = count * sizeof(XGameSaveBlob);
    std::lock_guard<std::mutex> lock(container->provider->io_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (!blobNames[i] || !blobNames[i][0]) return E_INVALIDARG;
        const std::wstring path = container->dir + L"\\" + utf8_to_wide(encode_name(blobNames[i]));
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return E_GS_BLOB_NOT_FOUND;
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(f, &li) || li.QuadPart < 0 || li.QuadPart > 0x7FFFFFFFLL) {
            CloseHandle(f);
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
        LoadedBlob b;
        b.name = blobNames[i];
        b.data.resize(static_cast<size_t>(li.QuadPart));
        DWORD read_total = 0;
        while (read_total < b.data.size()) {
            DWORD chunk = 0;
            if (!ReadFile(f, b.data.data() + read_total, static_cast<DWORD>(b.data.size() - read_total), &chunk, nullptr)) {
                CloseHandle(f);
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (chunk == 0) break;
            read_total += chunk;
        }
        CloseHandle(f);
        b.data.resize(read_total);
        required += b.name.size() + 1 + b.data.size();
        out.push_back(std::move(b));
    }
    *out_required_size = required;
    return S_OK;
}

HRESULT pack_blobs_into_buffer(const std::vector<LoadedBlob>& blobs, size_t required_size, size_t blobsSize,
                               XGameSaveBlob* blobData) {
    if (blobsSize < required_size) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    if (blobs.empty()) return S_OK;
    if (!blobData) return E_INVALIDARG;
    auto* cursor = reinterpret_cast<uint8_t*>(blobData + blobs.size());
    for (size_t i = 0; i < blobs.size(); ++i) {
        const size_t name_bytes = blobs[i].name.size() + 1;
        std::memcpy(cursor, blobs[i].name.c_str(), name_bytes);
        blobData[i].info.name = reinterpret_cast<const char*>(cursor);
        cursor += name_bytes;

        blobData[i].info.size = static_cast<uint32_t>(blobs[i].data.size());
        if (!blobs[i].data.empty()) {
            std::memcpy(cursor, blobs[i].data.data(), blobs[i].data.size());
            blobData[i].data = cursor;
            cursor += blobs[i].data.size();
        } else {
            blobData[i].data = nullptr;
        }
    }
    return S_OK;
}

HRESULT __stdcall XGameSaveReadBlobData(XGameSaveContainerHandle container, const char** blobNames,
                                        uint32_t* countOfBlobs, size_t blobsSize, XGameSaveBlob* blobData) {
    if (!container || !countOfBlobs) return E_INVALIDARG;
    const uint32_t count = *countOfBlobs;
    if (count == 0) return S_OK;
    if (!blobNames) return E_INVALIDARG;
    std::vector<LoadedBlob> blobs;
    size_t required = 0;
    HRESULT hr = load_blobs_from_disk(container, blobNames, count, blobs, &required);
    if (FAILED(hr)) return hr;
    return pack_blobs_into_buffer(blobs, required, blobsSize, blobData);
}

constexpr int kGsReadBlobId = 0;
struct GsReadBlobCtx {
    XGameSaveContainerHandle container;
    std::vector<std::string> requested_names;
    std::vector<LoadedBlob> loaded;
    size_t required_size{0};
};

HRESULT CALLBACK gs_read_blob_fn(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<GsReadBlobCtx*>(data->context);
    switch (op) {
        case XAsyncOp::DoWork: {
            std::vector<const char*> ptrs;
            ptrs.reserve(ctx->requested_names.size());
            for (const auto& s : ctx->requested_names) ptrs.push_back(s.c_str());
            HRESULT hr = load_blobs_from_disk(ctx->container, ptrs.data(), static_cast<uint32_t>(ptrs.size()),
                                              ctx->loaded, &ctx->required_size);
            XAsyncComplete(data->async, hr, SUCCEEDED(hr) ? ctx->required_size : 0);
            return S_OK;
        }
        case XAsyncOp::GetResult:
            return pack_blobs_into_buffer(ctx->loaded, ctx->required_size, data->bufferSize,
                                          static_cast<XGameSaveBlob*>(data->buffer));
        case XAsyncOp::Cleanup:
            release_container(ctx->container);
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT __stdcall XGameSaveReadBlobDataAsync(XGameSaveContainerHandle container, const char** blobNames,
                                             uint32_t countOfBlobs, XAsyncBlock* async) {
    if (!container || !async) return E_INVALIDARG;
    if (countOfBlobs > 0 && !blobNames) return E_INVALIDARG;
    retain_container(container);
    auto* ctx = new GsReadBlobCtx{container, {}, {}, 0};
    ctx->requested_names.reserve(countOfBlobs);
    for (uint32_t i = 0; i < countOfBlobs; ++i) {
        if (!blobNames[i] || !blobNames[i][0]) {
            release_container(container);
            delete ctx;
            return E_INVALIDARG;
        }
        ctx->requested_names.emplace_back(blobNames[i]);
    }
    HRESULT hr = XAsyncBegin(async, ctx, &kGsReadBlobId, "XGameSaveReadBlobDataAsync", gs_read_blob_fn);
    if (FAILED(hr)) {
        release_container(container);
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XGameSaveReadBlobDataResult(XAsyncBlock* async, size_t blobsSize, XGameSaveBlob* blobData,
                                              uint32_t* countOfBlobs) {
    if (!async || !countOfBlobs) return E_INVALIDARG;
    size_t required = 0;
    HRESULT hr = XAsyncGetResultSize(async, &required);
    if (FAILED(hr)) return hr;
    if (blobsSize < required) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    if (required == 0) {
        *countOfBlobs = 0;
        return S_OK;
    }
    if (!blobData) return E_INVALIDARG;
    hr = XAsyncGetResult(async, &kGsReadBlobId, blobsSize, blobData, nullptr);
    if (FAILED(hr)) return hr;
    const auto* base = reinterpret_cast<const uint8_t*>(blobData);
    const auto* first_name = reinterpret_cast<const uint8_t*>(blobData[0].info.name);
    *countOfBlobs = static_cast<uint32_t>((first_name - base) / sizeof(XGameSaveBlob));
    return S_OK;
}

HRESULT __stdcall XGameSaveCreateUpdate(XGameSaveContainerHandle container, const char* /*containerDisplayName*/,
                                        XGameSaveUpdateHandle* updateContext) {
    if (!container || !updateContext) return E_INVALIDARG;
    auto* u = new XGameSaveUpdate{};
    retain_container(container);
    u->container = container;
    *updateContext = u;
    return S_OK;
}

void __stdcall XGameSaveCloseUpdate(XGameSaveUpdateHandle updateContext) { release_update(updateContext); }

HRESULT __stdcall XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle updateContext, const char* blobName,
                                           const uint8_t* data, size_t dataSize) {
    if (!updateContext || !blobName || !blobName[0]) return E_INVALIDARG;
    if (dataSize > 0 && !data) return E_INVALIDARG;
    if (updateContext->submitted) return E_GS_HANDLE_EXPIRED;
    PendingBlobOp op;
    op.is_delete = false;
    op.blob_name = blobName;
    if (dataSize > 0) op.data.assign(data, data + dataSize);
    updateContext->ops.push_back(std::move(op));
    return S_OK;
}

HRESULT __stdcall XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle updateContext, const char* blobName) {
    if (!updateContext || !blobName || !blobName[0]) return E_INVALIDARG;
    if (updateContext->submitted) return E_GS_HANDLE_EXPIRED;
    PendingBlobOp op;
    op.is_delete = true;
    op.blob_name = blobName;
    updateContext->ops.push_back(std::move(op));
    return S_OK;
}

HRESULT __stdcall XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) {
    if (!updateContext) return E_INVALIDARG;
    if (updateContext->submitted) return E_GS_HANDLE_EXPIRED;
    updateContext->submitted = true;

    XGameSaveContainerHandle c = updateContext->container;
    std::lock_guard<std::mutex> lock(c->provider->io_mutex);
    if (!create_directories_recursive(c->dir)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    for (const auto& op : updateContext->ops) {
        const std::string enc = encode_name(op.blob_name.c_str());
        const std::wstring dst = c->dir + L"\\" + utf8_to_wide(enc);
        if (op.is_delete) {
            if (!DeleteFileW(dst.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        } else {
            const std::wstring tmp = c->dir + L"\\~" + utf8_to_wide(enc);
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
            size_t written_total = 0;
            while (written_total < op.data.size()) {
                DWORD chunk = 0;
                const DWORD to_write = static_cast<DWORD>(op.data.size() - written_total);
                if (!WriteFile(f, op.data.data() + written_total, to_write, &chunk, nullptr)) {
                    const DWORD err = GetLastError();
                    CloseHandle(f);
                    DeleteFileW(tmp.c_str());
                    return HRESULT_FROM_WIN32(err);
                }
                written_total += chunk;
            }
            CloseHandle(f);
            if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const DWORD err = GetLastError();
                DeleteFileW(tmp.c_str());
                return HRESULT_FROM_WIN32(err);
            }
        }
    }
    return S_OK;
}

constexpr int kGsSubmitUpdateId = 0;
struct GsSubmitUpdateCtx {
    XGameSaveUpdateHandle update;
};

HRESULT CALLBACK gs_submit_update_fn(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<GsSubmitUpdateCtx*>(data->context);
    switch (op) {
        case XAsyncOp::DoWork: {
            HRESULT hr = XGameSaveSubmitUpdate(ctx->update);
            XAsyncComplete(data->async, hr, 0);
            return S_OK;
        }
        case XAsyncOp::Cleanup:
            release_update(ctx->update);
            delete ctx;
            return S_OK;
        default:
            return S_OK;
    }
}

HRESULT __stdcall XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle updateContext, XAsyncBlock* async) {
    if (!updateContext || !async) return E_INVALIDARG;
    retain_update(updateContext);
    auto* ctx = new GsSubmitUpdateCtx{updateContext};
    HRESULT hr = XAsyncBegin(async, ctx, &kGsSubmitUpdateId, "XGameSaveSubmitUpdateAsync", gs_submit_update_fn);
    if (FAILED(hr)) {
        release_update(updateContext);
        delete ctx;
        return hr;
    }
    return XAsyncSchedule(async, 0);
}

HRESULT __stdcall XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    return XAsyncGetStatus(async, false);
}

// ---- XPackage ----

constexpr size_t XPACKAGE_IDENTIFIER_MAX_LENGTH = 33;
constexpr char kDefaultPackageIdentifier[XPACKAGE_IDENTIFIER_MAX_LENGTH] = "WaveSpanPackageIdentifier0000000";
static_assert(sizeof(kDefaultPackageIdentifier) == XPACKAGE_IDENTIFIER_MAX_LENGTH,
              "default package identifier must match XPACKAGE_IDENTIFIER_MAX_LENGTH");

// HRESULT XPackageGetCurrentProcessPackageIdentifier(size_t bufferSize, char* buffer).
// Retrieves the package identifier string for the current process.
HRESULT __stdcall XPackageGetCurrentProcessPackageIdentifier(size_t bufferSize, char* buffer) {
    if (!buffer || bufferSize == 0) return E_INVALIDARG;

    char id[XPACKAGE_IDENTIFIER_MAX_LENGTH] = {};
    const DWORD n = GetEnvironmentVariableA("WAVESPAN_PACKAGE_ID", id, sizeof(id));
    const char* selected = (n > 0 && n < sizeof(id)) ? id : kDefaultPackageIdentifier;
    const size_t len = std::strlen(selected);

    if (bufferSize < len + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(buffer, selected, len + 1);
    return S_OK;
}

// HRESULT XPackageGetMountPath(XPackageMountHandle mount, size_t pathSize, char* path).
// Retrieves the installed filesystem path for the mounted package.
// Reads WAVESPAN_GAME_DIR when set, otherwise falls back to the current module's directory.
HRESULT __stdcall XPackageGetMountPath(void* /*mount*/, size_t pathSize, char* path) {
    if (!path || pathSize == 0) return E_INVALIDARG;

    char buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("WAVESPAN_GAME_DIR", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        const DWORD mod_len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
        if (mod_len > 0 && mod_len < sizeof(buf)) {
            char* slash = std::strrchr(buf, '\\');
            char* fslash = std::strrchr(buf, '/');
            char* last = slash > fslash ? slash : fslash;
            if (last) {
                *last = '\0';
            } else {
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            }
        } else {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
    }

    const size_t len = std::strlen(buf);
    if (pathSize < len + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(path, buf, len + 1);
    return S_OK;
}

// bool XPackageIsPackagedProcess().
// Returns true on Xbox Series console. WAVESPAN_PACKAGED=0 overrides it.
bool __stdcall XPackageIsPackagedProcess() {
    char s[16];
    const DWORD n = GetEnvironmentVariableA("WAVESPAN_PACKAGED", s, sizeof(s));
    if (n > 0 && n < sizeof(s)) {
        if (std::atoi(s) == 0) return false;
    }
    return true;
}

#define XGR(name) {"xgameruntime.dll", #name, reinterpret_cast<void*>(&name)}

}  // namespace

SHIM_TABLE(xgameruntime,
    // what xgameruntime.lib's thunks GetProcAddress (library_loader.cpp serves the LoadLibrary before it)
    XGR(InitializeApiImpl),
    XGR(InitializeApiImplEx),
    XGR(InitializeApiImplEx2),
    XGR(UninitializeApiImpl),
    XGR(QueryApiImpl),
    XGR(XGameRuntimeInitialize),
    XGR(XGameRuntimeUninitialize),
    XGR(XSystemGetDeviceType),
    XGR(XSystemGetConsoleId),
    XGR(XSystemGetAppMemoryUsageLimit),
    XGR(XSystemGetAppMemoryUsage),
    XGR(XSystemGetAnalyticsInfo),
    XGR(XSystemGetXboxLiveSandboxId),
    XGR(XSystemGetRuntimeInfo),
    XGR(XPackageGetCurrentProcessPackageIdentifier),
    XGR(XPackageGetMountPath),
    XGR(XPackageIsPackagedProcess),
    // XUser (offline local user)
    XGR(XUserAddAsync),
    XGR(XUserAddResult),
    XGR(XUserAddByIdWithUiAsync),
    XGR(XUserAddByIdWithUiResult),
    XGR(XUserDuplicateHandle),
    XGR(XUserCloseHandle),
    XGR(XUserGetId),
    XGR(XUserGetLocalId),
    XGR(XUserGetState),
    XGR(XUserGetGamertag),
    XGR(XUserCompare),
    // XGameSave (local folder-backed)
    XGR(XGameSaveInitializeProvider),
    XGR(XGameSaveInitializeProviderAsync),
    XGR(XGameSaveInitializeProviderResult),
    XGR(XGameSaveCloseProvider),
    XGR(XGameSaveGetRemainingQuota),
    XGR(XGameSaveGetRemainingQuotaAsync),
    XGR(XGameSaveGetRemainingQuotaResult),
    XGR(XGameSaveCreateContainer),
    XGR(XGameSaveCloseContainer),
    XGR(XGameSaveDeleteContainer),
    XGR(XGameSaveDeleteContainerAsync),
    XGR(XGameSaveDeleteContainerResult),
    XGR(XGameSaveEnumerateContainerInfo),
    XGR(XGameSaveEnumerateContainerInfoByName),
    XGR(XGameSaveGetContainerInfo),
    XGR(XGameSaveEnumerateBlobInfo),
    XGR(XGameSaveEnumerateBlobInfoByName),
    XGR(XGameSaveReadBlobData),
    XGR(XGameSaveReadBlobDataAsync),
    XGR(XGameSaveReadBlobDataResult),
    XGR(XGameSaveCreateUpdate),
    XGR(XGameSaveCloseUpdate),
    XGR(XGameSaveSubmitBlobWrite),
    XGR(XGameSaveSubmitBlobDelete),
    XGR(XGameSaveSubmitUpdate),
    XGR(XGameSaveSubmitUpdateAsync),
    XGR(XGameSaveSubmitUpdateResult),
    // XTaskQueue (third_party/xtaskqueue)
    XGR(XTaskQueueCreate),
    XGR(XTaskQueueCreateComposite),
    XGR(XTaskQueueGetPort),
    XGR(XTaskQueueDuplicateHandle),
    XGR(XTaskQueueCloseHandle),
    XGR(XTaskQueueTerminate),
    XGR(XTaskQueueDispatch),
    XGR(XTaskQueueSubmitCallback),
    XGR(XTaskQueueSubmitDelayedCallback),
    XGR(XTaskQueueRegisterWaiter),
    XGR(XTaskQueueUnregisterWaiter),
    XGR(XTaskQueueRegisterMonitor),
    XGR(XTaskQueueUnregisterMonitor),
    XGR(XTaskQueueGetCurrentProcessTaskQueue),
    XGR(XTaskQueueSetCurrentProcessTaskQueue),
    // XAsync (third_party/xtaskqueue)
    XGR(XAsyncGetStatus),
    XGR(XAsyncGetResultSize),
    XGR(XAsyncCancel),
    XGR(XAsyncRun),
    XGR(XAsyncBegin),
    XGR(XAsyncSchedule),
    XGR(XAsyncComplete),
    XGR(XAsyncGetResult),
);
