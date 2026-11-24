"""Carve the zlib-compressed files out of an RPF9 archive without its (encrypted) table of contents.

The data area is mostly zlib streams laid end to end, separated by zero padding. This walks the archive, inflates
every stream that decodes cleanly to the end, and writes each one as <offset>.bin. It also records which byte
ranges it couldn't read, so coverage is measurable.

Usage:
    python -I rpf9_carve.py <archive.rpf> <out_dir> [--start N] [--limit BYTES] [--index-only]
"""
import argparse
import json
import mmap
import os
import re
import struct
import sys
import time
import zlib

# zlib stream start, or a Bink 2 video ('KB2' + revision letter, then u32 size-8).
# RPF9 magic as it sits on disk: the tag "RPF9" stored little-endian (docs/rpf.md, docs/bvh.md).
MAGIC = b"9FPR"
ZHDR = re.compile(rb"\x78[\x01\x5e\x9c\xda]|KB2[a-z]")
NONZERO = re.compile(rb"[^\x00]")
DECOMPRESS_MAX = 1 << 20


def classify_gap(m, start: int, end: int) -> tuple[int, int]:
    """Return ``(zero_bytes, unread_bytes)`` for ``m[start:end]`` without copying."""
    length = end - start
    if length <= 0:
        return 0, 0
    if NONZERO.search(m, start, end) is None:
        return length, 0
    return 0, length


def inflate_bounded(mv: memoryview, start: int, end: int, *,
                    max_keep: int | None = None,
                    chunk_size: int = DECOMPRESS_MAX) -> tuple[int, int, bytearray] | None:
    """Inflate a zlib stream in ``mv[start:end]`` using streaming ``bytearray`` accumulation.

    When ``max_keep`` is set, only the first ``max_keep`` inflated bytes are retained in the returned
    ``bytearray`` while still counting the full inflated size. Returns ``(stored, size, out)`` or
    ``None`` if the stream is invalid, truncated before ``end``, or inflates to fewer than 64 bytes.
    """
    d = zlib.decompressobj()
    out = bytearray()
    total = 0
    p = start
    pending = b""
    try:
        # Inflate only inside the scan window (all of the file without --limit): a stream that runs past
        # its end is left out, so readable + zero + unread add up to the span, as in story_index (#712).
        while not d.eof and (pending or p < end):
            if not pending:
                next_p = min(p + chunk_size, end)
                pending = mv[p:next_p]
                p = next_p
            data = d.decompress(pending, chunk_size)
            total += len(data)
            if max_keep is None:
                out.extend(data)
            elif len(out) < max_keep:
                out.extend(data[:max_keep - len(out)])
            pending = d.unconsumed_tail
        if d.eof:
            tail = d.flush()
            if tail:
                total += len(tail)
                if max_keep is None:
                    out.extend(tail)
                elif len(out) < max_keep:
                    out.extend(tail[:max_keep - len(out)])
    except zlib.error:
        return None
    if not d.eof or total < 64:
        return None
    used = p - start - len(d.unused_data)
    if used <= 0:
        return None
    return used, total, out


def below_normal():
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x00004000)


def offset(text):
    """--start: 0 or more, decimal or 0x hex."""
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("must be 0 or more") from None
    if value < 0:
        raise argparse.ArgumentTypeError("must be 0 or more")
    return value


def byte_count(text):
    """--limit: at least 1; 0 used to mean no limit and a negative value silently scanned less."""
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("must be at least 1") from None
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("out")
    ap.add_argument("--start", type=offset)
    ap.add_argument("--limit", type=byte_count)
    ap.add_argument("--index-only", action="store_true")
    a = ap.parse_args()
    below_normal()
    try:
        size = os.path.getsize(a.archive)
    except OSError as e:
        sys.exit(f"{a.archive}: {e.strerror or e}")
    if size == 0:
        sys.exit(f"{a.archive}: empty file")
    if size < 8:
        sys.exit(f"{a.archive}: too short for an RPF9 header ({size} bytes)")
    with open(a.archive, "rb") as fh:
        if fh.read(4) != MAGIC:
            sys.exit(f"{a.archive}: not an RPF9 archive (expected the magic {MAGIC.decode()} at offset 0)")
    if a.start is not None and not 0 <= a.start < size:
        sys.exit(f"{a.archive}: --start {a.start} is outside the file (0..{size - 1})")
    if a.start is None:
        with open(a.archive, "rb") as fh:
            base = struct.unpack_from("<I", fh.read(8), 4)[0]
        if base > size:   # base == size is an empty data area, which is fine
            sys.exit(f"{a.archive}: header data offset {base} is past the end of the file ({size} bytes)")
    else:
        base = a.start
    os.makedirs(a.out, exist_ok=True)
    with open(a.archive, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m, memoryview(m) as mv:
        pos = base
        end = min(size, pos + a.limit) if a.limit else size
        rows, last, unread, zero = [], pos, 0, 0
        t0 = time.time()
        keep_bytes = 16 if a.index_only else None
        while pos < end:
            mt = ZHDR.search(m, pos, end)
            if not mt:
                break
            s = mt.start()
            if mv[s:s + 3] == b"KB2":
                if s + 0x1C > end:   # header cut off by the end of the file or of --limit's window: not a video
                    pos = s + 1
                    continue
                blen = struct.unpack_from("<I", m, s + 4)[0] + 8
                w, h = struct.unpack_from("<II", m, s + 0x14)
                if not (64 <= blen <= end - s and 16 <= w <= 8192 and 16 <= h <= 8192):
                    pos = s + 1
                    continue
                gz, gu = classify_gap(m, last, s)
                zero += gz
                unread += gu
                rows.append({"off": s, "stored": blen, "size": blen, "type": "bink2", "w": w, "h": h,
                             "frames": struct.unpack_from("<I", m, s + 8)[0], "pre": "", "head": mv[s:s + 16].hex()})
                if not a.index_only:
                    with open(os.path.join(a.out, f"{s:012x}.bk2"), "wb") as f:
                        f.write(mv[s:s + blen])
                last = pos = s + blen
                continue
            inflated = inflate_bounded(mv, s, end, max_keep=keep_bytes)
            if inflated is None:
                pos = s + 1
                continue
            used, out_size, out = inflated
            gz, gu = classify_gap(m, last, s)
            zero += gz
            unread += gu
            rows.append({"off": s, "stored": used, "size": out_size, "pre": mv[max(0, s - 16):s].hex(),
                         "head": out[:16].hex()})
            if not a.index_only:
                with open(os.path.join(a.out, f"{s:012x}.bin"), "wb") as f:
                    f.write(out)
            last = pos = s + used
            if len(rows) % 2000 == 0:
                done = (pos - (a.start or 0)) / max(1, end - (a.start or 0))
                print(f"[{done:6.1%}] {len(rows)} files, {time.time() - t0:.0f}s", flush=True)
        tz, tu = classify_gap(m, last, end)
        zero += tz
        unread += tu
    total = sum(r["stored"] for r in rows)
    bink = sum(r["stored"] for r in rows if r.get("type") == "bink2")
    span = end - base
    summary = {"archive": a.archive, "files": len(rows), "compressed_bytes": total,
               "inflated_bytes": sum(r["size"] for r in rows), "bink2_bytes": bink,
               "bink2_files": sum(1 for r in rows if r.get("type") == "bink2"),
               "zero_padding": zero, "unread_bytes": unread,
               "span": span}
    with open(os.path.join(a.out, "index.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"summary": summary, "files": rows}, f)
    print(json.dumps(summary, indent=1))
    span = max(1, span)   # an empty data area (or --start at the end) has nothing to divide by
    print(f"readable: {total / span:.1%}, zero padding {zero / span:.1%}, unread {unread / span:.1%}")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
