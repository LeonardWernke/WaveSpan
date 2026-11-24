"""Semantic tests for sub-dword MUBUF buffer loads:
    buffer_load_ushort
    buffer_load_short
    buffer_load_ubyte
    buffer_load_sbyte

Covers:
  - Signed vs unsigned sub-dword bit extraction (OpBitFieldUExtract / OpBitFieldSExtract).
  - Byte alignments within a 32-bit dword (offsets 0, 1, 2, 3 for bytes; 0, 2 for shorts).
  - Bounds checking with elem=2 (ushort/short) and elem=1 (ubyte/sbyte) across:
      * Mode 0: STRUCTURED_WITH_OFFSET (stride and index bounds, sub-dword edge conditions)
      * Mode 1: STRUCTURED (index bounds)
      * Mode 2: DISABLED (num_records == 0 bounds)
      * Mode 3: RAW (num_records byte bounds, sub-dword edge conditions)
      * Runtime/dynamic V# mode selection
  - Out-of-bounds returning 0 via OpPhi and not reading memory.
  - Inactive lane execution mask (EXEC) preservation of destination VGPR and no memory reads.
  - SPIR-V structure validation (OpBitFieldUExtract/SExtract, OpPhi, OpLoad Aligned 4).
"""
import os
import re
import struct
import sys
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, os.environ.get("WAVESPAN_TOOLS_DIR") or os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402
import support  # noqa: E402
import test_value_semantics as tv  # noqa: E402

BASE = tv.VEC_REGION
OUT_V = 0x31016FAC
NONE = None

# Sample data:
# BASE + 0: 0x80FF7F12 -> bytes [0x12, 0x7F, 0xFF, 0x80]
#   byte 0: 0x12 (18)
#   byte 1: 0x7F (127)
#   byte 2: 0xFF (255 / -1)
#   byte 3: 0x80 (128 / -128)
#   ushort 0: 0x7F12 (32530)
#   short 0:  0x7F12 (32530)
#   ushort 2: 0x80FF (33023)
#   short 2:  0xFFFF80FF (-32513)
# BASE + 4: 0xFFFF8001 -> bytes [0x01, 0x80, 0xFF, 0xFF]
#   ushort 0: 0x8001 (32769)
#   short 0:  0xFFFF8001 (-32767)
#   ushort 2: 0xFFFF (65535)
#   short 2:  0xFFFFFFFF (-1)
DATA_WORDS = {
    BASE + 0: 0x80FF7F12,
    BASE + 4: 0xFFFF8001,
    BASE + 8: 0x12345678,
    BASE + 12: 0x9ABCDEF0,
}


def operands(idx, off, soffset, inst):
    """vaddr operand text, modifiers and the setup instructions for one test case."""
    regs, mods = [], []
    if idx is not NONE:
        regs.append(idx)
        mods.append("idxen")
    if off is not NONE:
        regs.append(off)
        mods.append("offen")
    vaddr = {0: "off", 1: "v1", 2: "v[1:2]"}[len(regs)]
    setup = [f"v_mov_b32 v{1 + i}, {r:#x}" for i, r in enumerate(regs)]
    if soffset:
        setup.append(f"s_mov_b32 s8, {soffset:#x}")
    return vaddr, " ".join(mods + ([f"offset:{inst}"] if inst else [])), setup, ("s8" if soffset else "0")


def v_sharp(mode, stride, nr, runtime, base=BASE, fmt=22):
    """Emit instructions putting V# in s[4:7]; if runtime=True, the mode is or-ed at runtime."""
    w3 = 0x01000FAC | fmt << 12
    lines = [f"s_mov_b32 s4, {base:#x}", f"s_mov_b32 s5, {hex(stride << 16)}", f"s_mov_b32 s6, {nr:#x}"]
    if runtime:
        lines += [f"s_mov_b32 s7, {w3:#x}", f"s_mov_b32 s9, {mode << 28:#x}", "s_or_b32 s7, s7, s9"]
    else:
        lines += [f"s_mov_b32 s7, {w3 | mode << 28:#x}"]
    return lines


class SubDwordTestBase(unittest.TestCase):
    def translate(self, lines):
        return support.translate_lines("cs_subdword_t", ["v_cmpx_eq_u32_e32 0, v0", *lines, "s_endpgm"],
                                       fssm=tv.CS_BOUNDS_FSSM)

    def run_spv(self, spv, memory=None):
        if memory is None:
            memory = tv.ReadLog(dict(DATA_WORDS))
        elif not isinstance(memory, tv.ReadLog):
            memory = tv.ReadLog(memory)
        support.run_fragment(spv, inputs={"%wg_id": (0, 0, 0), "%local_id": (0, 0, 0)},
                             push_constants=[0, 0, 0x100, OUT_V] + [0] * 28, memory=memory)
        return memory


class SubDwordExtractionTests(SubDwordTestBase):
    """Test signed vs unsigned bitfield extraction and byte alignments."""

    def test_ubyte_extraction_all_offsets(self):
        # BASE holds 0x80FF7F12 -> bytes: 0x12, 0x7F, 0xFF, 0x80
        expected = [0x12, 0x7F, 0xFF, 0x80]
        for byte_off, want in enumerate(expected):
            with self.subTest(byte_off=byte_off):
                lines = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"buffer_load_ubyte v2, off, s[4:7], 0 offset:{byte_off}",
                    "s_waitcnt vmcnt(0)",
                    "buffer_store_dword v2, off, s[0:3], 0",
                ]
                mem = self.run_spv(self.translate(lines))
                self.assertEqual(dict.get(mem, 0, 0), want)

    def test_sbyte_extraction_all_offsets(self):
        # BASE holds 0x80FF7F12 -> bytes: 0x12, 0x7F, 0xFF, 0x80
        # byte 0: 0x12 -> 0x00000012
        # byte 1: 0x7F -> 0x0000007F
        # byte 2: 0xFF -> 0xFFFFFFFF (sign extended)
        # byte 3: 0x80 -> 0xFFFFFF80 (sign extended)
        expected = [0x12, 0x7F, 0xFFFFFFFF, 0xFFFFFF80]
        for byte_off, want in enumerate(expected):
            with self.subTest(byte_off=byte_off):
                lines = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"buffer_load_sbyte v2, off, s[4:7], 0 offset:{byte_off}",
                    "s_waitcnt vmcnt(0)",
                    "buffer_store_dword v2, off, s[0:3], 0",
                ]
                mem = self.run_spv(self.translate(lines))
                self.assertEqual(dict.get(mem, 0, 0), want)

    def test_ushort_extraction_all_offsets(self):
        # BASE + 0: [0x12, 0x7F, 0xFF, 0x80] -> offset 0: 0x7F12, offset 2: 0x80FF
        # BASE + 4: [0x01, 0x80, 0xFF, 0xFF] -> offset 4: 0x8001, offset 6: 0xFFFF
        cases = [
            (0, 0x7F12),
            (2, 0x80FF),
            (4, 0x8001),
            (6, 0xFFFF),
        ]
        for byte_off, want in cases:
            with self.subTest(byte_off=byte_off):
                lines = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"buffer_load_ushort v2, off, s[4:7], 0 offset:{byte_off}",
                    "s_waitcnt vmcnt(0)",
                    "buffer_store_dword v2, off, s[0:3], 0",
                ]
                mem = self.run_spv(self.translate(lines))
                self.assertEqual(dict.get(mem, 0, 0), want)

    def test_short_extraction_all_offsets(self):
        # BASE + 0: [0x12, 0x7F, 0xFF, 0x80]
        #   offset 0: 0x7F12 (positive) -> 0x00007F12
        #   offset 2: 0x80FF (negative) -> 0xFFFF80FF
        # BASE + 4: [0x01, 0x80, 0xFF, 0xFF]
        #   offset 4: 0x8001 (negative) -> 0xFFFF8001
        #   offset 6: 0xFFFF (negative) -> 0xFFFFFFFF
        cases = [
            (0, 0x7F12),
            (2, 0xFFFF80FF),
            (4, 0xFFFF8001),
            (6, 0xFFFFFFFF),
        ]
        for byte_off, want in cases:
            with self.subTest(byte_off=byte_off):
                lines = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"buffer_load_short v2, off, s[4:7], 0 offset:{byte_off}",
                    "s_waitcnt vmcnt(0)",
                    "buffer_store_dword v2, off, s[0:3], 0",
                ]
                mem = self.run_spv(self.translate(lines))
                self.assertEqual(dict.get(mem, 0, 0), want)

    def test_addressing_modes_offen_idxen_soffset(self):
        # Test vaddr with offen, idxen, soffset combination
        # idx = 1, stride = 4, off = 2, soffset = 2 -> total addr = 4 + 2 + 2 = 8
        # DATA_WORDS[BASE + 8] = 0x12345678 -> bytes: [0x78, 0x56, 0x34, 0x12]
        lines = [
            *v_sharp(3, 4, 0x100, runtime=False),
            "v_mov_b32 v1, 1",
            "v_mov_b32 v2, 2",
            "s_mov_b32 s8, 2",
            "buffer_load_ushort v4, v[1:2], s[4:7], s8 idxen offen",
            "s_waitcnt vmcnt(0)",
            "buffer_store_dword v4, off, s[0:3], 0",
        ]
        mem = self.run_spv(self.translate(lines))
        # bytes [0, 1] of 0x12345678 are 0x5678
        self.assertEqual(dict.get(mem, 0, 0), 0x5678)


class SubDwordStructuredBoundsTests(SubDwordTestBase):
    """Test Mode 0 and Mode 1 structured bounds checking with elem=1 and elem=2."""

    def test_mode0_subdword_stride_edges(self):
        # Stride = 8, NUM_RECORDS = 4
        # off + elem <= stride 8:
        # For elem = 2 (ushort/short):
        #   off = 6: 6 + 2 = 8 <= 8 (in bounds)
        #   off = 7: 7 + 2 = 9 > 8 (out of bounds)
        # For elem = 1 (ubyte/sbyte):
        #   off = 7: 7 + 1 = 8 <= 8 (in bounds)
        #   off = 8: 8 + 1 = 9 > 8 (out of bounds)
        for runtime in (False, True):
            # ushort at off=6 is in-bounds
            lines = [
                *v_sharp(0, 8, 4, runtime),
                "v_mov_b32 v1, 0",  # idx = 0
                "v_mov_b32 v2, 6",  # off = 6
                "buffer_load_ushort v3, v[1:2], s[4:7], 0 idxen offen",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v3, off, s[0:3], 0",
            ]
            mem = self.run_spv(self.translate(lines))
            self.assertEqual(dict.get(mem, 0, 0), 0xFFFF, (runtime, "ushort off=6 in bounds"))
            self.assertIn(BASE + 4, mem.reads)

            # ushort at off=7 is out-of-bounds (7 + 2 > 8)
            lines = [
                *v_sharp(0, 8, 4, runtime),
                "v_mov_b32 v1, 0",  # idx = 0
                "v_mov_b32 v2, 7",  # off = 7
                "buffer_load_ushort v3, v[1:2], s[4:7], 0 idxen offen",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v3, off, s[0:3], 0",
            ]
            mem = self.run_spv(self.translate(lines))
            self.assertEqual(dict.get(mem, 0, 0), 0, (runtime, "ushort off=7 out of bounds"))
            self.assertEqual(mem.reads, [])

            # ubyte at off=7 is in-bounds (7 + 1 <= 8)
            lines = [
                *v_sharp(0, 8, 4, runtime),
                "v_mov_b32 v1, 0",  # idx = 0
                "v_mov_b32 v2, 7",  # off = 7
                "buffer_load_ubyte v3, v[1:2], s[4:7], 0 idxen offen",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v3, off, s[0:3], 0",
            ]
            mem = self.run_spv(self.translate(lines))
            self.assertEqual(dict.get(mem, 0, 0), 0xFF, (runtime, "ubyte off=7 in bounds"))
            self.assertIn(BASE + 4, mem.reads)

            # ubyte at off=8 is out-of-bounds (8 + 1 > 8)
            lines = [
                *v_sharp(0, 8, 4, runtime),
                "v_mov_b32 v1, 0",  # idx = 0
                "v_mov_b32 v2, 8",  # off = 8
                "buffer_load_ubyte v3, v[1:2], s[4:7], 0 idxen offen",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v3, off, s[0:3], 0",
            ]
            mem = self.run_spv(self.translate(lines))
            self.assertEqual(dict.get(mem, 0, 0), 0, (runtime, "ubyte off=8 out of bounds"))
            self.assertEqual(mem.reads, [])

    def test_mode0_index_bounds(self):
        # Stride = 8, NUM_RECORDS = 2
        # idx 1 < 2 is in bounds, idx 2 >= 2 is out of bounds
        for runtime in (False, True):
            for op, elem, val in (
                ("buffer_load_ushort", 2, 0x8001),
                ("buffer_load_short", 2, 0xFFFF8001),
                ("buffer_load_ubyte", 1, 0x01),
                ("buffer_load_sbyte", 1, 0x01),
            ):
                with self.subTest(op=op, runtime=runtime):
                    # idx 1: in bounds
                    lines_in = [
                        *v_sharp(0, 8, 2, runtime),
                        "v_mov_b32 v1, 1",  # idx = 1 -> addr BASE + 8 + 4 = BASE + 12
                        "v_mov_b32 v2, 4",  # off = 4
                        f"{op} v3, v[1:2], s[4:7], 0 idxen offen",
                        "s_waitcnt vmcnt(0)",
                        "buffer_store_dword v3, off, s[0:3], 0",
                    ]
                    mem_in = self.run_spv(self.translate(lines_in))
                    # At BASE + 12: DATA_WORDS[BASE + 12] = 0x9ABCDEF0 -> [0xF0, 0xDE, 0xBC, 0x9A]
                    # Since addr is BASE + 8 + 4 = BASE + 12:
                    # ushort is 0xDEF0, short is 0xFFFFDEF0, ubyte is 0xF0, sbyte is 0xFFFFFFF0
                    want_in = {
                        "buffer_load_ushort": 0xDEF0,
                        "buffer_load_short": 0xFFFFDEF0,
                        "buffer_load_ubyte": 0xF0,
                        "buffer_load_sbyte": 0xFFFFFFF0,
                    }[op]
                    self.assertIn(BASE + 12, mem_in.reads)
                    self.assertEqual(dict.get(mem_in, 0, 0), want_in)

                    # idx 2: out of bounds (idx >= NUM_RECORDS)
                    lines_out = [
                        *v_sharp(0, 8, 2, runtime),
                        "v_mov_b32 v1, 2",  # idx = 2
                        "v_mov_b32 v2, 4",  # off = 4
                        f"{op} v3, v[1:2], s[4:7], 0 idxen offen",
                        "s_waitcnt vmcnt(0)",
                        "buffer_store_dword v3, off, s[0:3], 0",
                    ]
                    mem_out = self.run_spv(self.translate(lines_out))
                    self.assertEqual(mem_out.reads, [])
                    self.assertEqual(dict.get(mem_out, 0, 0), 0)

    def test_mode1_structured_only_checks_index(self):
        # Mode 1: idx < NUM_RECORDS; off past stride is NOT checked
        for runtime in (False, True):
            lines = [
                *v_sharp(1, 4, 2, runtime),
                "v_mov_b32 v1, 0",   # idx = 0 < 2
                "v_mov_b32 v2, 12",  # off = 12 (far past stride 4)
                "buffer_load_ushort v3, v[1:2], s[4:7], 0 idxen offen",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v3, off, s[0:3], 0",
            ]
            mem = self.run_spv(self.translate(lines))
            # BASE + 12 has 0x9ABCDEF0 -> ushort at 0 is 0xDEF0
            self.assertEqual(dict.get(mem, 0, 0), 0xDEF0)

            # idx = 2 >= 2 -> out of bounds
            lines_oob = [
                *v_sharp(1, 4, 2, runtime),
                "v_mov_b32 v1, 2",
                "v_mov_b32 v2, 0",
                "buffer_load_ushort v3, v[1:2], s[4:7], 0 idxen offen",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v3, off, s[0:3], 0",
            ]
            mem_oob = self.run_spv(self.translate(lines_oob))
            self.assertEqual(mem_oob.reads, [])
            self.assertEqual(dict.get(mem_oob, 0, 0), 0)


class SubDwordRawBoundsTests(SubDwordTestBase):
    """Test Mode 3 raw buffer bounds checking: offs + elem <= NUM_RECORDS."""

    def test_mode3_subdword_edges(self):
        # NUM_RECORDS = 16 bytes (BASE .. BASE + 15)
        # offs + elem <= 16:
        # offs = 14:
        #   ushort/short: 14 + 2 = 16 <= 16 (in bounds!)
        #   dword: 14 + 4 = 18 > 16 (out of bounds!)
        # offs = 15:
        #   ubyte/sbyte: 15 + 1 = 16 <= 16 (in bounds!)
        #   ushort/short: 15 + 2 = 17 > 16 (out of bounds!)
        # offs = 16:
        #   ubyte/sbyte: 16 + 1 = 17 > 16 (out of bounds!)
        for runtime in (False, True):
            # offs = 14, ushort (in bounds)
            # BASE + 12 has 0x9ABCDEF0 -> bytes at 12, 13, 14, 15 are [0xF0, 0xDE, 0xBC, 0x9A]
            # byte 14 is 0xBC, byte 15 is 0x9A -> ushort is 0x9ABC
            lines_14_u16 = [
                *v_sharp(3, 0, 16, runtime),
                "buffer_load_ushort v2, off, s[4:7], 0 offset:14",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_14_u16 = self.run_spv(self.translate(lines_14_u16))
            self.assertIn(BASE + 12, mem_14_u16.reads)
            self.assertEqual(dict.get(mem_14_u16, 0, 0), 0x9ABC, (runtime, "ushort offs=14 in bounds"))

            # offs = 14, short (in bounds, 0x9ABC is negative -> sign-extended to 0xFFFF9ABC)
            lines_14_s16 = [
                *v_sharp(3, 0, 16, runtime),
                "buffer_load_short v2, off, s[4:7], 0 offset:14",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_14_s16 = self.run_spv(self.translate(lines_14_s16))
            self.assertIn(BASE + 12, mem_14_s16.reads)
            self.assertEqual(dict.get(mem_14_s16, 0, 0), 0xFFFF9ABC, (runtime, "short offs=14 in bounds"))

            # offs = 15, ushort (out of bounds: 15 + 2 > 16)
            lines_15_u16 = [
                *v_sharp(3, 0, 16, runtime),
                "buffer_load_ushort v2, off, s[4:7], 0 offset:15",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_15_u16 = self.run_spv(self.translate(lines_15_u16))
            self.assertEqual(mem_15_u16.reads, [])
            self.assertEqual(dict.get(mem_15_u16, 0, 0), 0, (runtime, "ushort offs=15 out of bounds"))

            # offs = 15, ubyte (in bounds: 15 + 1 <= 16, byte 15 is 0x9A)
            lines_15_u8 = [
                *v_sharp(3, 0, 16, runtime),
                "buffer_load_ubyte v2, off, s[4:7], 0 offset:15",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_15_u8 = self.run_spv(self.translate(lines_15_u8))
            self.assertIn(BASE + 12, mem_15_u8.reads)
            self.assertEqual(dict.get(mem_15_u8, 0, 0), 0x9A, (runtime, "ubyte offs=15 in bounds"))

            # offs = 15, sbyte (in bounds: 15 + 1 <= 16, 0x9A -> sign-extended to 0xFFFFFF9A)
            lines_15_s8 = [
                *v_sharp(3, 0, 16, runtime),
                "buffer_load_sbyte v2, off, s[4:7], 0 offset:15",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_15_s8 = self.run_spv(self.translate(lines_15_s8))
            self.assertIn(BASE + 12, mem_15_s8.reads)
            self.assertEqual(dict.get(mem_15_s8, 0, 0), 0xFFFFFF9A, (runtime, "sbyte offs=15 in bounds"))

            # offs = 16, ubyte (out of bounds: 16 + 1 > 16)
            lines_16_u8 = [
                *v_sharp(3, 0, 16, runtime),
                "buffer_load_ubyte v2, off, s[4:7], 0 offset:16",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_16_u8 = self.run_spv(self.translate(lines_16_u8))
            self.assertEqual(mem_16_u8.reads, [])
            self.assertEqual(dict.get(mem_16_u8, 0, 0), 0, (runtime, "ubyte offs=16 out of bounds"))

    def test_mode2_disabled_bounds(self):
        # Mode 2: only NUM_RECORDS == 0 is out of bounds
        for runtime in (False, True):
            # NR = 16 -> in bounds
            lines_in = [
                *v_sharp(2, 0, 16, runtime),
                "buffer_load_ushort v2, off, s[4:7], 0",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_in = self.run_spv(self.translate(lines_in))
            self.assertEqual(dict.get(mem_in, 0, 0), 0x7F12)

            # NR = 0 -> out of bounds
            lines_out = [
                *v_sharp(2, 0, 0, runtime),
                "buffer_load_ushort v2, off, s[4:7], 0",
                "s_waitcnt vmcnt(0)",
                "buffer_store_dword v2, off, s[0:3], 0",
            ]
            mem_out = self.run_spv(self.translate(lines_out))
            self.assertEqual(mem_out.reads, [])
            self.assertEqual(dict.get(mem_out, 0, 0), 0)


class SubDwordInactiveLaneTests(SubDwordTestBase):
    """Test inactive lane preservation and masking behavior."""

    SENTINEL = 0xDEADBEEF

    def test_single_lane_inactive_preserves_destination(self):
        # When lane 0 is inactive under EXEC mask (exec_lo = 0), buffer_load_* must NOT
        # overwrite the pre-existing destination register value, and must NOT read memory.
        for op in ("buffer_load_ushort", "buffer_load_short", "buffer_load_ubyte", "buffer_load_sbyte"):
            with self.subTest(op=op):
                lines = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"v_mov_b32 v2, {self.SENTINEL:#x}",
                    "s_mov_b32 exec_lo, 0",  # lane 0 becomes inactive
                    f"{op} v2, off, s[4:7], 0",
                    "s_mov_b32 exec_lo, 1",  # re-enable lane 0 so we can store v2 out
                    "buffer_store_dword v2, off, s[0:3], 0",
                ]
                mem = self.run_spv(self.translate(lines))
                self.assertEqual(mem.reads, [], f"{op} issued load in inactive lane")
                self.assertEqual(dict.get(mem, 0, 0), self.SENTINEL, f"{op} clobbered inactive destination")

    def test_multi_lane_workgroup_inactive_preservation(self):
        # Two lanes run: lane 0 active, lane 1 inactive (v_cmpx_eq_u32_e32 0, v0).
        # Lane 0 should load data; lane 1 should keep SENTINEL.
        for op, want_val in (
            ("buffer_load_ushort", 0x7F12),
            ("buffer_load_short", 0x7F12),
            ("buffer_load_ubyte", 0x12),
            ("buffer_load_sbyte", 0x12),
        ):
            with self.subTest(op=op):
                body = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"v_mov_b32 v2, {self.SENTINEL:#x}",
                    "v_cmpx_eq_u32_e32 0, v0",  # only lane 0 remains active in EXEC
                    f"{op} v2, off, s[4:7], 0",
                    "s_mov_b32 exec_lo, 3",  # re-enable lanes 0 and 1
                    "v_lshlrev_b32_e32 v3, 2, v0",  # v3 = lane * 4
                    "buffer_store_dword v2, v3, s[0:3], 0 offen",
                    "s_endpgm",
                ]
                spv = support.translate_lines("cs_inactive_wg", body, fssm=tv.CS_BOUNDS_FSSM)
                mem = tv.ReadLog(dict(DATA_WORDS))
                si.run_workgroup(spv, 2, 32,
                                 inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
                                 push_constants=[0, 0, 0x100, OUT_V] + [0] * 28, memory=mem)
                self.assertEqual(dict.get(mem, 0, 0), want_val, f"lane 0 active load failed for {op}")
                self.assertEqual(dict.get(mem, 4, 0), self.SENTINEL, f"lane 1 inactive destination clobbered for {op}")


class SubDwordSpirvStructureTests(unittest.TestCase):
    """Verify that generated SPIR-V matches expected OpBitField instructions, OpPhi and alignment."""

    def test_spirv_opcodes(self):
        cases = [
            ("buffer_load_ushort", "OpBitFieldUExtract", "%c16"),
            ("buffer_load_short", "OpBitFieldSExtract", "%c16"),
            ("buffer_load_ubyte", "OpBitFieldUExtract", "%c8"),
            ("buffer_load_sbyte", "OpBitFieldSExtract", "%c8"),
        ]
        for op, extract_op, count_const in cases:
            with self.subTest(op=op):
                lines = [
                    *v_sharp(3, 0, 0x100, runtime=False),
                    f"{op} v2, off, s[4:7], 0",
                    "s_endpgm",
                ]
                spv = support.translate_lines("cs_struct_test", lines, fssm=tv.CS_BOUNDS_FSSM)
                # Check for the correct extraction opcode
                self.assertIn(extract_op, spv)
                self.assertIn(count_const, spv)
                # Check for aligned 4-byte load
                self.assertIn("OpLoad %uint", spv)
                self.assertIn("Aligned 4", spv)
                # Check for OpPhi yielding 0
                self.assertIn("OpPhi %uint", spv)
                self.assertIn("%c0", spv)


if __name__ == "__main__":
    unittest.main()
