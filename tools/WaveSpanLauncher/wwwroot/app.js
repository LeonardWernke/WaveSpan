const gameInput = document.getElementById("gameInput");
const packageList = document.getElementById("packageList");
const outputInput = document.getElementById("outputInput");
const workInput = document.getElementById("workInput");
const fullCheckInput = document.getElementById("fullCheckInput");
const scanBtn = document.getElementById("scanBtn");
const browseGameBtn = document.getElementById("browseGameBtn");
const browseOutputBtn = document.getElementById("browseOutputBtn");
const browseWorkBtn = document.getElementById("browseWorkBtn");
const checkBtn = document.getElementById("checkBtn");
const buildBtn = document.getElementById("buildBtn");
const stopBtn = document.getElementById("stopBtn");
const heroBanner = document.getElementById("heroBanner");
const heroStatus = document.getElementById("heroStatus");
const buildGraph = document.getElementById("buildGraph");
const chartStages = new Map(Array.from(buildGraph.querySelectorAll("[data-stage]"),
  (stage) => [stage.dataset.stage, stage]));
const stageCount = document.getElementById("stageCount");
const graphPhase = document.getElementById("graphPhase");
const graphUnits = document.getElementById("graphUnits");
const graphMeter = document.getElementById("graphMeter");
const statusLabel = document.getElementById("statusLabel");
const progressTrack = document.getElementById("progressTrack");
const logConsole = document.getElementById("logConsole");

const formWidgets = [
  gameInput,
  outputInput,
  workInput,
  fullCheckInput,
  browseGameBtn,
  browseOutputBtn,
  browseWorkBtn,
  checkBtn,
  buildBtn,
];

function send(payload) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(payload);
  }
}

function setStatus(text, tone = "") {
  statusLabel.textContent = text;
  heroStatus.textContent = text;
  heroBanner.dataset.tone = tone;
  statusLabel.className = `status-label ${tone}`.trim();
}

let building = false;
let scanning = false;

function setBusy(busy, checkOnly = false) {
  building = busy;
  heroBanner.classList.toggle("is-active", busy);
  for (const el of formWidgets) {
    el.disabled = busy;
  }
  scanBtn.disabled = building || scanning;
  stopBtn.disabled = !busy;
  progressTrack.classList.toggle("hidden", !busy || !checkOnly);
}

function setGraphPhase(detail, done, total) {
  if (graphPhase.textContent !== detail) graphPhase.textContent = detail;
  const measured = Number.isInteger(done) && Number.isInteger(total) && total > 0 && done >= 0 && done <= total;
  graphUnits.textContent = measured ? `${done} / ${total}` : "";
  if (measured) {
    graphMeter.max = total;
    graphMeter.value = done;
  }
  graphMeter.hidden = !measured;
}

function setChartStage(stage, state) {
  stage.dataset.state = state;
  stage.setAttribute("aria-label", `${stage.textContent.trim()}, ${state}`);
  if (state === "running") stage.setAttribute("aria-current", "step");
  else stage.removeAttribute("aria-current");
}

function startBuildGraph(checkOnly) {
  buildGraph.hidden = checkOnly;
  heroBanner.classList.toggle("has-build-graph", !checkOnly);
  if (checkOnly) return;
  for (const stage of chartStages.values()) setChartStage(stage, "waiting");
  stageCount.textContent = `0 of ${chartStages.size} stages`;
  setGraphPhase("Starting build");
}

function updateBuildGraph({ stage, detail, done, total }) {
  const step = chartStages.get(stage);
  if (!building || buildGraph.hidden || !step) return;
  if (detail === null) {
    setChartStage(step, "complete");
    const completed = [...chartStages.values()].filter((item) => item.dataset.state === "complete").length;
    stageCount.textContent = `${completed} of ${chartStages.size} stages`;
    setGraphPhase(completed === chartStages.size ? "Finishing build" : "");
  } else if (typeof detail === "string") {
    if (step.dataset.state !== "running") setChartStage(step, "running");
    setGraphPhase(detail, done, total);
  }
}

function finishBuildGraph(state) {
  if (buildGraph.hidden) return;
  for (const stage of chartStages.values()) {
    if (stage.dataset.state === "running") setChartStage(stage, state);
  }
  if (state === "complete") setGraphPhase("Workspace ready");
}

function appendLog(text, tag = "") {
  const atBottom =
    logConsole.scrollHeight - logConsole.scrollTop - logConsole.clientHeight < 24;
  const span = document.createElement("span");
  if (tag) span.className = tag;
  span.textContent = text;
  logConsole.appendChild(span);
  if (logConsole.childElementCount > 5000) logConsole.firstElementChild.remove();
  if (atBottom) {
    logConsole.scrollTop = logConsole.scrollHeight;
  }
}

function normalizePathInput(value) {
  return value.trim().replace(/^"+|"+$/g, "");
}

scanBtn.addEventListener("click", () => send({ type: "scanPackages" }));
browseGameBtn.addEventListener("click", () =>
  send({ type: "browseFolder", target: "game", current: normalizePathInput(gameInput.value) })
);
browseOutputBtn.addEventListener("click", () =>
  send({ type: "browseFolder", target: "output", current: normalizePathInput(outputInput.value) })
);
browseWorkBtn.addEventListener("click", () =>
  send({ type: "browseFolder", target: "work", current: normalizePathInput(workInput.value) })
);

function startBuild(checkOnly) {
  send({
    type: "startBuild",
    game: normalizePathInput(gameInput.value),
    output: normalizePathInput(outputInput.value),
    work: normalizePathInput(workInput.value),
    fullCheck: fullCheckInput.checked,
    checkOnly,
  });
}

checkBtn.addEventListener("click", () => startBuild(true));
buildBtn.addEventListener("click", () => startBuild(false));
stopBtn.addEventListener("click", () => send({ type: "stopBuild" }));

if (!window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
  window.addEventListener("mousemove", (ev) => {
    const x = Math.round((ev.clientX / window.innerWidth) * 100);
    const y = Math.round(80 + (ev.clientY / window.innerHeight) * 20);
    document.documentElement.style.setProperty("--glow-x", `${x}%`);
    document.documentElement.style.setProperty("--glow-y", `${y}%`);
  });
}

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener("message", (event) => {
    const msg = event.data;
    switch (msg.type) {
      case "init":
        if (msg.defaultOut && !outputInput.value) {
          outputInput.value = msg.defaultOut;
        }
        break;
      case "scanStarted":
        scanning = true;
        scanBtn.disabled = true;
        scanBtn.textContent = "Searching…";
        break;
      case "scanComplete":
        scanning = false;
        scanBtn.disabled = building;
        scanBtn.textContent = "Find packages";
        packageList.innerHTML = "";
        for (const pkg of msg.packages || []) {
          const opt = document.createElement("option");
          opt.value = pkg;
          packageList.appendChild(opt);
        }
        if (msg.packages && msg.packages.length > 0 && !gameInput.value) {
          gameInput.value = msg.packages[0];
        }
        break;
      case "scanError":
        scanning = false;
        scanBtn.disabled = building;
        scanBtn.textContent = "Find packages";
        appendLog(`Package search failed. ${msg.error}\n`, "error");
        break;
      case "folderSelected":
        if (msg.target === "game") gameInput.value = msg.path;
        if (msg.target === "output") outputInput.value = msg.path;
        if (msg.target === "work") workInput.value = msg.path;
        break;
      case "buildStarted":
        setBusy(true, msg.checkOnly);
        startBuildGraph(msg.checkOnly);
        setStatus(msg.checkOnly ? "Checking package…" : "Building workspace…");
        break;
      case "buildStopping":
        stopBtn.disabled = true;
        setStatus("Stopping…", "warning");
        break;
      case "log":
        appendLog(msg.line, msg.tag || "");
        break;
      case "buildProgress":
        if (msg.progress) updateBuildGraph(msg.progress);
        break;
      case "buildFinished":
        if (!building && !msg.checkOnly) {
          startBuildGraph(false);
          setGraphPhase("");
        }
        setBusy(false);
        if (msg.returnCode === 0) {
          const status = msg.checkOnly ? "Package check passed" : "Build complete";
          setStatus(status, "success");
          if (!msg.checkOnly) finishBuildGraph("complete");
          appendLog(`${status}.\n`, "accent");
        } else if (msg.stopped || msg.returnCode === 130) {
          setStatus("Stopped", "warning");
          if (!msg.checkOnly) finishBuildGraph("stopped");
        } else {
          setStatus(msg.checkOnly ? "Check failed" : "Build failed", "error");
          if (!msg.checkOnly) finishBuildGraph("failed");
          appendLog(`Builder exited with code ${msg.returnCode}.\n`, "error");
        }
        break;
    }
  });
}

send({ type: "ready" });
