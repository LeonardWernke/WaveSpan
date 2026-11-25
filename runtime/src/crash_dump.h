#pragma once

#include <windows.h>

#include <filesystem>

namespace rt::crash_dump {

// Resolve the crash output directory:
// 1. WAVESPAN_CRASH_DIR environment variable, if set and non-empty.
// 2. Fallback: overlay_dir / "errors".
std::filesystem::path default_error_dir(const std::filesystem::path& overlay_dir);

// Configure the output folder and install the unhandled-exception filter.
// The output folder is only created if a crash is actually written.
void install(const std::filesystem::path& error_dir, bool trace_startup_faults = false);

// Write a unique .dmp and .log pair for `info` into the configured error folder.
// Returns true if at least the log or dump was written.
bool write_report(EXCEPTION_POINTERS* info,
                  std::filesystem::path* out_dmp = nullptr,
                  std::filesystem::path* out_log = nullptr);

}  // namespace rt::crash_dump
