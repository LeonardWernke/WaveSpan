"""Unit tests for tools/wavespan_launcher.py and tools/WaveSpanLauncher/."""

import json
import os
import queue
import re
import shutil
import subprocess
import sys
import unittest
from html.parser import HTMLParser
from unittest import mock

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")
LAUNCHER_DIR = os.path.join(TOOLS_DIR, "WaveSpanLauncher")
WWWROOT_DIR = os.path.join(LAUNCHER_DIR, "wwwroot")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import wavespan_launcher  # noqa: E402


def _make_stub_launcher():
    launcher = wavespan_launcher.Launcher.__new__(wavespan_launcher.Launcher)
    launcher.root = mock.MagicMock()
    launcher.events = queue.Queue()
    launcher.process = None
    launcher.stopping = False
    launcher.close_pending = False
    launcher.scanning = False
    launcher.scan_timeout = None
    launcher.scan_button = mock.MagicMock()
    launcher.stop_button = mock.MagicMock()
    launcher.set_busy = mock.MagicMock()
    launcher.set_status = mock.MagicMock()
    launcher.write_log = mock.MagicMock()
    return launcher


class WavespanLauncherTests(unittest.TestCase):
    def test_close_destroys_immediately_when_idle(self):
        launcher = _make_stub_launcher()
        launcher.close()
        launcher.root.destroy.assert_called_once_with()

    def test_close_sets_pending_when_already_stopping(self):
        launcher = _make_stub_launcher()
        launcher.process = mock.MagicMock()
        launcher.stopping = True
        launcher.close()
        self.assertTrue(launcher.close_pending)
        launcher.root.destroy.assert_not_called()

    def test_close_destroys_if_process_exited_during_askyesno(self):
        launcher = _make_stub_launcher()
        launcher.process = mock.MagicMock()

        def simulate_exit_during_dialog(*args, **kwargs):
            launcher.finish_build(0, False)
            return True

        with mock.patch.object(
            wavespan_launcher.messagebox, "askyesno", side_effect=simulate_exit_during_dialog
        ):
            launcher.close()

        launcher.root.destroy.assert_called_once_with()
        self.assertFalse(launcher.close_pending)

    def test_close_stops_process_and_sets_pending_when_still_running(self):
        launcher = _make_stub_launcher()
        launcher.process = mock.MagicMock()
        launcher.stop = mock.MagicMock(return_value=True)

        with mock.patch.object(wavespan_launcher.messagebox, "askyesno", return_value=True):
            launcher.close()

        launcher.stop.assert_called_once_with()
        self.assertTrue(launcher.close_pending)
        launcher.root.destroy.assert_not_called()

    def test_finish_build_destroys_root_when_close_pending(self):
        launcher = _make_stub_launcher()
        launcher.process = mock.MagicMock()
        launcher.close_pending = True

        launcher.finish_build(0, False)

        self.assertIsNone(launcher.process)
        self.assertFalse(launcher.stopping)
        launcher.root.destroy.assert_called_once_with()

    def test_poll_events_stops_scheduling_after_exit_with_close_pending(self):
        launcher = _make_stub_launcher()
        launcher.process = mock.MagicMock()
        launcher.close_pending = True
        launcher.events.put(("exit", (0, False)))

        launcher.poll_events()

        launcher.root.destroy.assert_called_once_with()
        launcher.root.after.assert_not_called()


class _ElementAttrParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.by_id = {}

    def handle_starttag(self, tag, attrs):
        attr_map = dict(attrs)
        if "id" in attr_map:
            self.by_id[attr_map["id"]] = attr_map


class WaveSpanWebLauncherTests(unittest.TestCase):
    def test_html_accessibility_live_regions(self):
        with open(os.path.join(WWWROOT_DIR, "index.html"), encoding="utf-8") as handle:
            parser = _ElementAttrParser()
            parser.feed(handle.read())

        self.assertEqual(parser.by_id["heroStatus"].get("role"), "status")
        self.assertEqual(parser.by_id["heroStatus"].get("aria-live"), "polite")
        self.assertEqual(parser.by_id["statusLabel"].get("role"), "status")
        self.assertEqual(parser.by_id["statusLabel"].get("aria-live"), "polite")
        self.assertEqual(parser.by_id["logConsole"].get("role"), "log")
        self.assertEqual(parser.by_id["logConsole"].get("aria-live"), "polite")

    def test_css_forced_colors_rules(self):
        with open(os.path.join(WWWROOT_DIR, "styles.css"), encoding="utf-8") as handle:
            css = handle.read()

        match = re.search(r"@media\s*\(forced-colors:\s*active\)\s*\{(.*)\}\s*$", css, re.DOTALL)
        self.assertIsNotNone(match, "Expected @media (forced-colors: active) block in styles.css")
        block = match.group(1)
        for selector in (".progress-bar", ".wave-divider", ".hero-status::before"):
            self.assertIn(selector, block)

    def test_app_js_strips_quotes_and_updates_status(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")

        app_js_path = os.path.join(WWWROOT_DIR, "app.js")
        harness = r"""
const fs = require("fs");
const vm = require("vm");
const assert = require("assert");

const elements = {};
function makeElement(id) {
  const listeners = {};
  const classes = new Set();
  const el = {
    id,
    value: "",
    checked: false,
    disabled: false,
    textContent: "",
    innerHTML: "",
    className: "",
    dataset: {},
    children: [],
    scrollHeight: 100,
    scrollTop: 0,
    clientHeight: 100,
    get childElementCount() { return this.children.length; },
    get firstElementChild() { return this.children[0] || null; },
    appendChild(child) { this.children.push(child); },
    classList: {
      toggle(name, force) {
        if (force === undefined) {
          if (classes.has(name)) classes.delete(name); else classes.add(name);
        } else if (force) classes.add(name); else classes.delete(name);
      },
      contains(name) { return classes.has(name); }
    },
    addEventListener(type, fn) { (listeners[type] ||= []).push(fn); },
    click() { for (const fn of listeners.click || []) fn(); }
  };
  elements[id] = el;
  return el;
}

const posted = [];
const webviewListeners = {};
const sandbox = {
  document: {
    getElementById: (id) => elements[id] || makeElement(id),
    createElement: (tag) => ({ tag, value: "", textContent: "", className: "", remove() {} }),
    documentElement: { style: { setProperty() {} } },
  },
  window: {
    innerWidth: 1000,
    innerHeight: 800,
    matchMedia: () => ({ matches: true }),
    addEventListener() {},
    chrome: {
      webview: {
        postMessage: (msg) => posted.push(msg),
        addEventListener: (type, fn) => { (webviewListeners[type] ||= []).push(fn); },
      },
    },
  },
};

vm.runInNewContext(fs.readFileSync(process.argv[1], "utf8"), sandbox);
const normalize = (obj) => JSON.parse(JSON.stringify(obj));
assert.deepStrictEqual(normalize(posted.shift()), { type: "ready" });

elements.gameInput.value = '  "C:\\Games\\Xbox"  ';
elements.outputInput.value = '"C:\\Out"';
elements.workInput.value = ' "C:\\Work" ';
elements.fullCheckInput.checked = true;
elements.buildBtn.click();

assert.deepStrictEqual(normalize(posted.shift()), {
  type: "startBuild",
  game: "C:\\Games\\Xbox",
  output: "C:\\Out",
  work: "C:\\Work",
  fullCheck: true,
  checkOnly: false,
});

elements.browseGameBtn.click();
assert.deepStrictEqual(normalize(posted.shift()), {
  type: "browseFolder",
  target: "game",
  current: "C:\\Games\\Xbox",
});

for (const fn of webviewListeners.message) {
  fn({ data: { type: "buildStarted", checkOnly: false } });
}
assert.strictEqual(elements.statusLabel.textContent, "Building workspace\u2026");
assert.strictEqual(elements.heroStatus.textContent, "Building workspace\u2026");
assert.strictEqual(elements.stopBtn.disabled, false);
assert.strictEqual(elements.progressTrack.classList.contains("hidden"), false);

for (const fn of webviewListeners.message) {
  fn({ data: { type: "buildFinished", returnCode: 0, checkOnly: false, stopped: false } });
}
assert.strictEqual(elements.statusLabel.textContent, "Build complete");
assert.strictEqual(elements.heroStatus.textContent, "Build complete");
assert.strictEqual(elements.heroBanner.dataset.tone, "success");
assert.strictEqual(elements.stopBtn.disabled, true);
assert.strictEqual(elements.progressTrack.classList.contains("hidden"), true);
"""
        proc = subprocess.run([node, "-e", harness, app_js_path], capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)

    def test_mainform_csharp_contract(self):
        with open(os.path.join(LAUNCHER_DIR, "MainForm.cs"), encoding="utf-8") as handle:
            cs = handle.read()

        # 1. WAVESPAN_PYTHON is validated with the sys.version_info >= (3, 12) probe.
        find_python = cs[cs.index("private async Task<string> FindPythonAsync()"):cs.index("private ProcessStartInfo PythonStartInfo(")]
        self.assertIn("WAVESPAN_PYTHON", find_python)
        self.assertIn("sys.version_info >= (3, 12)", find_python)
        self.assertNotIn("return selected;", find_python)

        # 2. Path quote stripping and Directory.Exists(game) before spawning Python.
        start_build = cs[cs.index("private async Task StartBuildAsync("):cs.index("private bool StopBuild()")]
        self.assertIn("NormalizePathInput(game)", start_build)
        self.assertIn("NormalizePathInput(output)", start_build)
        self.assertIn("NormalizePathInput(work)", start_build)
        self.assertIn("Directory.Exists(game)", start_build)
        self.assertLess(start_build.index("Directory.Exists(game)"), start_build.index("FindPythonAsync()"))

        # 3. OnFormClosing handles StopBuild() == false without stranding the window open.
        on_closing = cs[cs.index("private void OnFormClosing("):cs.index("private void OnWebMessageReceived(")]
        self.assertIn("if (StopBuild())", on_closing)
        self.assertIn("_closePending = false;", on_closing)
        self.assertIn("e.Cancel = false;", on_closing)


if __name__ == "__main__":
    unittest.main()
