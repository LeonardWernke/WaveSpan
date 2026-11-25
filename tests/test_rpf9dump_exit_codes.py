"""Exit codes of a built rpf9dump. Set RPF9DUMP_DLL to its Release DLL to run it."""

import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib


def awc_rpf(path):
    # "9FPR" archive holding one AWC bank: one nonstreamed XMA2 (codec 5) mono stream, id 0x1234, 64 data bytes.
    hlen = 16 + 4 + 2 * 8
    tag = lambda t, size, rel: struct.pack("<Q", (t << 56) | (size << 28) | rel)
    fmt = bytearray(32)
    struct.pack_into("<I", fmt, 0, 1000)
    struct.pack_into("<H", fmt, 8, 48000)
    fmt[28], fmt[29] = 5, 1
    bank = (b"ADAT" + struct.pack("<IIII", 0xFF000001, 1, hlen, (2 << 29) | 0x1234)
            + tag(0x76, 32, hlen) + tag(0x55, 64, hlen + 32) + bytes(fmt) + b"\x11" * 64)
    path.write_bytes(b"9FPR" + bytes(0x1000 - 4) + bank + bytes(0x1000))


def _recovered_rpf(path, system_bytes, graphics_bytes):
    sys_z = zlib.compress(system_bytes)
    gfx_z = zlib.compress(graphics_bytes)
    pad = (-len(sys_z)) & 15
    payload = bytes(16) + sys_z + bytes(pad) + gfx_z
    path.write_bytes(b"9FPR" + bytes(0x1000 - 4) + payload + bytes(0x1000))


def tex_rpf(path):
    sys = bytearray(256)
    struct.pack_into("<Q", sys, 0x08, 0x50000040)
    sys[0x48], sys[0x49] = 1, 1
    struct.pack_into("<Q", sys, 0x20, 0x50000050)
    struct.pack_into("<H", sys, 0x28, 1)
    struct.pack_into("<Q", sys, 0x30, 0x50000058)
    struct.pack_into("<HH", sys, 0x38, 1, 1)
    struct.pack_into("<I", sys, 0x50, 0x12345678)
    struct.pack_into("<Q", sys, 0x58, 0x50000060)
    t = 0x60
    struct.pack_into("<I", sys, t + 0x08, 512)
    struct.pack_into("<H", sys, t + 0x0C, 8)
    struct.pack_into("<Q", sys, t + 0x18, 0x60000000)
    struct.pack_into("<Q", sys, t + 0x28, 0x50000000 + t + 0x50)
    struct.pack_into("<Q", sys, t + 0x30, 0x500000D0)
    struct.pack_into("<HHH", sys, t + 0x38, 4, 4, 1)
    sys[t + 0x3E], sys[t + 0x3F], sys[t + 0x40], sys[t + 0x42] = 1, 71, 5, 1
    sys[0xD8:0xDD] = b"tex0\x00"
    _recovered_rpf(path, bytes(sys), bytes(4096))


def model_rpf(path):
    # Minimal standalone drawable: 1 LOD model, 1 geometry, 3 float3 vertices, 1 triangle.
    sys = bytearray(1024)
    # Drawable header (0x000..0x0B0)
    struct.pack_into("<Q", sys, 0x08, 0x500000B0)   # pageInfo
    struct.pack_into("<Q", sys, 0x10, 0x500000C0)   # shaderGroup
    struct.pack_into("<f", sys, 0x2C, 2.0)          # radius
    struct.pack_into("<fff", sys, 0x30, 0.0, 0.0, 0.0)
    struct.pack_into("<fff", sys, 0x40, 1.0, 1.0, 0.0)
    struct.pack_into("<Q", sys, 0x50, 0x50000100)   # high LOD
    struct.pack_into("<Q", sys, 0xA8, 0x500003F0)   # name
    sys[0xB8], sys[0xB9] = 1, 1                     # systemPages, graphicsPages
    # High LOD (0x100) -> model list (0x110) -> model (0x120)
    struct.pack_into("<QHH", sys, 0x100, 0x50000110, 1, 1)
    struct.pack_into("<Q", sys, 0x110, 0x50000120)
    # Model (0x120)
    struct.pack_into("<Q", sys, 0x128, 0x50000160)  # geometries ptr
    struct.pack_into("<HHH", sys, 0x130, 1, 1, 0)   # count, capacity, boneCount
    struct.pack_into("<Q", sys, 0x138, 0x50000170)  # bounds
    struct.pack_into("<H", sys, 0x156, 1)           # count2
    struct.pack_into("<Q", sys, 0x160, 0x50000190)  # geometry[0]
    struct.pack_into("<fff", sys, 0x170, 0.0, 0.0, 0.0)
    struct.pack_into("<fff", sys, 0x180, 1.0, 1.0, 0.0)
    # Geometry (0x190): vb at +8, descriptor at +64
    struct.pack_into("<Q", sys, 0x198, 0x50000230)  # vb
    struct.pack_into("<Q", sys, 0x1D0, 0x50000280)  # descriptor
    # VertexBuffer (0x230): count=132 stride=1 at +8/+12, gfx=0 at +24, fvf=0x290 at +64
    struct.pack_into("<IH", sys, 0x238, 132, 1)
    struct.pack_into("<Q", sys, 0x248, 0x60000000)
    struct.pack_into("<Q", sys, 0x270, 0x50000290)
    # IndexDescriptor (0x280): ib=0x240? No, place ib at 0x3D0? Wait: 0x290+320=0x3D0, name at 0x3F0.
    # Let's put ib at 0x1F0 (unused space between 0x1D8 and 0x230 is 0x58=88 bytes, >= 64!).
    struct.pack_into("<QHH", sys, 0x280, 0x500001F0, 1, 1)
    struct.pack_into("<I", sys, 0x28C, 3 << 8)      # refs=3
    # IndexBuffer (0x1F0): count=9 stride=4 (36 bytes >= 32) at +8/+12, gfx=132 at +24
    struct.pack_into("<IH", sys, 0x1F8, 9, 4)
    struct.pack_into("<Q", sys, 0x208, 0x60000000 + 132)
    # FVF (0x290, 320 bytes): posOff=96 at +0, posSize=12 at +0xD0, fmt=6 at +0x104, flags=(3<<10)|1 at +0x138
    struct.pack_into("<I", sys, 0x290, 96)
    sys[0x290 + 0xD0], sys[0x290 + 0x104] = 12, 6
    struct.pack_into("<I", sys, 0x290 + 0x138, (3 << 10) | 1)
    sys[0x3F0:0x3F4] = b"box\x00"

    gfx = bytearray(132 + 44)
    struct.pack_into("<I", gfx, 0, 0x63)            # float3 position marker
    struct.pack_into("<fffffffff", gfx, 96, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
    ib_at = 132
    struct.pack_into("<III", gfx, ib_at, 1, 1, 3)   # 1 cluster, 1 tri, 3 refs
    struct.pack_into("<I", gfx, ib_at + 12, 2)      # cluster[0]: 3 vertices (2+1), 1 tri (0+1)
    struct.pack_into("<I", gfx, ib_at + 28, (0) | (1 << 7) | (2 << 14))  # tri (0, 1, 2)
    struct.pack_into("<III", gfx, ib_at + 32, 0, 1, 2)
    struct.pack_into("<IH", sys, 0x1F8, 11, 4)      # 11 * 4 = 44 bytes
    _recovered_rpf(path, bytes(sys), bytes(gfx))


def run(*args):
    return subprocess.run(["dotnet", os.environ["RPF9DUMP_DLL"], *map(str, args)], capture_output=True, text=True)


@unittest.skipUnless(os.environ.get("RPF9DUMP_DLL"), "requires a built rpf9dump")
class Rpf9dumpExitCodeTest(unittest.TestCase):
    def test_failed_xma_write_exits_nonzero_and_rerun_completes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "audio.rpf"
            awc_rpf(src)
            wav = root / "out" / "audio" / "xma" / "audio_0001_at_0000000000001000" / "00001234.xma.wav"
            blocker = Path(str(wav) + ".partial")
            blocker.mkdir(parents=True)          # the write of this one stream fails
            failed = run("xma", src, root / "out")
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertFalse(wav.exists())
            blocker.rmdir()
            retry = run("xma", src, root / "out")
            self.assertEqual(retry.returncode, 0, retry.stdout + retry.stderr)
            self.assertEqual(wav.read_bytes()[:4], b"RIFF")
            self.assertEqual(wav.read_bytes()[-64:], b"\x11" * 64)

    def test_failed_textures_write_exits_nonzero_withholds_manifest_and_reruns(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "tex.rpf"
            tex_rpf(src)
            mode_dir = root / "out" / "tex" / "textures"
            manifest = mode_dir / "manifest.json"
            mode_dir.mkdir(parents=True)
            manifest.write_text('{"stale": true}', encoding="utf-8")
            dds = mode_dir / "dictionary_at_0000000000001010" / "000_tex0.dds"
            blocker = Path(str(dds) + ".partial")
            blocker.mkdir(parents=True)
            failed = run("textures", src, root / "out")
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertFalse(manifest.exists())
            self.assertFalse(Path(str(manifest) + ".partial").exists())
            self.assertFalse(dds.exists())
            blocker.rmdir()
            retry = run("textures", src, root / "out")
            self.assertEqual(retry.returncode, 0, retry.stdout + retry.stderr)
            self.assertTrue(manifest.is_file())
            self.assertEqual(dds.read_bytes()[:4], b"DDS ")

    def test_failed_models_write_exits_nonzero_withholds_manifest_and_reruns(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "mdl.rpf"
            model_rpf(src)
            mode_dir = root / "out" / "mdl" / "models"
            manifest = mode_dir / "manifest.json"
            mode_dir.mkdir(parents=True)
            manifest.write_text('{"stale": true}', encoding="utf-8")
            obj = mode_dir / "box_at_0000000000001010.obj"
            blocker = Path(str(obj) + ".partial")
            blocker.mkdir(parents=True)
            failed = run("models", src, root / "out")
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertFalse(manifest.exists())
            self.assertFalse(Path(str(manifest) + ".partial").exists())
            self.assertFalse(obj.exists())
            blocker.rmdir()
            retry = run("models", src, root / "out")
            self.assertEqual(retry.returncode, 0, retry.stdout + retry.stderr)
            self.assertTrue(manifest.is_file())
            self.assertIn("v ", obj.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
