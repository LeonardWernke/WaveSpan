// XCRDAPI.dll: Xbox DirectStorage factory.
//
// XCRDAPI.dll exports DStorageGetFactory, the Xbox Series equivalent of the PC DirectStorage
// IDStorageFactory COM factory. The function takes an IID and an out-pointer, mirroring the PC API shape.
//
// Offline stub: DirectStorage relies on the console's high-bandwidth SSD controller; there is no equivalent
// on a PC that maps to the game's storage coordinates. Return E_NOTIMPL and log once. If the game falls back
// to ordinary file I/O (which the file_system shim already handles) this never needs to do more.
//
// ABI measured from the game_scarlett_master.exe import table (SHA-256 e95020b2…).
// Source: docs/runtime_imports.md, "XCRDAPI.dll".
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "shims.h"

namespace {

// DStorageGetFactory(riid, ppv) -> HRESULT
// Mirrors IDStorageFactory COM shape (pc dstorage.h). Returns E_NOTIMPL offline; ppv is zeroed.
HRESULT WINAPI DStorageGetFactory(REFIID /*riid*/, void** ppv) {
    if (ppv) *ppv = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr,
                     "[rt] xcrdapi: DStorageGetFactory stubbed -> E_NOTIMPL "
                     "(Xbox DirectStorage is not available offline)\n");
    return E_NOTIMPL;
}

}  // namespace

SHIM_TABLE(xcrdapi,
    {"XCRDAPI.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory)},
);
