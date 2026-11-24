"""make_game.py: dynamic shard chunking, bounded worker pool, and per-stage timings.

Standard library only; no GPU, no Vulkan SDK, no game data.
"""
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import make_game  # noqa: E402


class DynamicShardBalanceTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = tmp.name
        self.disasm = os.path.join(self.root, "disasm")
        self.spirv = os.path.join(self.root, "spirv")
        os.makedirs(self.disasm)
        for i in range(12):
            with open(os.path.join(self.disasm, f"s{i:02d}.s"), "w", encoding="utf-8") as f:
                f.write("s_endpgm\n")

    def test_splits_into_smaller_shards_and_bounds_concurrency(self):
        jobs = 2
        expected_shards = jobs * make_game.SHARD_CHUNK_MULTIPLIER
        started = []
        active = 0
        max_active = 0
        logs = []

        class FakeProc:
            def __init__(self, args):
                nonlocal active, max_active
                shard_spec = args[args.index("--shard") + 1]
                self.i = int(shard_spec.split("/")[0])
                self.n = int(shard_spec.split("/")[1])
                self.polls = 0
                started.append((self.i, self.n))
                active += 1
                max_active = max(max_active, active)

            def _finish(self):
                nonlocal active
                active -= 1
                part = os.path.join(self._spirv, f"index.part{self.i}.json")
                with open(part, "w", encoding="utf-8") as f:
                    json.dump({"n": self.n}, f)
                return 0

            def poll(self):
                self.polls += 1
                if self.polls >= 1:
                    return self._finish()
                return None

            def wait(self):
                return self._finish()

        FakeProc._spirv = self.spirv

        with mock.patch.object(make_game.subprocess, "Popen", side_effect=FakeProc), \
                mock.patch.object(make_game, "py") as py, \
                mock.patch.object(make_game, "translator_digest", return_value="d1"), \
                mock.patch.object(make_game, "log", side_effect=logs.append):
            make_game.translate(self.disasm, self.spirv, jobs)

        self.assertEqual([i for i, _ in started], list(range(expected_shards)))
        self.assertTrue(all(n == expected_shards for _, n in started))
        self.assertLessEqual(max_active, jobs)
        self.assertEqual(py.call_count, 1)
        self.assertIn("--merge", py.call_args.args)

        with open(os.path.join(self.spirv, make_game.SHARD_PLAN), encoding="utf-8") as f:
            plan = json.load(f)
        self.assertEqual(plan["shards"], expected_shards)
        self.assertEqual(plan["jobs"], jobs)

        self.assertTrue(any("translation:" in m for m in logs), logs)
        self.assertTrue(any("shard merge:" in m for m in logs), logs)

    def test_pipelines_logs_validation_timing(self):
        out = os.path.join(self.root, "out")
        spirv = os.path.join(self.root, "shaders", "spirv")
        os.makedirs(spirv, exist_ok=True)
        with open(os.path.join(spirv, "index.json"), "w", encoding="utf-8") as f:
            f.write("{}")
        logs = []
        with mock.patch.object(make_game, "pipeline_check_exe", return_value="vk_pipeline_check.exe"), \
                mock.patch.object(make_game, "run"), \
                mock.patch.object(make_game, "log", side_effect=logs.append):
            make_game.pipelines(self.root, out, 4)
        self.assertTrue(any("validation took" in m for m in logs), logs)


if __name__ == "__main__":
    unittest.main()
