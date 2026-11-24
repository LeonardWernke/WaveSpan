"""Desktop front end for make_game.py. The terminal builder owns validation and build state."""

import os
import queue
import runpy
import signal
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk


BACKGROUND = "#0b1019"
PANEL = "#121b29"
FIELD = "#0e1521"
BORDER = "#29394f"
TEXT = "#f2f7fc"
MUTED = "#a6b7ca"
ACCENT = "#61e3fa"
ACCENT_HOVER = "#a1effb"
WARNING = "#f0bd80"
ERROR = "#ffa7a2"
FONT = "Segoe UI"
ART = Path(__file__).resolve().parents[1] / "assets"


class Launcher:
    def __init__(self, root, builder, default_out, find_dumps):
        self.root = root
        self.builder = builder
        self.find_dumps = find_dumps
        self.game = tk.StringVar(root)
        self.output = tk.StringVar(root, default_out)
        self.work = tk.StringVar(root)
        self.full_check = tk.BooleanVar(root)
        self.status = tk.StringVar(root, "Ready")
        self.events = queue.SimpleQueue()
        self.process = None
        self.stopping = False
        self.close_pending = False
        self.scanning = False
        self.browse_buttons = []

        root.title("WaveSpan")
        root.configure(bg=BACKGROUND)
        root.geometry("1060x804")
        root.minsize(900, 716)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(2, weight=1)
        self.configure_styles()
        self.header()
        self.workspace()
        self.activity()
        self.game_input.focus_set()
        root.after(70, self.poll_events)
        root.after(180, self.start_scan)

    def configure_styles(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        self.root.option_add("*TCombobox*Listbox.background", FIELD)
        self.root.option_add("*TCombobox*Listbox.foreground", TEXT)
        self.root.option_add("*TCombobox*Listbox.selectBackground", "#225169")
        style.configure("Wave.TButton", background=BORDER, foreground=TEXT, bordercolor=BORDER,
                        lightcolor=BORDER, darkcolor=BORDER, focuscolor=ACCENT, focusthickness=1,
                        font=(FONT, 10, "bold"), padding=(14, 7), anchor="center")
        style.map("Wave.TButton", background=[("disabled", FIELD), ("active", "#354c65")],
                  foreground=[("disabled", MUTED)])
        style.configure("Primary.TButton", background=ACCENT, foreground=BACKGROUND,
                        bordercolor=ACCENT, lightcolor=ACCENT, darkcolor=ACCENT,
                        focuscolor=BACKGROUND, focusthickness=1, font=(FONT, 10, "bold"), padding=(18, 8))
        style.map("Primary.TButton", background=[("disabled", BORDER), ("active", ACCENT_HOVER)],
                  foreground=[("disabled", MUTED)], bordercolor=[("disabled", BORDER)],
                  lightcolor=[("disabled", BORDER)], darkcolor=[("disabled", BORDER)])
        for name in ("Wave.TEntry", "Wave.TCombobox"):
            style.configure(name, fieldbackground=FIELD, background=FIELD, foreground=TEXT, arrowcolor=ACCENT,
                            insertcolor=TEXT, selectbackground="#225169", selectforeground=TEXT,
                            bordercolor=BORDER, lightcolor=FIELD, darkcolor=FIELD, padding=(9, 6))
            style.map(name, fieldbackground=[("disabled", FIELD)], foreground=[("disabled", MUTED)],
                      bordercolor=[("focus", ACCENT)], background=[("active", BORDER)])
        style.configure("Wave.TCheckbutton", background=PANEL, foreground=MUTED, font=(FONT, 10),
                        indicatorbackground=FIELD, indicatorforeground=BACKGROUND, indicatormargin=(0, 2, 8, 2),
                        focuscolor=ACCENT)
        style.map("Wave.TCheckbutton", background=[("active", PANEL)], foreground=[("active", TEXT)],
                  indicatorbackground=[("selected", ACCENT)])
        style.configure("Wave.Vertical.TScrollbar", background=BORDER, troughcolor=FIELD,
                        bordercolor=FIELD, arrowcolor=MUTED, darkcolor=BORDER, lightcolor=BORDER)
        style.map("Wave.Vertical.TScrollbar", background=[("active", "#354c65")])
        style.configure("Wave.Horizontal.TProgressbar", background=ACCENT, troughcolor=BORDER,
                        bordercolor=BORDER, lightcolor=ACCENT, darkcolor=ACCENT, thickness=3)

    def label(self, parent, text, size=10, color=TEXT, bold=False, **options):
        return tk.Label(parent, text=text, bg=parent.cget("bg"), fg=color, anchor="w", justify="left",
                        font=(FONT, size, "bold" if bold else "normal"), **options)

    def card(self, parent):
        return tk.Frame(parent, bg=PANEL, highlightbackground=BORDER, highlightthickness=1)

    def button(self, parent, label, callback, primary=False):
        return ttk.Button(parent, text=label, command=callback,
                          style="Primary.TButton" if primary else "Wave.TButton")

    def header(self):
        header = tk.Frame(self.root, bg=BACKGROUND)
        header.grid(row=0, column=0, padx=24, pady=(16, 13), sticky="ew")
        icon = tk.PhotoImage(master=self.root, file=ART / "wavespan-mark.png")
        self.mark = icon.subsample(2)
        self.root.iconphoto(True, icon, self.mark)
        tk.Label(header, image=self.mark, bg=BACKGROUND, borderwidth=0).pack(side="left", padx=(0, 11))
        self.label(header, "WaveSpan", 20, bold=True).pack(side="left")
        self.label(header, "Experimental runtime", color=WARNING, bold=True).pack(side="right")

    def workspace(self):
        main = tk.Frame(self.root, bg=BACKGROUND)
        main.grid(row=1, column=0, padx=24, sticky="ew")
        main.columnconfigure(1, weight=1)

        overview = self.card(main)
        overview.grid(row=0, column=0, sticky="nsew", padx=(0, 14))
        self.artwork = tk.PhotoImage(master=self.root, file=ART / "wavespan-hero.png")
        tk.Label(overview, image=self.artwork, bg=PANEL, borderwidth=0).pack(fill="x")
        notes = tk.Frame(overview, bg=PANEL)
        notes.pack(fill="x", padx=18, pady=(12, 15))
        self.label(notes, "Xbox Series to Windows", 16, bold=True).pack(fill="x")
        self.label(notes, "Translate shaders, export assets, and build the Windows runtime from your own package.",
                   color=MUTED, wraplength=300).pack(fill="x", pady=(5, 0))
        self.label(notes, "Retail gameplay is unavailable.", color=WARNING).pack(fill="x", pady=(10, 0))

        setup = self.card(main)
        setup.grid(row=0, column=1, sticky="nsew")
        setup.columnconfigure(0, weight=1)
        content = tk.Frame(setup, bg=PANEL)
        content.grid(row=0, column=0, sticky="ew", padx=18, pady=14)
        content.columnconfigure(0, weight=1)

        self.label(content, "Build setup", 18, bold=True).grid(row=0, column=0, sticky="w")
        self.scan_button = self.button(content, "Find packages", self.start_scan)
        self.scan_button.grid(row=0, column=1, sticky="e")
        self.game_input = self.path_row(content, 1, "Game package", self.game, package=True)
        self.output_input = self.path_row(content, 3, "Build destination", self.output)
        self.work_input = self.path_row(content, 5, "Work folder (optional)", self.work)
        self.label(content, "Defaults to the work subfolder in your build destination.", color=MUTED).grid(
            row=7, column=0, columnspan=2, sticky="w", pady=(3, 0))
        self.check_option = ttk.Checkbutton(content, text="Verify every file with SHA-256",
                                            variable=self.full_check, style="Wave.TCheckbutton")
        self.check_option.grid(row=8, column=0, columnspan=2, sticky="w", pady=(9, 10))

        actions = tk.Frame(content, bg=PANEL)
        actions.grid(row=9, column=0, columnspan=2, sticky="ew")
        self.build_button = self.button(actions, "Build workspace", lambda: self.start_build(False), primary=True)
        self.build_button.pack(side="right")
        self.check_button = self.button(actions, "Check package", lambda: self.start_build(True))
        self.check_button.pack(side="right", padx=(0, 8))
        self.form_widgets = (self.game_input, self.output_input, self.work_input, self.check_option,
                             self.check_button, self.build_button, *self.browse_buttons)

    def path_row(self, parent, row, label, variable, package=False):
        self.label(parent, label, bold=True).grid(row=row, column=0, sticky="w", pady=(7, 2))
        if package:
            entry = ttk.Combobox(parent, textvariable=variable, style="Wave.TCombobox", font=(FONT, 10), width=20)
        else:
            entry = ttk.Entry(parent, textvariable=variable, style="Wave.TEntry", font=(FONT, 10), width=20)
        entry.grid(row=row + 1, column=0, sticky="ew", padx=(0, 8))
        button = self.button(parent, "Browse…", lambda: self.browse(label, variable))
        button.grid(row=row + 1, column=1, sticky="ew")
        self.browse_buttons.append(button)
        return entry

    def activity(self):
        panel = self.card(self.root)
        panel.grid(row=2, column=0, sticky="nsew", padx=24, pady=(14, 18))
        panel.columnconfigure(0, weight=1)
        panel.rowconfigure(2, weight=1)
        toolbar = tk.Frame(panel, bg=PANEL)
        toolbar.grid(row=0, column=0, sticky="ew", padx=14, pady=(8, 6))
        self.label(toolbar, "Build activity", 13, bold=True).pack(side="left")
        self.stop_button = self.button(toolbar, "Stop", self.stop)
        self.stop_button.pack(side="right")
        self.stop_button.state(["disabled"])
        self.status_label = self.label(toolbar, "", color=ACCENT, bold=True, textvariable=self.status)
        self.status_label.pack(side="right", padx=(0, 10))
        self.progress = ttk.Progressbar(panel, mode="indeterminate", style="Wave.Horizontal.TProgressbar")
        self.progress.grid(row=1, column=0, sticky="ew", padx=14)
        self.progress.grid_remove()

        console = tk.Frame(panel, bg=FIELD)
        console.grid(row=2, column=0, sticky="nsew", padx=14, pady=(7, 12))
        console.columnconfigure(0, weight=1)
        console.rowconfigure(0, weight=1)
        self.log = tk.Text(console, bg=FIELD, fg=MUTED, font=("Consolas", 10), borderwidth=0,
                           height=3, wrap="word", padx=10, pady=8, state="disabled", highlightthickness=0,
                           selectbackground="#225169", selectforeground=TEXT)
        scrollbar = ttk.Scrollbar(console, orient="vertical", style="Wave.Vertical.TScrollbar", command=self.log.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")

        def scroll_changed(first, last):
            scrollbar.set(first, last)
            if float(first) == 0 and float(last) == 1:
                scrollbar.grid_remove()
            else:
                scrollbar.grid()

        self.log.configure(yscrollcommand=scroll_changed)
        self.log.grid(row=0, column=0, sticky="nsew")
        self.log.tag_configure("accent", foreground=ACCENT)
        self.log.tag_configure("error", foreground=ERROR)
        self.write_log("Choose a game package to check its files or start a build.\n")

    def write_log(self, text, tag=""):
        self.log.configure(state="normal")
        at_bottom = self.log.yview()[1] == 1.0
        self.log.insert("end", text, tag)
        if at_bottom:
            self.log.see("end")
        self.log.configure(state="disabled")

    def browse(self, label, variable):
        current = variable.get()
        path = filedialog.askdirectory(parent=self.root, title=f"Choose {label.lower().split(' (')[0]}",
                                       initialdir=current if os.path.isdir(current) else str(Path.home()))
        if path:
            variable.set(os.path.normpath(path))

    def start_scan(self):
        if self.scanning or self.process is not None:
            return
        self.scanning = True
        self.scan_button.state(["disabled"])
        self.scan_button.configure(text="Searching…")
        threading.Thread(target=self.scan, daemon=True).start()

    def scan(self):
        try:
            found = self.find_dumps()
        except (OSError, ValueError) as error:
            self.events.put(("scan_error", str(error)))
        else:
            self.events.put(("scan", [path for path, _ in found]))

    def finish_scan(self, paths):
        self.scanning = False
        self.scan_button.configure(text="Find packages")
        self.scan_button.state(["disabled" if self.process is not None else "!disabled"])
        self.game_input.configure(values=paths)
        if len(paths) == 1:
            if not self.game.get().strip():
                self.game.set(paths[0])
            self.write_log(f"Found a supported package at {paths[0]}.\n")
        elif paths:
            self.write_log(f"Found {len(paths)} packages. Choose one from the game package list.\n")
        else:
            self.write_log("No packages found in common folders. Use Browse to choose one.\n")

    def start_build(self, check_only):
        if self.process is not None:
            return
        game = self.game.get().strip().strip('"')
        output = self.output.get().strip().strip('"')
        work = self.work.get().strip().strip('"')
        if not game or not os.path.isdir(game):
            messagebox.showinfo("WaveSpan", "Choose the folder containing MicrosoftGame.Config and the .rpf archives.",
                                parent=self.root)
            return
        if not check_only and not output:
            messagebox.showinfo("WaveSpan", "Choose a build destination.", parent=self.root)
            return

        command = [sys.executable, "-I", "-X", "utf8", "-u"]
        if os.name == "nt":
            # The builder and its tools share a private console so Stop only interrupts this build.
            startup = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=subprocess.SW_HIDE)
            command.append(__file__)
            group = {"creationflags": subprocess.CREATE_NEW_CONSOLE, "startupinfo": startup, "stdin": subprocess.PIPE}
        else:
            group = {"start_new_session": True, "stdin": subprocess.DEVNULL}
        command.extend([self.builder, "--game", game])
        if check_only:
            command.append("--check")
        else:
            command.extend(["--out", output])
            if work:
                command.extend(["--work", work])
        if self.full_check.get():
            command.append("--full-check")

        try:
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, encoding="utf-8", errors="replace", **group)
        except OSError as error:
            self.write_log(f"Could not start the builder. {error}\n", "error")
            self.set_status("Could not start", ERROR)
            return

        self.set_busy(True)
        self.set_status("Checking package" if check_only else "Building workspace")
        self.write_log("\nChecking package…\n" if check_only else "\nBuilding workspace…\n", "accent")
        self.log.see("end")
        threading.Thread(target=self.read_build, args=(self.process, check_only), daemon=True).start()

    def read_build(self, process, check_only):
        with process.stdout as output:
            for line in output:
                self.events.put(("line", line))
        returncode = process.wait()
        if process.stdin is not None:
            process.stdin.close()
        self.events.put(("exit", (returncode, check_only)))

    def set_status(self, status, color=ACCENT):
        self.status.set(status)
        self.status_label.configure(fg=color)

    def set_busy(self, busy):
        for widget in self.form_widgets:
            widget.state(["disabled" if busy else "!disabled"])
        self.scan_button.state(["disabled" if busy or self.scanning else "!disabled"])
        self.stop_button.state(["!disabled" if busy else "disabled"])
        if busy:
            self.progress.grid()
            self.progress.start(16)
        else:
            self.progress.stop()
            self.progress.grid_remove()

    def finish_build(self, returncode, check_only):
        stopped = self.stopping
        self.process = None
        self.stopping = False
        self.set_busy(False)
        if returncode == 0:
            status = "Package check passed" if check_only else "Build complete"
            self.set_status(status)
            self.write_log(f"{status}.\n", "accent")
        elif stopped or returncode == 130:
            self.set_status("Stopped", WARNING)
        else:
            self.set_status("Check failed" if check_only else "Build failed", ERROR)
            self.write_log(f"Builder exited with code {returncode}.\n", "error")
        if self.close_pending:
            self.root.destroy()

    def poll_events(self):
        for _ in range(150):
            try:
                event, value = self.events.get_nowait()
            except queue.Empty:
                break
            if event == "scan":
                self.finish_scan(value)
            elif event == "scan_error":
                self.scanning = False
                self.scan_button.configure(text="Find packages")
                if self.process is None:
                    self.scan_button.state(["!disabled"])
                self.write_log(f"Package search failed. {value}\n", "error")
            elif event == "line":
                self.write_log(value, "error" if value.startswith("ERROR:") else "")
            elif event == "exit":
                self.finish_build(*value)
                if self.close_pending:
                    return
        self.root.after(70, self.poll_events)

    def stop(self):
        if self.process is None or self.stopping:
            return False
        try:
            if os.name == "nt":
                os.write(self.process.stdin.fileno(), b"\x03")
            else:
                os.killpg(self.process.pid, signal.SIGINT)
        except (OSError, ValueError) as error:
            if self.process.poll() is None:
                self.write_log(f"Could not interrupt the builder. {error}\n", "error")
                return False
        self.stopping = True
        self.stop_button.state(["disabled"])
        self.set_status("Stopping…", WARNING)
        return True

    def close(self):
        if self.process is None:
            self.root.destroy()
        elif self.stopping:
            self.close_pending = True
        elif messagebox.askyesno("WaveSpan", "Stop the current operation and close WaveSpan?", parent=self.root):
            self.close_pending = self.stop()


def open_launcher(builder, default_out, find_dumps):
    root = tk.Tk()
    Launcher(root, builder, default_out, find_dumps)
    root.mainloop()


def run_windows_builder():
    """Receive Stop over a private pipe and deliver Ctrl+C to the builder's console."""
    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel = ctypes.windll.kernel32
    control_pipe = sys.stdin.fileno()
    # Tools that the builder spawns get EOF on stdin. Only the control thread reads the pipe.
    sys.stdin = open(os.devnull, encoding="utf-8")
    kernel.SetStdHandle(wintypes.DWORD(-10), wintypes.HANDLE(msvcrt.get_osfhandle(sys.stdin.fileno())))

    def wait_for_stop():
        if os.read(control_pipe, 1) == b"\x03":
            kernel.GenerateConsoleCtrlEvent(0, 0)  # CTRL_C_EVENT, all processes on this private console

    threading.Thread(target=wait_for_stop, daemon=True).start()
    try:
        runpy.run_path(sys.argv.pop(1), run_name="__main__")
    except KeyboardInterrupt:
        raise SystemExit(130) from None


if __name__ == "__main__":
    run_windows_builder()
