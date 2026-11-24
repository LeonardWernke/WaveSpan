"""Synthetic CPU & memory benchmark for RPF9 scanning, bounded inflation, and fused name extraction.

Exercises ``classify_gap`` and ``inflate_bounded`` from ``rpf9_carve.py`` and ``scan_archive`` from
``rpf9_names.py`` on an in-memory/temp synthetic RPF9 archive (no game files). Reports median wall time,
``tracemalloc`` peak allocation, and process RSS/peak-working-set memory.

Usage:
    python -I tools/bench_rpf9.py [--reps R] [--warmup W] [--streams N] [--gap-kb K]
                                  [--csv] [--self-test] [--check-budget]
"""
import argparse
import ctypes
import mmap
import os
import statistics
import struct
import sys
import tempfile
import time
import tracemalloc
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rpf9_carve import classify_gap, inflate_bounded  # noqa: E402
import rpf9_names  # noqa: E402


def _process_rss_kb() -> int:
    """Return current/peak process RSS in KiB using stdlib only."""
    if os.name == "nt":
        class _PMC(ctypes.Structure):
            _fields_ = [
                ("cb", ctypes.c_ulong),
                ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        pmc = _PMC()
        pmc.cb = ctypes.sizeof(_PMC)
        k32 = ctypes.windll.kernel32
        k32.GetCurrentProcess.restype = ctypes.c_void_p
        proc = k32.GetCurrentProcess()
        fn = getattr(k32, "K32GetProcessMemoryInfo", None)
        if fn is None:
            fn = ctypes.windll.psapi.GetProcessMemoryInfo
        fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PMC), ctypes.c_ulong]
        if fn(proc, ctypes.byref(pmc), pmc.cb):
            return int(pmc.WorkingSetSize // 1024)
        return 0
    import resource
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return int(rss // 1024) if sys.platform == "darwin" else int(rss)


def _build_synthetic_rpf9(num_streams: int, payload_bytes: int = 32768) -> tuple[bytes, int]:
    """Build a synthetic RPF9 archive in memory; return ``(archive_bytes, expected_unique_names)``."""
    header_size = 64
    chunks = [bytearray(b"9FPR" + struct.pack("<I", header_size) + b"\x00" * (header_size - 8))]
    expected_names = set()
    for i in range(num_streams):
        name_a = f"prop_bench_{i:04d}.#dr".encode("ascii")
        name_b = f"tex_bench_{i:04d}.#dd".encode("ascii")
        expected_names.add(name_a.decode("ascii"))
        expected_names.add(name_b.decode("ascii"))
        body = bytearray(b" " * payload_bytes)
        body[128:128 + len(name_a)] = name_a
        mid = payload_bytes // 2
        body[mid:mid + len(name_b)] = name_b
        z = zlib.compress(bytes(body), level=1)
        chunks.append(z)
        # Zero padding + occasional small non-zero gap
        if i % 8 == 7:
            chunks.append(b"\xaa" * 64)
        chunks.append(b"\x00" * 512)
    return b"".join(chunks), len(expected_names)


def _bench_case(name: str, fn, reps: int, warmup: int) -> dict:
    for _ in range(warmup):
        fn()
    times_ms = []
    peak_bytes = 0
    last_result = None
    for _ in range(reps):
        tracemalloc.start()
        t0 = time.perf_counter()
        last_result = fn()
        t1 = time.perf_counter()
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        times_ms.append((t1 - t0) * 1000.0)
        if peak > peak_bytes:
            peak_bytes = peak
    return {
        "name": name,
        "median_ms": statistics.median(times_ms),
        "min_ms": min(times_ms),
        "max_ms": max(times_ms),
        "tracemalloc_peak_kb": peak_bytes / 1024.0,
        "rss_kb": _process_rss_kb(),
        "result": last_result,
    }


def run_benchmark(args) -> int:
    gap_bytes = args.gap_kb * 1024
    zero_buf = bytearray(gap_bytes)
    nonzero_buf = bytearray(gap_bytes)
    nonzero_buf[-1] = 0x5A

    # Prepare synthetic RPF9 archive on disk for mmap + rpf9_names.scan_archive
    archive_bytes, expected_names = _build_synthetic_rpf9(args.streams)
    # Prepare a large compressed stream for inflate_bounded
    big_payload = (b"A" * 16384 + b"prop_big_0001.#dr" + b"B" * 16384) * 8  # ~256 KiB
    big_compressed = zlib.compress(big_payload, level=1)

    fd, tmp_path = tempfile.mkstemp(prefix="bench_rpf9_", suffix=".rpf")
    try:
        os.write(fd, archive_bytes)
        os.close(fd)

        def case_classify_gap():
            z_total = 0
            u_total = 0
            for _ in range(8):
                gz, gu = classify_gap(zero_buf, 0, gap_bytes)
                z_total += gz
                u_total += gu
                gz, gu = classify_gap(nonzero_buf, 0, gap_bytes)
                z_total += gz
                u_total += gu
            return z_total, u_total

        def case_inflate_bounded_full():
            mv = memoryview(big_compressed)
            res = inflate_bounded(mv, 0, len(big_compressed), max_keep=None)
            return res[0], res[1], len(res[2])

        def case_inflate_bounded_prefix():
            mv = memoryview(big_compressed)
            res = inflate_bounded(mv, 0, len(big_compressed), max_keep=16384, chunk_size=65536)
            return res[0], res[1], len(res[2])

        def case_rpf9_names():
            rows, summary = rpf9_names.scan_archive(tmp_path, max_inflate=65536, types=("dr", "ft", "dd"))
            return len(rows), summary["streams"], summary["unique_names"]

        def case_classify_mmap():
            with open(tmp_path, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
                return classify_gap(m, 8, 64)

        results = [
            _bench_case("classify_gap", case_classify_gap, args.reps, args.warmup),
            _bench_case("classify_gap_mmap", case_classify_mmap, args.reps, args.warmup),
            _bench_case("inflate_bounded_full", case_inflate_bounded_full, args.reps, args.warmup),
            _bench_case("inflate_bounded_16k", case_inflate_bounded_prefix, args.reps, args.warmup),
            _bench_case("rpf9_names_fused", case_rpf9_names, args.reps, args.warmup),
        ]
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass

    if not args.self_test:
        if args.csv:
            print("case,median_ms,min_ms,max_ms,tracemalloc_peak_kb,rss_kb")
            for r in results:
                print(f"{r['name']},{r['median_ms']:.3f},{r['min_ms']:.3f},{r['max_ms']:.3f},"
                      f"{r['tracemalloc_peak_kb']:.2f},{r['rss_kb']}")
        else:
            print(f"{'case':<24} {'med_ms':>9} {'min_ms':>9} {'max_ms':>9} {'peak_kb':>10} {'rss_kb':>9}")
            for r in results:
                print(f"{r['name']:<24} {r['median_ms']:>9.3f} {r['min_ms']:>9.3f} {r['max_ms']:>9.3f} "
                      f"{r['tracemalloc_peak_kb']:>10.2f} {r['rss_kb']:>9}")

    failures = 0
    by_name = {r["name"]: r for r in results}

    # Correctness checks (always active for --self-test and --check-budget)
    if args.self_test or args.check_budget:
        z_tot, u_tot = by_name["classify_gap"]["result"]
        if z_tot != 8 * gap_bytes or u_tot != 8 * gap_bytes:
            print(f"FAIL classify_gap: got ({z_tot}, {u_tot}), expected ({8 * gap_bytes}, {8 * gap_bytes})",
                  file=sys.stderr)
            failures += 1

        mz, mu = by_name["classify_gap_mmap"]["result"]
        if (mz, mu) != (56, 0):
            print(f"FAIL classify_gap_mmap: got ({mz}, {mu}), expected (56, 0)", file=sys.stderr)
            failures += 1

        used_f, size_f, kept_f = by_name["inflate_bounded_full"]["result"]
        used_p, size_p, kept_p = by_name["inflate_bounded_16k"]["result"]
        if used_f != len(big_compressed) or size_f != len(big_payload) or kept_f != len(big_payload):
            print(f"FAIL inflate_bounded_full: got ({used_f}, {size_f}, {kept_f})", file=sys.stderr)
            failures += 1
        if used_p != len(big_compressed) or size_p != len(big_payload) or kept_p != 16384:
            print(f"FAIL inflate_bounded_16k: got ({used_p}, {size_p}, {kept_p})", file=sys.stderr)
            failures += 1

        row_count, stream_count, unique_count = by_name["rpf9_names_fused"]["result"]
        if row_count != args.streams or stream_count != args.streams or unique_count != expected_names:
            print(f"FAIL rpf9_names_fused: rows={row_count} streams={stream_count} unique={unique_count} "
                  f"(expected {args.streams}, {args.streams}, {expected_names})", file=sys.stderr)
            failures += 1

    # Memory / allocation budgets
    if args.check_budget:
        budgets_kb = {
            "classify_gap": 64.0,
            "classify_gap_mmap": 64.0,
            "inflate_bounded_16k": 512.0,
            "rpf9_names_fused": 2048.0,
        }
        for cname, max_kb in budgets_kb.items():
            peak = by_name[cname]["tracemalloc_peak_kb"]
            if peak > max_kb:
                print(f"BUDGET FAIL {cname}: tracemalloc_peak_kb={peak:.2f} > {max_kb:.2f}", file=sys.stderr)
                failures += 1

    if args.self_test:
        status = "FAILED" if failures else "ok"
        print(f"bench_rpf9 self-test: {len(results)} cases ({args.streams} streams), "
              f"rpf9_names_peak={by_name['rpf9_names_fused']['tracemalloc_peak_kb']:.1f} KiB, {status}")

    return 1 if failures else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Synthetic RPF9 scan/inflate/name-extraction benchmark")
    parser.add_argument("--reps", type=int, default=7, help="Timed repetitions per case")
    parser.add_argument("--warmup", type=int, default=2, help="Warmup repetitions per case")
    parser.add_argument("--streams", type=int, default=64, help="Synthetic zlib streams in test archive")
    parser.add_argument("--gap-kb", type=int, default=1024, help="Gap buffer size in KiB for classify_gap")
    parser.add_argument("--csv", action="store_true", help="Emit CSV output")
    parser.add_argument("--self-test", action="store_true", help="Run quick self-test")
    parser.add_argument("--check-budget", action="store_true", help="Verify allocation/correctness budgets")
    args = parser.parse_args(argv)
    if args.self_test:
        args.reps = min(args.reps, 3)
        args.warmup = min(args.warmup, 1)
        args.streams = min(args.streams, 16)
        args.gap_kb = min(args.gap_kb, 128)
    return run_benchmark(args)


if __name__ == "__main__":
    raise SystemExit(main())
