using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace WaveSpanLauncher;

public sealed class MainForm : Form
{
    private const int DwmwaUseImmersiveDarkMode = 20;
    private const int DwmwaWindowCornerPreference = 33;
    private const int DwmwaBorderColor = 34;
    private const int DwmwaCaptionColor = 35;
    private const int DwmwaTextColor = 36;
    private const string ProgressPrefix = "@wavespan-progress ";

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);

    private readonly WebView2 _webView;
    private readonly string _repoRoot;
    private readonly string _makeGamePath;
    private Process? _buildProcess;
    private bool _stopping;
    private bool _starting;
    private bool _scanning;
    private bool _closePending;

    public MainForm()
    {
        Text = "WaveSpan";
        if (Environment.ProcessPath is string executable)
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(executable);
        StartPosition = FormStartPosition.Manual;
        Location = Screen.FromPoint(Cursor.Position).WorkingArea.Location;
        BackColor = Color.FromArgb(8, 12, 23);

        _repoRoot = ResolveRepoRoot();
        _makeGamePath = Path.Combine(_repoRoot, "make_game.py");

        _webView = new WebView2
        {
            Dock = DockStyle.Fill,
            DefaultBackgroundColor = Color.FromArgb(8, 12, 23),
        };
        Controls.Add(_webView);

        Load += OnFormLoad;
        FormClosing += OnFormClosing;
    }

    protected override void OnLoad(EventArgs e)
    {
        Rectangle area = Screen.FromHandle(Handle).WorkingArea;
        int Pixels(int logical) => (int)Math.Round(logical * DeviceDpi / 96.0);
        int width = Math.Min(Pixels(1060), Math.Max(1, area.Width - Pixels(24)));
        int height = Math.Min(Pixels(804), Math.Max(1, area.Height - Pixels(24)));
        MinimumSize = new Size(Math.Min(Pixels(800), width), Math.Min(Pixels(560), height));
        SetBounds(area.Left + (area.Width - width) / 2, area.Top + (area.Height - height) / 2, width, height);
        base.OnLoad(e);
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        ApplyDarkDwmFrame();
    }

    private void ApplyDarkDwmFrame()
    {
        if (!OperatingSystem.IsWindows())
            return;

        int dark = 1;
        int roundCorners = 2;
        int borderColor = 0x473226;
        int captionColor = 0x170C08;
        int textColor = 0xFDF6F2;

        _ = DwmSetWindowAttribute(Handle, DwmwaUseImmersiveDarkMode, ref dark, sizeof(int));
        _ = DwmSetWindowAttribute(Handle, DwmwaWindowCornerPreference, ref roundCorners, sizeof(int));
        _ = DwmSetWindowAttribute(Handle, DwmwaBorderColor, ref borderColor, sizeof(int));
        _ = DwmSetWindowAttribute(Handle, DwmwaCaptionColor, ref captionColor, sizeof(int));
        _ = DwmSetWindowAttribute(Handle, DwmwaTextColor, ref textColor, sizeof(int));
    }

    private async void OnFormLoad(object? sender, EventArgs e)
    {
        try
        {
            string profile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                          "WaveSpan", "WebView2");
            var environment = await CoreWebView2Environment.CreateAsync(userDataFolder: profile);
            await _webView.EnsureCoreWebView2Async(environment);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not start WebView2. {ex.Message}", "WaveSpan");
            Close();
            return;
        }
        _webView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
        _webView.CoreWebView2.Settings.AreDevToolsEnabled = false;
        _webView.CoreWebView2.WebMessageReceived += OnWebMessageReceived;

        string wwwroot = Path.Combine(AppContext.BaseDirectory, "wwwroot");
        _webView.CoreWebView2.SetVirtualHostNameToFolderMapping(
            "wavespan.local",
            wwwroot,
            CoreWebView2HostResourceAccessKind.Allow);

        _webView.Source = new Uri("https://wavespan.local/index.html");
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        if (_buildProcess is null || _buildProcess.HasExited)
            return;

        if (_stopping)
        {
            _closePending = true;
            e.Cancel = true;
            return;
        }

        DialogResult result = MessageBox.Show(
            this,
            "Stop the current operation and close WaveSpan?",
            "WaveSpan",
            MessageBoxButtons.YesNo,
            MessageBoxIcon.Question);

        if (result != DialogResult.Yes)
        {
            e.Cancel = true;
            return;
        }

        if (_buildProcess is not null && !_buildProcess.HasExited)
        {
            if (StopBuild())
            {
                _closePending = true;
                e.Cancel = true;
            }
            else
            {
                _closePending = false;
                e.Cancel = false;
            }
        }
    }

    private void OnWebMessageReceived(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(e.WebMessageAsJson);
            JsonElement root = doc.RootElement;
            string type = root.GetProperty("type").GetString() ?? "";

            switch (type)
            {
                case "ready":
                    PostMessage(new { type = "init", defaultOut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Documents", "WaveSpan") });
                    _ = ScanPackagesAsync();
                    break;
                case "scanPackages":
                    _ = ScanPackagesAsync();
                    break;
                case "browseFolder":
                    BrowseFolder(
                        root.GetProperty("target").GetString() ?? "",
                        root.TryGetProperty("current", out JsonElement cur) ? cur.GetString() ?? "" : "");
                    break;
                case "startBuild":
                    _ = StartBuildAsync(
                        root.GetProperty("game").GetString() ?? "",
                        root.GetProperty("output").GetString() ?? "",
                        root.TryGetProperty("work", out JsonElement w) ? w.GetString() ?? "" : "",
                        root.TryGetProperty("fullCheck", out JsonElement fc) && fc.GetBoolean(),
                        root.TryGetProperty("checkOnly", out JsonElement co) && co.GetBoolean());
                    break;
                case "stopBuild":
                    StopBuild();
                    break;
            }
        }
        catch (Exception ex)
        {
            PostMessage(new { type = "log", line = $"ERROR: {ex.Message}\n", tag = "error" });
        }
    }

    private void BrowseFolder(string target, string current)
    {
        current = NormalizePathInput(current);
        using var dialog = new FolderBrowserDialog
        {
            Description = $"Choose {target}",
            UseDescriptionForTitle = true,
            InitialDirectory = Directory.Exists(current)
                ? current
                : Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        };

        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            PostMessage(new { type = "folderSelected", target, path = Path.GetFullPath(dialog.SelectedPath) });
        }
    }

    private async Task<string> RunPythonQueryAsync(string executable, string[] args, TimeSpan timeout)
    {
        using var process = new Process { StartInfo = PythonStartInfo(executable, args) };
        process.Start();
        Task<string> output = process.StandardOutput.ReadToEndAsync();
        Task<string> error = process.StandardError.ReadToEndAsync();
        using var deadline = new CancellationTokenSource(timeout);
        try
        {
            await process.WaitForExitAsync(deadline.Token);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
            throw new TimeoutException("Python did not respond in time.");
        }
        if (process.ExitCode != 0)
            throw new InvalidOperationException((await error).Trim());
        return (await output).Trim();
    }

    private async Task<string> FindPythonAsync()
    {
        const string probe = "import sys; assert sys.version_info >= (3, 12); print(sys.executable)";
        var candidates = new List<(string Executable, string[] Args)>();
        string selected = NormalizePathInput(Environment.GetEnvironmentVariable("WAVESPAN_PYTHON"));
        if (!string.IsNullOrEmpty(selected) && File.Exists(selected))
            candidates.Add((selected, ["-I", "-X", "utf8", "-c", probe]));

        candidates.Add(("py", ["-3", "-I", "-X", "utf8", "-c", probe]));
        candidates.Add(("python", ["-I", "-X", "utf8", "-c", probe]));

        foreach (var (executable, args) in candidates)
        {
            try
            {
                string path = await RunPythonQueryAsync(executable, args, TimeSpan.FromSeconds(5));
                if (File.Exists(path))
                    return path;
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException or TimeoutException)
            {
                // Try the next entry point if this launcher or interpreter is unavailable.
            }
        }
        throw new InvalidOperationException("Python 3.12 or newer is required. Install it or open WaveSpan through Build.bat.");
    }

    private ProcessStartInfo PythonStartInfo(string executable, IEnumerable<string> args)
    {
        var info = new ProcessStartInfo
        {
            FileName = executable,
            WorkingDirectory = _repoRoot,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8,
            CreateNoWindow = true,
        };
        foreach (string arg in args)
            info.ArgumentList.Add(arg);
        return info;
    }

    private async Task ScanPackagesAsync()
    {
        if (_scanning)
            return;
        _scanning = true;
        PostMessage(new { type = "scanStarted" });
        try
        {
            string python = await FindPythonAsync();
            const string scan = "import json,runpy,sys; print(json.dumps([p for p,v in runpy.run_path(sys.argv[1])['find_dumps']()]))";
            string result = await RunPythonQueryAsync(python, ["-I", "-X", "utf8", "-c", scan, _makeGamePath],
                                                      TimeSpan.FromSeconds(30));
            PostMessage(new { type = "scanComplete", packages = JsonSerializer.Deserialize<string[]>(result) });
        }
        catch (Exception ex)
        {
            PostMessage(new { type = "scanError", error = ex.Message });
        }
        finally
        {
            _scanning = false;
        }
    }

    private async Task StartBuildAsync(string game, string output, string work, bool fullCheck, bool checkOnly)
    {
        if (_starting || _buildProcess is not null)
            return;

        game = NormalizePathInput(game);
        output = NormalizePathInput(output);
        work = NormalizePathInput(work);

        if (string.IsNullOrWhiteSpace(game) || !Directory.Exists(game))
        {
            MessageBox.Show(this, "Choose the folder containing MicrosoftGame.Config and the .rpf archives.", "WaveSpan");
            return;
        }
        if (!checkOnly && string.IsNullOrWhiteSpace(output))
        {
            MessageBox.Show(this, "Choose a build destination.", "WaveSpan");
            return;
        }

        _starting = true;
        PostMessage(new { type = "buildStarting", checkOnly });
        int code = -1;
        try
        {
            string python = await FindPythonAsync();
            if (IsDisposed)
                return;
            string worker = Path.Combine(_repoRoot, "tools", "wavespan_launcher.py");
            var args = new List<string> { "-I", "-X", "utf8", "-u", worker, _makeGamePath, "--game", game,
                "--launcher-progress" };
            if (checkOnly)
                args.Add("--check");
            else
            {
                args.AddRange(["--out", output]);
                if (!string.IsNullOrWhiteSpace(work))
                    args.AddRange(["--work", work]);
            }
            if (fullCheck)
                args.Add("--full-check");

            var info = PythonStartInfo(python, args);
            info.RedirectStandardInput = true;
            using var process = new Process { StartInfo = info };
            process.OutputDataReceived += (_, ev) =>
            {
                if (ev.Data is not null)
                    ForwardBuildOutput(ev.Data);
            };
            process.ErrorDataReceived += (_, ev) =>
            {
                if (ev.Data is not null)
                    PostMessage(new { type = "log", line = ev.Data + "\n", tag = "error" });
            };
            process.Start();
            _buildProcess = process;
            PostMessage(new { type = "buildStarted", checkOnly });
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync();
            code = process.ExitCode;
        }
        catch (Exception ex)
        {
            PostMessage(new { type = "log", line = $"ERROR: {ex.Message}\n", tag = "error" });
        }
        finally
        {
            _starting = false;
            _buildProcess = null;
            bool stopped = _stopping;
            _stopping = false;
            PostMessage(new { type = "buildFinished", returnCode = code, checkOnly, stopped });
            if (_closePending)
                Close();
        }
    }

    private void ForwardBuildOutput(string line)
    {
        if (line.StartsWith(ProgressPrefix, StringComparison.Ordinal))
        {
            try
            {
                JsonElement progress = JsonSerializer.Deserialize<JsonElement>(line[ProgressPrefix.Length..]);
                PostMessage(new { type = "buildProgress", progress });
                return;
            }
            catch (JsonException)
            {
                // Keep malformed messages visible in the build log.
            }
        }
        PostMessage(new { type = "log", line = line + "\n", tag = line.StartsWith("ERROR:") ? "error" : "" });
    }

    private bool StopBuild()
    {
        if (_buildProcess is null || _buildProcess.HasExited || _stopping)
            return false;
        try
        {
            Stream input = _buildProcess.StandardInput.BaseStream;
            input.WriteByte(3);
            input.Flush();
            _stopping = true;
            PostMessage(new { type = "buildStopping" });
            return true;
        }
        catch (IOException ex)
        {
            PostMessage(new { type = "log", line = $"Could not interrupt the builder. {ex.Message}\n", tag = "error" });
            return false;
        }
    }

    private void PostMessage(object payload)
    {
        if (IsDisposed || !IsHandleCreated)
            return;
        if (InvokeRequired)
        {
            BeginInvoke(() => PostMessage(payload));
            return;
        }
        if (_webView.CoreWebView2 is null)
            return;

        string json = JsonSerializer.Serialize(payload, new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        });
        _webView.CoreWebView2.PostWebMessageAsJson(json);
    }

    internal static string NormalizePathInput(string? path)
        => string.IsNullOrWhiteSpace(path) ? "" : path.Trim().Trim('"');

    private static string ResolveRepoRoot()
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            if (File.Exists(Path.Combine(dir, "make_game.py")))
                return dir;
            dir = Directory.GetParent(dir)?.FullName;
        }
        return Environment.CurrentDirectory;
    }
}
