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


BACKGROUND = "#060813"
PANEL = "#0c1222"
RAISED = "#131c36"
FIELD = "#080d1a"
BORDER = "#1e2c4a"
SELECTION = "#1e3a5f"
TEXT = "#f4f7ff"
MUTED = "#94a3c4"
ACCENT = "#00e5ff"
PERIWINKLE = "#a855f7"
ACCENT_HOVER = "#5cf2ff"
WARNING = "#fbbf24"
ERROR = "#ff5c7c"
SUCCESS = "#34d399"
FONT = "Segoe UI"
ART = Path(__file__).resolve().parents[1] / "assets"

def mix(first, second, amount):
    channels = (
        round(int(first[i:i + 2], 16) * (1 - amount) + int(second[i:i + 2], 16) * amount)
        for i in (1, 3, 5)
    )
    return "#%02x%02x%02x" % tuple(channels)


def rounded_image(root, radius, fill, border, backdrop, *, symbol="", gradient=False):
    size = 2 * radius + 3
    middle = size // 2
    rows = []
    for y in range(size):
        row = []
        shade = mix(fill, ACCENT_HOVER, .37 * max(0, 1 - y / size)) if gradient else fill
        for x in range(size):
            dx = max(radius - x, x - (size - radius - 1), 0)
            dy = max(radius - y, y - (size - radius - 1), 0)
            distance = (dx * dx + dy * dy) ** .5
            edge = border if distance > radius - 1 else shade
            color = mix(backdrop, edge, max(0, min(1, radius + .5 - distance)))
            sx, sy = x - middle, y - middle
            if symbol == "check" and -4 <= sx <= 4 and abs(sy - (2 - abs(sx + 1))) < 1.2:
                color = backdrop
            row.append(color)
        rows.append("{" + " ".join(row) + "}")
    image = tk.PhotoImage(master=root, width=size, height=size)
    image.put(" ".join(rows))
    return image


def style_windows_frame(root):
    """Use the system caption buttons and window gestures with a dark DWM frame."""
    if sys.platform != "win32":
        return False, False
    import ctypes
    from ctypes import wintypes

    class HighContrast(ctypes.Structure):
        _fields_ = [("size", wintypes.UINT), ("flags", wintypes.DWORD), ("scheme", wintypes.LPWSTR)]

    user32 = ctypes.windll.user32
    contrast = HighContrast()
    contrast.size = ctypes.sizeof(contrast)
    user32.SystemParametersInfoW(0x0042, contrast.size, ctypes.byref(contrast), 0)
    animation = wintypes.BOOL(True)
    user32.SystemParametersInfoW(0x1042, 0, ctypes.byref(animation), 0)
    high_contrast = bool(contrast.flags & 1)
    if not high_contrast:
        root.update_idletasks()
        hwnd = wintypes.HWND(int(str(root.wm_frame()), 0))
        dwm = ctypes.windll.dwmapi
        for attribute, value in ((20, 1),):
            setting = wintypes.DWORD(value)
            dwm.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(setting), ctypes.sizeof(setting))
    return high_contrast, not bool(animation.value)


def replace_layout_elements(layout, replacements):
    """Preserve each theme's layout while swapping its decorative elements."""
    result = []
    for name, options in layout:
        options = options.copy()
        if "children" in options:
            options["children"] = replace_layout_elements(options["children"], replacements)
        result.append((replacements.get(name.rsplit(".", 1)[-1], name), options))
    return result


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
        self.folder_timeout = None
        self.scan_timeout = None
        self.browse_buttons = []

        root.title("WaveSpan")
        screen_w, screen_h = root.winfo_screenwidth(), root.winfo_screenheight()
        width, height = min(1060, screen_w - 24), min(804, screen_h - 88)
        root.geometry(f"{width}x{height}+{(screen_w - width) // 2}+{max(0, (screen_h - height - 64) // 2)}")
        root.minsize(min(width, 900), min(height, 716))
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(2, weight=1)
        self.high_contrast, self.reduce_motion = style_windows_frame(root)
        self.background = "SystemWindow" if self.high_contrast else BACKGROUND
        self.panel = "SystemWindow" if self.high_contrast else PANEL
        self.field = "SystemWindow" if self.high_contrast else FIELD
        root.configure(bg=self.background)
        self.configure_styles()
        self.header()
        self.workspace()
        self.activity()
        self.game_input.focus_set()
        root.after(70, self.poll_events)
        root.after(180, self.start_scan)

    def configure_styles(self):
        style = ttk.Style(self.root)
        # Keep native system colors in Windows contrast themes.
        if self.high_contrast:
            return
        style.theme_use("clam")
        self.skin_images = []
        self.root.option_add("*TCombobox*Listbox.background", FIELD)
        self.root.option_add("*TCombobox*Listbox.foreground", TEXT)
        self.root.option_add("*TCombobox*Listbox.selectBackground", SELECTION)
        self.root.option_add("*TCombobox*Listbox.selectForeground", TEXT)
        self.root.option_add("*TCombobox*Listbox.font", (FONT, 10))

        def element(name, radius, backdrop, states, *, gradient=False):
            images = []
            for state, fill, edge in states:
                image = rounded_image(self.root, radius, fill, edge, backdrop, gradient=gradient)
                self.skin_images.append(image)
                images.append((*state.split(), image) if state else image)
            style.element_create(name, "image", *images, border=radius + 1, padding=0, sticky="nsew")

        for primary in (False, True):
            name = "Primary" if primary else "Wave"
            fill = ACCENT if primary else PANEL
            edge = ACCENT if primary else BORDER
            element(f"{name}.button", 16, PANEL, [
                ("", fill, edge),
                ("disabled", FIELD, BORDER),
                ("pressed focus", "#62c4e1" if primary else FIELD, PERIWINKLE),
                ("pressed", "#62c4e1" if primary else FIELD, ACCENT),
                ("focus", fill, PERIWINKLE if primary else ACCENT),
                ("active", ACCENT_HOVER if primary else RAISED, ACCENT),
            ])
            style.layout(f"{name}.TButton", [(f"{name}.button", {
                "sticky": "nsew", "children": [("Button.padding", {
                    "sticky": "nsew", "children": [("Button.label", {"sticky": "nsew"})],
                })],
            })])
            style.configure(f"{name}.TButton", foreground=BACKGROUND if primary else TEXT,
                            font=(FONT, 10, "bold"), padding=(16, 8), anchor="center")
            style.map(f"{name}.TButton", foreground=[("disabled", MUTED)])

        element("Wave.field", 10, PANEL, [
            ("", FIELD, BORDER), ("disabled", FIELD, BORDER),
            ("focus", FIELD, ACCENT), ("active", RAISED, PERIWINKLE),
        ])
        for name in ("TEntry", "TCombobox"):
            layout = replace_layout_elements(
                style.layout(name), {"field": "Wave.field"},
            )
            style.layout(f"Wave.{name}", layout)
            style.configure(f"Wave.{name}", foreground=TEXT, insertcolor=TEXT,
                            selectbackground=SELECTION, selectforeground=TEXT, padding=(9, 6),
                            background=FIELD, fieldbackground=FIELD, bordercolor=BORDER,
                            lightcolor=FIELD, darkcolor=FIELD, arrowcolor=ACCENT)
            style.map(f"Wave.{name}", foreground=[("disabled", MUTED)],
                      background=[("active", RAISED)], arrowcolor=[("disabled", MUTED)])

        empty = rounded_image(self.root, 7, FIELD, BORDER, PANEL)
        focused = rounded_image(self.root, 7, FIELD, ACCENT, PANEL)
        selected = rounded_image(self.root, 7, ACCENT, ACCENT, PANEL, symbol="check")
        disabled = rounded_image(self.root, 7, BORDER, BORDER, PANEL, symbol="check")
        self.skin_images.extend((empty, focused, selected, disabled))
        style.element_create("Wave.indicator", "image", empty, ("disabled", "selected", disabled),
                             ("disabled", empty), ("selected", selected), ("focus", focused), ("active", focused),
                             padding=(0, 0, 7, 0), sticky="")
        check = replace_layout_elements(style.layout("TCheckbutton"), {"indicator": "Wave.indicator"})
        style.layout("Wave.TCheckbutton", check)
        style.configure("Wave.TCheckbutton", background=PANEL, foreground=MUTED, font=(FONT, 10),
                        focuscolor=ACCENT, focusthickness=1)
        style.map("Wave.TCheckbutton", foreground=[("disabled", MUTED), ("active", TEXT)])

        element("Wave.thumb", 4, FIELD, [
            ("", "#617691", "#617691"), ("pressed", ACCENT, ACCENT), ("active", PERIWINKLE, PERIWINKLE),
        ])
        style.layout("Wave.Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {
            "sticky": "ns", "children": [("Wave.thumb", {"expand": 1, "sticky": "ns"})],
        })])
        style.configure("Wave.Vertical.TScrollbar", background=FIELD, troughcolor=FIELD, bordercolor=FIELD,
                        lightcolor=FIELD, darkcolor=FIELD, arrowcolor=MUTED)

        element("Wave.trough", 4, PANEL, [("", FIELD, BORDER)])
        element("Wave.pbar", 4, FIELD, [("", ACCENT, ACCENT_HOVER)], gradient=True)
        style.layout("Wave.Horizontal.TProgressbar", [("Wave.trough", {
            "sticky": "ew", "children": [("Wave.pbar", {"side": "left", "sticky": "ns"})],
        })])

    def label(self, parent, text, size=10, color=TEXT, bold=False, **options):
        return tk.Label(parent, text=text, bg=parent.cget("bg"),
                        fg="SystemWindowText" if self.high_contrast else color, anchor="w", justify="left",
                        font=(FONT, size, "bold" if bold else "normal"), **options)

    def card(self, parent):
        if self.high_contrast:
            return tk.Frame(parent, bg="SystemWindow", highlightbackground="SystemWindowText", highlightthickness=1)
        return tk.Frame(parent, bg=PANEL, highlightbackground=BORDER, highlightthickness=1)

    def button(self, parent, label, callback, primary=False):
        return ttk.Button(parent, text=label, command=callback,
                          style="Primary.TButton" if primary else "Wave.TButton")

    def header(self):
        header = tk.Frame(self.root, bg=self.background)
        header.grid(row=0, column=0, padx=24, pady=(16, 13), sticky="ew")
        self.icon = tk.PhotoImage(master=self.root, file=ART / "wavespan-mark.png")
        self.mark = self.icon.subsample(2)
        self.root.iconphoto(True, self.icon, self.mark)
        tk.Label(header, image=self.mark, bg=self.background, borderwidth=0).pack(side="left", padx=(0, 11))
        self.label(header, "WaveSpan", 20, bold=True).pack(side="left")
        self.label(header, "Experimental runtime", color=WARNING, bold=True).pack(side="right")

    def workspace(self):
        main = tk.Frame(self.root, bg=self.background)
        main.grid(row=1, column=0, padx=24, sticky="ew")
        main.columnconfigure(1, weight=1)

        overview = self.card(main)
        overview.grid(row=0, column=0, sticky="nsew", padx=(0, 14))
        self.artwork = tk.PhotoImage(master=self.root, file=ART / "wavespan-hero.png")
        tk.Label(overview, image=self.artwork, bg=self.panel, borderwidth=0).pack(fill="x", padx=12, pady=(12, 0))
        notes = tk.Frame(overview, bg=self.panel)
        notes.pack(fill="x", padx=18, pady=(12, 15))
        self.label(notes, "Xbox Series to Windows", 16, bold=True).pack(fill="x")
        self.label(notes, "Translate shaders, export assets, and build the Windows runtime from your own package.",
                   color=MUTED, wraplength=300).pack(fill="x", pady=(5, 0))
        self.label(notes, "Retail gameplay is unavailable.", color=WARNING).pack(fill="x", pady=(10, 0))

        setup = self.card(main)
        setup.grid(row=0, column=1, sticky="nsew")
        setup.columnconfigure(0, weight=1)
        setup.rowconfigure(0, weight=1)
        content = tk.Frame(setup, bg=self.panel)
        content.grid(row=0, column=0, sticky="nsew", padx=18, pady=14)
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

        actions = tk.Frame(content, bg=self.panel)
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
        toolbar = tk.Frame(panel, bg=self.panel)
        toolbar.grid(row=0, column=0, sticky="ew", padx=14, pady=(8, 6))
        self.label(toolbar, "Build activity", 13, bold=True).pack(side="left")
        self.stop_button = self.button(toolbar, "Stop", self.stop)
        self.stop_button.pack(side="right")
        self.stop_button.state(["disabled"])
        self.status_label = self.label(toolbar, "", color=ACCENT, bold=True, textvariable=self.status)
        self.status_label.pack(side="right", padx=(0, 10))
        self.progress = ttk.Progressbar(panel, mode="indeterminate", style="Wave.Horizontal.TProgressbar")
        self.progress.grid(row=1, column=0, sticky="ew", padx=14)
        panel.rowconfigure(1, minsize=self.progress.winfo_reqheight())
        self.progress.grid_remove()

        console = tk.Frame(panel, bg=self.field, highlightthickness=1,
                           highlightbackground="SystemWindowText" if self.high_contrast else BORDER)
        console.grid(row=2, column=0, sticky="nsew", padx=14, pady=(7, 12))
        console.columnconfigure(0, weight=1)
        console.rowconfigure(0, weight=1)
        log_text = "SystemWindowText" if self.high_contrast else MUTED
        self.log = tk.Text(console, bg=self.field, fg=log_text, font=("Consolas", 10), borderwidth=0,
                           height=3, wrap="word", padx=10, pady=8, spacing1=2, spacing3=2, state="disabled", highlightthickness=0,
                           selectbackground="SystemHighlight" if self.high_contrast else SELECTION,
                           selectforeground="SystemHighlightText" if self.high_contrast else TEXT)
        scrollbar = ttk.Scrollbar(console, orient="vertical", style="Wave.Vertical.TScrollbar", command=self.log.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scrollbar.set)
        self.log.grid(row=0, column=0, sticky="nsew")
        self.log.tag_configure("accent", foreground="SystemWindowText" if self.high_contrast else ACCENT)
        self.log.tag_configure("error", foreground="SystemWindowText" if self.high_contrast else ERROR)
        self.write_log("Choose a game package to check its files or start a build.\n")

    def write_log(self, text, tag=""):
        self.log.configure(state="normal")
        at_bottom = self.log.yview()[1] == 1.0
        self.log.insert("end", text, tag)
        lines = int(self.log.index("end-1c").split(".")[0])
        if lines > 5000:
            self.log.delete("1.0", f"{lines - 4500}.0")
        if at_bottom:
            self.log.see("end")
        self.log.configure(state="disabled")

    def browse(self, label, variable):
        current = variable.get()
        path = filedialog.askdirectory(parent=self.root, title=f"Choose {label.lower().split(' (')[0]}",
                                       initialdir=current or str(Path.home()))
        if path:
            variable.set(os.path.normpath(path))

    def start_scan(self):
        if self.scanning or self.process is not None:
            return
        self.scanning = True
        self.scan_button.state(["disabled"])
        self.scan_button.configure(text="Searching…")
        self.scan_timeout = self.root.after(15000, self.scan_slow)
        threading.Thread(target=self.scan, daemon=True).start()

    def scan_slow(self):
        self.scan_timeout = None
        self.write_log("Package search is taking longer than usual. You can still use Browse.\n")

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
        if self.process is not None or self.folder_timeout is not None:
            return
        game = self.game.get().strip().strip('"')
        output = self.output.get().strip().strip('"')
        work = self.work.get().strip().strip('"')
        if not game:
            messagebox.showinfo("WaveSpan", "Choose the folder containing MicrosoftGame.Config and the .rpf archives.",
                                parent=self.root)
            return
        if not check_only and not output:
            messagebox.showinfo("WaveSpan", "Choose a build destination.", parent=self.root)
            return

        request = object()
        self.folder_request = request
        self.folder_timeout = self.root.after(15000, self.folder_check_expired)
        self.set_status("Checking folder")
        threading.Thread(target=self.validate_folder, args=(request, game, output, work, check_only, self.full_check.get()),
                         daemon=True).start()

    def folder_check_expired(self):
        self.folder_timeout = None
        self.set_status("Folder unavailable", WARNING)
        self.write_log("The package folder did not respond. Check the path or use Browse.\n", "error")

    def validate_folder(self, request, game, output, work, check_only, full_check):
        self.events.put(("folder", (request, os.path.isdir(game), game, output, work, check_only, full_check)))

    def launch_build(self, request, exists, game, output, work, check_only, full_check):
        if self.folder_timeout is None or request is not self.folder_request:
            return
        self.root.after_cancel(self.folder_timeout)
        self.folder_timeout = None
        if not exists:
            self.set_status("Ready")
            messagebox.showinfo("WaveSpan", "Choose the folder containing MicrosoftGame.Config and the .rpf archives.",
                                parent=self.root)
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
        if full_check:
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
        self.status_label.configure(fg="SystemWindowText" if self.high_contrast else color)

    def set_busy(self, busy):
        for widget in self.form_widgets:
            widget.state(["disabled" if busy else "!disabled"])
        self.scan_button.state(["disabled" if busy or self.scanning else "!disabled"])
        self.stop_button.state(["!disabled" if busy else "disabled"])
        if busy:
            self.progress.grid()
            if self.reduce_motion:
                self.progress.configure(value=34)
            else:
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
            self.set_status(status, SUCCESS)
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
            if event in ("scan", "scan_error") and self.scan_timeout is not None:
                self.root.after_cancel(self.scan_timeout)
                self.scan_timeout = None
            if event == "folder":
                self.launch_build(*value)
            elif event == "scan":
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
            if self.process is None:
                self.root.destroy()
            else:
                self.close_pending = self.stop()


def open_launcher(builder, default_out, find_dumps):
    if sys.platform == "win32":
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("WaveSpan.Launcher")
    root = tk.Tk()
    Launcher(root, builder, default_out, find_dumps)
    root.mainloop()


def run_windows_builder():
    """Receive Stop over a private pipe and deliver Ctrl+C to the builder's console."""
    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel = ctypes.windll.kernel32
    if not kernel.GetConsoleCP():
        # The desktop EXE starts the worker without a console window.
        if not kernel.AllocConsole():
            raise OSError("Could not allocate a private builder console")
        kernel.GetConsoleWindow.restype = wintypes.HWND
        ctypes.windll.user32.ShowWindow(kernel.GetConsoleWindow(), 0)
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
