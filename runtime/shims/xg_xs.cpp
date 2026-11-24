// xg_xs.dll: Xbox Series texture-layout helpers.
//
// XGComputeTexture2DLayout and XGComputeOptimalSwizzleMode are the two functions this shim already serves
// through registry.cpp's unimplemented-thunk path (they are in d3d12_xs.dll's import namespace on PC).
// Two additional exports appear in game_scarlett_master.exe's import table and need stub implementations:
//
//   XGCreateTextureComputer   -- creates a COM-like texture-compute object
//   XGSuggestSwizzleMode      -- suggests a swizzle mode given a texture descriptor
//
// ABI measured from the game_scarlett_master.exe import table (SHA-256 e95020b2…).
// No public SDK headers declare these for Xbox Series; signatures are inferred from naming conventions and
// the PC DirectX Texture library equivalents.
//
// ASSUMPTION: XGCreateTextureComputer follows the COM factory shape (const desc*, void** out) -> HRESULT,
// consistent with XGComputeTexture2DLayout's shape. XGSuggestSwizzleMode follows the XG* texture query
// shape (const desc*) -> uint32_t swizzle mode index. Revisit when a measured call site shows the layout.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "shims.h"

namespace {

// XGCreateTextureComputer: creates a texture-layout computer object (COM-like, out via ppv).
// The game uses it to query tile shapes at texture-creation time. No equivalent exists on PC without the
// Xbox texture library. Offline stub: zero *ppv and return E_NOTIMPL. The game is expected to fall back
// to its offline path (pre-computed layouts embedded in the package).
HRESULT WINAPI XGCreateTextureComputer(const void* /*desc*/, void** ppv) {
    if (ppv) *ppv = nullptr;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr,
                     "[rt] xg_xs: XGCreateTextureComputer stubbed -> E_NOTIMPL "
                     "(no Xbox texture-compute object on PC)\n");
    return E_NOTIMPL;
}

// XGSuggestSwizzleMode: given a texture descriptor, returns the preferred hardware swizzle-mode index.
// A return of 0 is XG_SWIZZLE_MODE_LINEAR (or the equivalent "no swizzle" sentinel), which is safe for
// all formats and is what the PC path uses. The game handles this gracefully when running against unswizzled
// textures loaded by the file_system shim.
uint32_t WINAPI XGSuggestSwizzleMode(const void* /*desc*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr,
                     "[rt] xg_xs: XGSuggestSwizzleMode stubbed -> 0 (linear / no swizzle)\n");
    return 0;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode",    reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
