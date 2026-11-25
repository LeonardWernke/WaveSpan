#include "../src/crash_dump.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

int main() {
    std::error_code ec;
    const auto temp_dir =
        std::filesystem::temp_directory_path() /
        ("wavespan_crash_dump_test_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(temp_dir, ec);

    rt::crash_dump::install(temp_dir);

    // Folder should not be created until a crash actually happens.
    if (std::filesystem::exists(temp_dir, ec)) {
        std::fprintf(stderr, "FAIL: error dir created prematurely\n");
        return 1;
    }

    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er{};
    er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    er.NumberParameters = 2;
    er.ExceptionInformation[0] = 0;
    er.ExceptionInformation[1] = 0x12345678ULL;
    EXCEPTION_POINTERS info{&er, &ctx};

    std::filesystem::path dmp1, log1, dmp2, log2;
    if (!rt::crash_dump::write_report(&info, &dmp1, &log1)) {
        std::fprintf(stderr, "FAIL: first write_report failed\n");
        return 1;
    }
    if (!rt::crash_dump::write_report(&info, &dmp2, &log2)) {
        std::fprintf(stderr, "FAIL: second write_report failed\n");
        return 1;
    }

    if (dmp1 == dmp2 || log1 == log2) {
        std::fprintf(stderr, "FAIL: sequential crash reports collided\n");
        return 1;
    }
    if (!std::filesystem::exists(dmp1, ec) || std::filesystem::file_size(dmp1, ec) == 0) {
        std::fprintf(stderr, "FAIL: missing or empty dmp1\n");
        return 1;
    }
    if (!std::filesystem::exists(log1, ec) || std::filesystem::file_size(log1, ec) == 0) {
        std::fprintf(stderr, "FAIL: missing or empty log1\n");
        return 1;
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::printf("crash_dump_test: PASS\n");
    return 0;
}
