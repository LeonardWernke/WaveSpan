"""The disassembler must stop with a reason instead of writing empty .s files and calling them clean.

Issue: the official Windows LLVM build ships no llvm-mc and has no AMDGPU target. Before this check, a
"llvm-mc" that existed but could not disassemble produced empty output, counted 0 invalid words, and every
shader was reported "clean" (and make_game.py translated nothing). The probe now catches it once, up front.

    python -I tests/test_shader_disasm.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "shader_disasm.py")

# Two instructions: s_nop 0 then s_endpgm. The probe sends only the s_endpgm, so a fake tool can tell the
# one-instruction probe from the real shader by the input it receives.
CODE = b"\x00\x00\x80\xbf\x00\x00\x81\xbf"
PROBE = "0x00 0x00 0x81 0xbf"
ASM = "\ts_nop 0 ; encoding: [0x00,0x00,0x80,0xbf]\n\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]\n"


def fssm(code: bytes = CODE) -> bytes:
    """A minimal FSSM blob: the code offset at 0xC0, then code at offset + 0x28."""
    blob = bytearray(max(0x200, 0x128 + len(code)))
    blob[0:4] = b"FSSM"
    blob[0xC0:0xC4] = (0x100).to_bytes(4, "little")
    blob[0x128:0x128 + len(code)] = code
    return bytes(blob)


class DisasmFailureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.fssm = os.path.join(self.dir, "one.fssm")
        with open(self.fssm, "wb") as f:
            f.write(fssm())

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, llvm_mc: str, out: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, LLVM_MC=llvm_mc)
        return subprocess.run([sys.executable, "-I", TOOL, self.fssm, "--out", out, "--jobs", "1"],
                              capture_output=True, text=True, env=env)

    def fake(self, body: str) -> str:
        path = os.path.join(self.dir, "fake_mc.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
        return f'"{sys.executable}" "{path}"'  # a command line, not just a path: LLVM_MC must accept both

    def test_missing_tool(self):
        out = os.path.join(self.dir, "out")
        p = self.run_tool("definitely-not-an-llvm-mc", out)
        self.assertEqual(p.returncode, 1)
        self.assertIn("could not be started", p.stderr)

    def test_no_amdgpu_target(self):
        mc = self.fake("import sys\n"
                       "sys.stderr.write('LLVM ERROR: No available targets are compatible with triple "
                       "\\\"amdgcn--amdpal\\\"\\n')\n"
                       "sys.exit(1)\n")
        p = self.run_tool(mc, os.path.join(self.dir, "out"))
        self.assertEqual(p.returncode, 1)
        self.assertIn("No available targets", p.stderr)
        self.assertIn("AMDGPU target", p.stderr)

    def test_empty_output_is_a_failure_and_writes_nothing(self):
        mc = self.fake("import sys\n"
                       "if sys.stdin.read().strip() == %r:\n"
                       "    print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n" % PROBE)
        out = os.path.join(self.dir, "out")
        p = self.run_tool(mc, out)
        self.assertEqual(p.returncode, 1)
        self.assertIn("produced no disassembly", p.stderr)
        self.assertEqual(sorted(os.listdir(out)), [])

    def test_llvm_error_discards_partial_disassembly(self):
        mc = self.fake("import sys\n"
                       "sys.stdin.read()\n"
                       "if '-show-encoding' in sys.argv:\n"
                       "    print('s_nop 0')\n"
                       "    sys.stderr.write('error: disassembly stopped\\n')\n"
                       "    sys.exit(1)\n"
                       "print('s_endpgm')\n")
        out = os.path.join(self.dir, "out")
        p = self.run_tool(mc, out)
        self.assertEqual(p.returncode, 1, p.stdout)
        self.assertIn("'failed': 1", p.stdout)
        self.assertIn(self.fssm, p.stderr)
        self.assertIn("error: disassembly stopped", p.stderr)
        self.assertEqual(len(p.stderr.splitlines()), 1)
        self.assertEqual(sorted(os.listdir(out)), [])

    def test_no_code_is_an_error_when_no_listings_are_written(self):
        with open(self.fssm, "wb") as f:
            f.write(b"FSSM\x01\x01" + bytes(320))
        mc = self.fake("import sys\n"
                       "sys.stdin.read()\n"
                       "print('s_endpgm')\n")
        out = os.path.join(self.dir, "out")
        p = self.run_tool(mc, out)
        self.assertEqual(p.returncode, 1, p.stdout)
        self.assertIn("'no s_endpgm': 1", p.stdout)
        self.assertIn(self.fssm, p.stderr)
        self.assertEqual(len(p.stderr.splitlines()), 1)
        self.assertEqual(sorted(os.listdir(out)), [])

    def test_a_real_disassembly_is_written(self):
        mc = self.fake("import sys\n"
                       "probe = sys.stdin.read().strip() == %r\n"
                       "print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]' if probe else %r)\n"
                       % (PROBE, ASM))
        out = os.path.join(self.dir, "out")
        p = self.run_tool(mc, out)
        self.assertEqual(p.returncode, 0, p.stderr)
        written = os.listdir(out)
        self.assertEqual(written, ["one.s"])
        with open(os.path.join(out, "one.s"), encoding="utf-8") as f:
            body = f.read()
        self.assertIn("s_endpgm", body)
        self.assertIn("0 invalid words", body)


class DisasmInputTest(unittest.TestCase):
    """Short blobs, bad --jobs/--limit and an empty folder end with a reason, not a traceback."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.src = os.path.join(self.dir, "fssm")
        os.makedirs(self.src)
        mc = os.path.join(self.dir, "fake_mc.py")
        with open(mc, "w", encoding="utf-8") as f:
            f.write("import sys\n"
                    "probe = sys.stdin.read().strip() == %r\n"
                    "print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]' if probe else %r)\n" % (PROBE, ASM))
        self.mc = f'"{sys.executable}" "{mc}"'

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, LLVM_MC=self.mc)
        return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True, env=env)

    def write(self, name: str, data: bytes) -> None:
        with open(os.path.join(self.src, name), "wb") as f:
            f.write(data)

    def test_code_range_scans_a_blob_shorter_than_the_header(self):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        try:
            import shader_disasm
        finally:
            sys.path.pop(0)
        self.assertEqual(shader_disasm.code_range(b"FSSM" + CODE), (None, 12, "scan"))
        self.assertEqual(shader_disasm.code_range(b"FSSM"), (None, None, "scan"))
        self.assertEqual(shader_disasm.code_range(b""), (None, None, "scan"))

    def test_a_short_blob_does_not_stop_the_others(self):
        self.write("a_good.fssm", fssm())
        self.write("b_short.fssm", b"FSSM" + CODE)   # 12 bytes: no room for the code offset at 0xC0
        self.write("c_good.fssm", fssm())
        out = os.path.join(self.dir, "out")
        p = self.run_tool(self.src, "--out", out, "--jobs", "1")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertEqual(sorted(os.listdir(out)), ["a_good.s", "c_good.s"])
        self.assertIn("'clean': 2", p.stdout)
        self.assertIn("'scan failed': 1", p.stdout)

    def test_directory_with_fssm_suffix_does_not_count_toward_limit(self):
        os.mkdir(os.path.join(self.src, "a_cache.fssm"))
        self.write("b_shader.fssm", fssm())
        out = os.path.join(self.dir, "out")

        result = self.run_tool(self.src, "--out", out, "--limit", "1")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(os.listdir(out), ["b_shader.s"])

    def test_jobs_and_limit_must_be_at_least_one(self):
        self.write("one.fssm", fssm())
        out = os.path.join(self.dir, "out")
        for flag, value in (("--jobs", "0"), ("--jobs", "-2"), ("--limit", "0"), ("--limit", "-1")):
            with self.subTest(flag=flag, value=value):
                p = self.run_tool(self.src, "--out", out, flag, value)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertIn("must be at least 1", p.stderr)
        self.assertFalse(os.path.exists(out))

    def test_a_folder_without_fssm_files_is_an_error(self):
        for args in ((self.src,), (self.src, "--out", os.path.join(self.dir, "out"))):
            with self.subTest(args=args):
                p = self.run_tool(*args)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertIn("no .fssm files", p.stderr)
                self.assertNotIn("Traceback", p.stderr)

    def test_a_missing_source_is_an_error(self):
        p = self.run_tool(os.path.join(self.dir, "nope"), "--out", os.path.join(self.dir, "out"))
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("is not a file or folder", p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_an_out_that_is_a_file_is_an_error(self):
        self.write("one.fssm", fssm())
        out = os.path.join(self.dir, "out")
        with open(out, "w", encoding="utf-8") as f:
            f.write("keep")
        p = self.run_tool(self.src, "--out", out)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("cannot create the output folder", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        with open(out, encoding="utf-8") as f:
            self.assertEqual(f.read(), "keep")


class DisasmBatchTest(unittest.TestCase):
    """Batching invokes llvm-mc once per batch and falls back per-shader on any error or sentinel mismatch."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.src = os.path.join(self.dir, "fssm")
        os.makedirs(self.src)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name: str, code: bytes) -> str:
        path = os.path.join(self.src, name)
        with open(path, "wb") as f:
            f.write(fssm(code))
        return path

    def fake(self, body: str) -> str:
        path = os.path.join(self.dir, "fake_mc.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
        return f'"{sys.executable}" "{path}"'

    def run_tool(self, llvm_mc: str, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, LLVM_MC=llvm_mc)
        return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True, env=env)

    def test_batch_disassembles_multiple_shaders_in_one_call(self):
        log = os.path.join(self.dir, "calls.log")
        mc = self.fake(
            "import sys\n"
            f"with open({log!r}, 'a', encoding='utf-8') as f:\n"
            "    f.write('call\\n')\n"
            "data = sys.stdin.read().strip()\n"
            f"if data == {PROBE!r}:\n"
            "    print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n"
            "    sys.exit(0)\n"
            "sent = '0xfe 0x00 0x80 0xbf 0xff 0x00 0x80 0xbf'\n"
            "parts = data.split(sent)\n"
            "out = []\n"
            "for idx, part in enumerate(parts):\n"
            "    lines = []\n"
            "    toks = part.strip().split()\n"
            "    for i in range(0, len(toks), 4):\n"
            "        b = toks[i:i+4]\n"
            "        if b == ['0x00', '0x00', '0x81', '0xbf']:\n"
            "            lines.append('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n"
            "        else:\n"
            "            lines.append(f'\\ts_nop {int(b[0], 16)} ; encoding: [{b[0]},{b[1]},{b[2]},{b[3]}]')\n"
            "    out.append('\\n'.join(lines))\n"
            "sep = '\\n\\ts_nop 0xfe ; encoding: [0xfe,0x00,0x80,0xbf]\\n\\ts_nop 0xff ; encoding: [0xff,0x00,0x80,0xbf]\\n'\n"
            "print(sep.join(out))\n"
        )
        for i in range(4):
            self.write(f"s{i}.fssm", bytes([i, 0x00, 0x80, 0xbf]) + b"\x00\x00\x81\xbf")
        out = os.path.join(self.dir, "out")
        p = self.run_tool(mc, self.src, "--out", out, "--jobs", "1")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("'clean': 4", p.stdout)
        with open(log, encoding="utf-8") as f:
            self.assertEqual(len(f.read().splitlines()), 2)  # 1 probe + 1 batch
        for i in range(4):
            with open(os.path.join(out, f"s{i}.s"), encoding="utf-8") as f:
                body = f.read()
            self.assertIn(f"s_nop {i}", body)
            self.assertIn("0 invalid words", body)

    def test_batch_falls_back_on_stderr_and_preserves_invalid_words(self):
        mc = self.fake(
            "import sys\n"
            "data = sys.stdin.read().strip()\n"
            f"if data == {PROBE!r}:\n"
            "    print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n"
            "    sys.exit(0)\n"
            "if '0xde 0xad 0xbe 0xef' in data:\n"
            "    sys.stderr.write('warning: invalid instruction encoding\\n')\n"
            "if '0xfe 0x00 0x80 0xbf' in data:\n"
            "    print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n"
            "    sys.exit(0)\n"
            "print('\\ts_nop 0 ; encoding: [0x00,0x00,0x80,0xbf]\\n\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n"
        )
        self.write("a_clean.fssm", CODE)
        self.write("b_bad.fssm", b"\xde\xad\xbe\xef\x00\x00\x81\xbf")
        self.write("c_clean.fssm", CODE)
        out = os.path.join(self.dir, "out")
        p = self.run_tool(mc, self.src, "--out", out, "--jobs", "1")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("'clean': 2", p.stdout)
        self.assertIn("'invalid words': 1", p.stdout)
        with open(os.path.join(out, "a_clean.s"), encoding="utf-8") as f:
            a_body = f.read()
        with open(os.path.join(out, "b_bad.s"), encoding="utf-8") as f:
            b_body = f.read()
        self.assertIn("0 invalid words", a_body)
        self.assertNotIn("llvm-mc messages", a_body)
        self.assertIn("1 invalid words", b_body)
        self.assertIn("; --- llvm-mc messages ---\n; warning: invalid instruction encoding\n", b_body)

    def test_batch_falls_back_on_nonzero_exit_or_sentinel_mismatch(self):
        for mode in ("nonzero", "mismatch"):
            with self.subTest(mode=mode):
                sub = os.path.join(self.dir, mode)
                os.makedirs(sub, exist_ok=True)
                for name in ("one.fssm", "two.fssm"):
                    with open(os.path.join(sub, name), "wb") as f:
                        f.write(fssm(CODE))
                mc = self.fake(
                    "import sys\n"
                    "data = sys.stdin.read().strip()\n"
                    f"if data == {PROBE!r}:\n"
                    "    print('\\ts_endpgm ; encoding: [0x00,0x00,0x81,0xbf]')\n"
                    "    sys.exit(0)\n"
                    "if '0xfe 0x00 0x80 0xbf' in data:\n"
                    + ("    sys.exit(1)\n" if mode == "nonzero" else f"    print({ASM!r})\n    sys.exit(0)\n")
                    + f"print({ASM!r}, end='')\n"
                )
                out = os.path.join(self.dir, f"out_{mode}")
                p = self.run_tool(mc, sub, "--out", out, "--jobs", "1")
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertIn("'clean': 2", p.stdout)
                self.assertEqual(sorted(os.listdir(out)), ["one.s", "two.s"])

    def test_batch_matches_single_shader_output_with_real_llvm_mc(self):
        llvm_mc = os.environ.get("LLVM_MC")
        if not llvm_mc:
            self.skipTest("LLVM_MC not set")
        shaders = {
            "a_nop.fssm": CODE,
            "b_branch.fssm": b"\x00\x00\x80\xbf\x02\x00\x82\xbf\x00\x00\x81\xbf",
            "c_invalid.fssm": b"\xff\xff\xff\xff\x00\x00\x81\xbf",
        }
        paths = [self.write(name, code) for name, code in shaders.items()]
        out_batch = os.path.join(self.dir, "out_batch")
        p_batch = self.run_tool(llvm_mc, self.src, "--out", out_batch, "--jobs", "1")
        self.assertEqual(p_batch.returncode, 0, p_batch.stderr)
        self.assertIn("'clean': 2", p_batch.stdout)
        self.assertIn("'invalid words': 1", p_batch.stdout)

        out_single = os.path.join(self.dir, "out_single")
        for path in paths:
            p_one = self.run_tool(llvm_mc, path, "--out", out_single, "--jobs", "1")
            self.assertEqual(p_one.returncode, 0, p_one.stderr)

        for name in shaders:
            s_name = os.path.splitext(name)[0] + ".s"
            with open(os.path.join(out_batch, s_name), encoding="utf-8") as fb, \
                 open(os.path.join(out_single, s_name), encoding="utf-8") as fs:
                self.assertEqual(fb.read(), fs.read(), s_name)


if __name__ == "__main__":
    unittest.main()
