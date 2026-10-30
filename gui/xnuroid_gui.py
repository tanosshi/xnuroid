"""xnuroid helper: a point-and-click front end for turning a decrypted iOS game into an Android app.

Run with `python gui/xnuroid_gui.py`, or the packaged xnuroid-helper.exe placed in the project folder.
Dropping an .ipa onto the exe opens it straight away.
"""
from __future__ import annotations

import base64
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
import plistlib
import webbrowser
import zipfile
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

sys.path.insert(0, str(Path(__file__).resolve().parent))
import environment as env  # noqa: E402
import game as games  # noqa: E402

SETTINGS = Path(os.environ.get("APPDATA", Path.home())) / "xnuroid" / "helper.json"
PHONE_POLL_SECONDS = 3
CRASH_WATCH_SECONDS = 10

BG = "#f4f1f8"
CARD = "#ffffff"
INK = "#1d1b20"
MUTED = "#625b71"
ACCENT = "#6750a4"
ACCENT_DARK = "#4f378b"
GOOD = "#1b7f3b"
BAD = "#b3261e"
WARN = "#8a5a00"
FONT = "Segoe UI"

BUILD_STAGES = [
    ("assets:", 0.80, "Packing the game into the app"),
    ("built build/xnuroid.apk", 0.88, "The app is built"),
    ("Performing Streamed Install", 0.92, "Installing it on your phone"),
    ("Success", 0.97, "Installed"),
    ("Starting: Intent", 1.00, "Starting the game on your phone"),
]

BUILD_HINTS = [
    ("connect the arm64 device once", "Plug in your phone the first time you build. The build copies one file from it."),
    ("INSTALL_FAILED_UPDATE_INCOMPATIBLE",
     "An older copy of this game, signed on another PC, is on the phone. Use 'Remove from phone', then try again."),
    ("INSTALL_FAILED_INSUFFICIENT_STORAGE", "Your phone is out of space. Free some space and try again."),
    ("INSTALL_FAILED_USER_RESTRICTED",
     "Your phone blocked the install. On Xiaomi/Redmi/POCO phones turn on 'Install via USB' in Developer options."),
    ("INSTALL_FAILED_NO_MATCHING_ABIS", "This phone is not a 64-bit ARM phone, so the app cannot run on it."),
    ("debug.keystore", "The app signing key is missing. Use the 'Fix' button next to 'App signing key'."),
    ("python: command not found", "Git Bash cannot find Python. Reinstall Python and tick 'Add python.exe to PATH'."),
    ("no devices/emulators found", "The phone disconnected during the build. Plug it back in and try again."),
    ("device unauthorized", "Tap 'Allow' on the USB debugging prompt on your phone, then try again."),
    ("No space left on device", "This PC's disk is full. Free some space and try again."),
    ("clang", "The C++ compiler failed. Check that NDK " + env.NDK_VERSION + " is installed."),
]


def load_settings() -> dict:
    try:
        return json.loads(SETTINGS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_settings(values: dict) -> None:
    try:
        SETTINGS.parent.mkdir(parents=True, exist_ok=True)
        SETTINGS.write_text(json.dumps(values, indent=2), encoding="utf-8")
    except OSError:
        pass


def guess_project() -> Path | None:
    here = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
    starts = [here, Path.cwd()]
    saved = load_settings().get("project")
    if saved:
        starts.insert(0, Path(saved))
    for start in starts:
        for folder in [start, *start.parents][:4]:
            if env.is_project_root(folder):
                return folder
    return None


@dataclass
class Rejected:
    choice: Path
    report: object
    message: str = ""


class Job:
    def __init__(self, post):
        self.post = post
        self.cancelled = threading.Event()
        self.proc: subprocess.Popen | None = None

    def log(self, text: str) -> None:
        self.post("log", text)

    def progress(self, fraction: float | None, text: str) -> None:
        self.post("progress", (fraction, text))

    def stream(self, cmd: list, cwd: Path, extra_env: dict | None = None, stdin_text: str = "") -> tuple[int, str]:
        self.log("$ " + " ".join(str(c) for c in cmd))
        self.proc = subprocess.Popen(
            [str(c) for c in cmd], cwd=cwd, env={**os.environ, **(extra_env or {})},
            stdin=subprocess.PIPE if stdin_text else subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
            creationflags=env.NO_WINDOW)
        if stdin_text:
            try:
                self.proc.stdin.write(stdin_text)
                self.proc.stdin.close()
            except OSError:
                pass
        lines = []
        for line in self.proc.stdout:
            line = line.rstrip("\r\n")
            lines.append(line)
            self.log(line)
            self.post("line", line)
        code = self.proc.wait()
        self.proc = None
        return code, "\n".join(lines)

    def kill(self) -> None:
        self.cancelled.set()
        proc = self.proc
        if proc and proc.poll() is None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, creationflags=env.NO_WINDOW)


class Card(tk.Frame):
    def __init__(self, parent, number: str, title: str):
        super().__init__(parent, bg=CARD, highlightbackground="#e7e0ec", highlightthickness=1, padx=18, pady=14)
        head = tk.Frame(self, bg=CARD)
        head.pack(fill="x")
        tk.Label(head, text=number, bg=ACCENT, fg="white", font=(FONT, 11, "bold"), width=2).pack(side="left")
        tk.Label(head, text=title, bg=CARD, fg=INK, font=(FONT, 13, "bold")).pack(side="left", padx=10)
        self.badge = tk.Label(head, bg=CARD, font=(FONT, 10, "bold"))
        self.badge.pack(side="right")
        self.body = tk.Frame(self, bg=CARD)
        self.body.pack(fill="x", pady=(10, 0))

    def set_badge(self, text: str, color: str) -> None:
        self.badge.config(text=text, fg=color)


class Helper(tk.Tk):
    def __init__(self, project: Path, start_with: Path | None):
        super().__init__()
        self.project = project
        self.settings = load_settings()
        self.settings["project"] = str(project)
        save_settings(self.settings)
        self.events: queue.Queue = queue.Queue()
        self.stop = threading.Event()
        self.job: Job | None = None
        self.queued: list[tuple] = []
        self.found: dict = {}
        self.checks: list[env.Check] = []
        self.adb: env.Adb | None = None
        self.phones: list[env.Phone] = []
        self.game: games.Game | None = None
        self.package = ""
        self.encryption_ok: bool | None = None
        self.forced = False
        self.rejected: Path | None = None
        self.icon_image: tk.PhotoImage | None = None
        self.log_lines: list[str] = []
        self.status_is_hint = False

        self.title("xnuroid helper")
        self.configure(bg=BG)
        self.scale = self.winfo_fpixels("1i") / 96
        self._style()
        self._layout()
        self.update_idletasks()
        self.minsize(self.winfo_reqwidth(), min(self.winfo_reqheight(), self.winfo_screenheight() - 80))
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(100, self.drain)
        self.refresh_computer()
        self.load_saved_game()
        threading.Thread(target=self.poll_phones, daemon=True).start()
        if start_with:
            self.after(300, lambda: self.choose(start_with))

    # ------------------------------------------------------------------ layout

    def _style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        def flat(name, font, fg, bg, hover, disabled, padding):
            style.configure(name, font=font, foreground=fg, background=bg, lightcolor=bg, darkcolor=bg,
                            bordercolor=bg, focuscolor=bg, borderwidth=0, padding=padding)
            states = [("disabled", disabled), ("pressed", hover), ("active", hover)]
            style.map(name, background=states, lightcolor=states, darkcolor=states, bordercolor=states,
                      foreground=[("disabled", "#8d8794")])

        flat("Accent.TButton", (FONT, 12, "bold"), "white", ACCENT, ACCENT_DARK, "#ddd8e3", (22, 12))
        flat("Soft.TButton", (FONT, 10), ACCENT_DARK, "#e8def8", "#d6c8f5", "#eeeaf2", (14, 7))
        flat("Fix.TButton", (FONT, 9, "bold"), "white", BAD, "#8c1d18", "#ddd8e3", (10, 3))
        style.configure("Horizontal.TProgressbar", troughcolor="#e7e0ec", background=ACCENT, borderwidth=0,
                        thickness=8)
        style.configure("TCombobox", padding=4)

    def px(self, value: int) -> int:
        return round(value * self.scale)

    def _label(self, parent, text="", size=10, color=INK, bold=False, **kwargs) -> tk.Label:
        return tk.Label(parent, text=text, bg=parent["bg"], fg=color, justify="left", anchor="w",
                        font=(FONT, size, "bold" if bold else "normal"), **kwargs)

    def _layout(self) -> None:
        outer = tk.Frame(self, bg=BG, padx=22, pady=16)
        outer.pack(fill="both", expand=True)

        self._label(outer, "Play your iOS game on Android", 18, bold=True).pack(fill="x")
        self._label(outer, "Follow the three steps. Anything with a red button needs your attention.",
                    10, MUTED).pack(fill="x", pady=(2, 12))

        self.computer_card = Card(outer, "1", "Your computer")
        self.computer_card.pack(fill="x")
        self.check_rows = tk.Frame(self.computer_card.body, bg=CARD)
        self.check_rows.pack(fill="x")

        self.phone_card = Card(outer, "2", "Your phone")
        self.phone_card.pack(fill="x", pady=10)
        self.phone_text = self._label(self.phone_card.body, "Looking for your phone…", 11, bold=True)
        self.phone_text.pack(fill="x")
        self.phone_help = self._label(self.phone_card.body, "", 10, MUTED, wraplength=self.px(780))
        self.phone_help.pack(fill="x", pady=(2, 0))
        self.phone_pick = ttk.Combobox(self.phone_card.body, state="readonly", width=50)
        self.phone_pick.bind("<<ComboboxSelected>>", lambda _e: self.update_actions())

        self.game_card = Card(outer, "3", "Your game")
        self.game_card.pack(fill="x")
        row = tk.Frame(self.game_card.body, bg=CARD)
        row.pack(fill="x")
        self.icon_label = tk.Label(row, bg="#efe9f5", width=9, height=4)
        self.icon_label.pack(side="left")
        text = tk.Frame(row, bg=CARD)
        text.pack(side="left", fill="x", expand=True, padx=14)
        self.game_title = self._label(text, "No game chosen yet", 13, bold=True)
        self.game_title.pack(fill="x")
        self.game_detail = self._label(text, "Pick a decrypted .ipa file, or a folder you already unpacked.",
                                       10, MUTED, wraplength=self.px(520))
        self.game_detail.pack(fill="x")
        self.game_crypt = self._label(text, "", 10, bold=True, wraplength=self.px(520))
        self.game_crypt.pack(fill="x", pady=(4, 0))
        pick = tk.Frame(row, bg=CARD)
        pick.pack(side="right")
        self.pick_ipa = ttk.Button(pick, text="Choose .ipa file…", style="Soft.TButton", command=self.ask_ipa)
        self.pick_ipa.pack(fill="x")
        self.pick_folder = ttk.Button(pick, text="Choose folder…", style="Soft.TButton", command=self.ask_folder)
        self.pick_folder.pack(fill="x", pady=(6, 0))
        self.override_button = ttk.Button(pick, text="Use it anyway…", style="Fix.TButton",
                                          command=self.use_anyway)

        actions = tk.Frame(outer, bg=BG)
        actions.pack(fill="x", pady=(16, 0))
        self.go_button = ttk.Button(actions, text="Put the game on my phone", style="Accent.TButton",
                                    command=lambda: self.build(install=True))
        self.go_button.pack(side="left")
        self.cancel_button = ttk.Button(actions, text="Cancel", style="Soft.TButton", command=self.cancel)
        more = tk.Frame(actions, bg=BG)
        more.pack(side="right")
        self.more_buttons = {
            "start": ttk.Button(more, text="Start game", style="Soft.TButton", command=self.start_game),
            "logs": ttk.Button(more, text="What went wrong?", style="Soft.TButton", command=self.show_phone_log),
            "apk": ttk.Button(more, text="Only build the APK", style="Soft.TButton",
                              command=lambda: self.build(install=False)),
            "save": ttk.Button(more, text="Save APK…", style="Soft.TButton", command=self.save_apk),
            "remove": ttk.Button(more, text="Remove from phone", style="Soft.TButton", command=self.uninstall),
        }
        for index, key in enumerate(("start", "logs", "remove", "apk", "save")):
            self.more_buttons[key].grid(row=index // 3, column=index % 3, padx=3, pady=3, sticky="ew")

        status = tk.Frame(outer, bg=BG)
        status.pack(fill="x", pady=(12, 0))
        self.status_text = self._label(status, "", 10, MUTED)
        self.status_text.pack(fill="x")
        self.progress = ttk.Progressbar(status, mode="determinate", maximum=1.0)
        self.progress.pack(fill="x", pady=(4, 0))

        toggle_row = tk.Frame(outer, bg=BG)
        toggle_row.pack(fill="x", pady=(8, 0))
        self.toggle = tk.Label(toggle_row, text="▸ Show details", bg=BG, fg=ACCENT, cursor="hand2",
                               font=(FONT, 10, "underline"))
        self.toggle.pack(side="left")
        self.toggle.bind("<Button-1>", lambda _e: self.toggle_details())
        self.copy_log = tk.Label(toggle_row, text="Copy details", bg=BG, fg=ACCENT, cursor="hand2",
                                 font=(FONT, 10, "underline"))
        self.copy_log.bind("<Button-1>", lambda _e: self.copy_details())
        self.details = ScrolledText(outer, height=12, font=("Consolas", 9), bg="#1d1b20", fg="#e6e0e9",
                                    insertbackground="white", relief="flat", state="disabled")

    def toggle_details(self) -> None:
        if self.details.winfo_ismapped():
            self.details.pack_forget()
            self.copy_log.pack_forget()
            self.toggle.config(text="▸ Show details")
        else:
            self.details.pack(fill="both", expand=True, pady=(4, 0))
            self.copy_log.pack(side="right")
            self.toggle.config(text="▾ Hide details")

    def copy_details(self) -> None:
        self.clipboard_clear()
        self.clipboard_append("\n".join(self.log_lines))
        self.set_status("Details copied. Paste them wherever you ask for help.", GOOD)

    # ------------------------------------------------------------------ events

    def post(self, kind: str, payload=None) -> None:
        self.events.put((kind, payload))

    def drain(self) -> None:
        pending_log = []
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    pending_log.append(payload)
                elif kind == "call":
                    payload()
                elif kind == "progress":
                    self.show_progress(*payload)
                elif kind == "line":
                    self.on_build_line(payload)
                elif kind == "phones":
                    self.show_phones(payload)
        except queue.Empty:
            pass
        if pending_log:
            self.append_log(pending_log)
        if not self.stop.is_set():
            self.after(80, self.drain)

    def append_log(self, lines: list[str]) -> None:
        self.log_lines.extend(lines)
        self.details.config(state="normal")
        self.details.insert("end", "\n".join(lines) + "\n")
        if int(self.details.index("end-1c").split(".")[0]) > 5000:
            self.details.delete("1.0", "1000.0")
        self.details.see("end")
        self.details.config(state="disabled")

    def set_status(self, text: str, color: str = MUTED, hint: bool = False) -> None:
        self.status_text.config(text=text, fg=color)
        self.status_is_hint = hint

    def show_progress(self, fraction: float | None, text: str) -> None:
        if fraction is None:
            if str(self.progress["mode"]) != "indeterminate":
                self.progress.config(mode="indeterminate")
                self.progress.start(12)
        else:
            if str(self.progress["mode"]) != "determinate":
                self.progress.stop()
                self.progress.config(mode="determinate")
            self.progress["value"] = fraction
        if text:
            self.set_status(text)

    def run_job(self, title: str, work, done=None) -> None:
        if self.job:
            self.queued.append((title, work, done))
            return
        job = Job(self.post)
        self.job = job
        self.show_progress(None, title)
        self.update_actions()

        def worker():
            try:
                result, error = work(job), None
            except Exception as exc:  # shown to the user, never swallowed
                result, error = None, exc
            self.post("call", lambda: self.finish_job(job, result, error, done))

        threading.Thread(target=worker, daemon=True).start()

    def finish_job(self, job: Job, result, error, done) -> None:
        self.job = None
        self.show_progress(0.0, "")
        self.set_status("")
        if job.cancelled.is_set():
            self.set_status("Cancelled.", WARN)
        elif error is not None:
            self.set_status(str(error).splitlines()[0], BAD)
            messagebox.showerror("Something went wrong", str(error), parent=self)
        elif done:
            done(result)
        if self.queued and not self.job:
            self.run_job(*self.queued.pop(0))
        self.update_actions()

    def cancel(self) -> None:
        if self.job:
            self.job.kill()

    def on_close(self) -> None:
        if self.job and not messagebox.askyesno(
                "Still working", "I am still working. Stop and close anyway?", parent=self):
            return
        if self.job:
            self.job.kill()
        self.stop.set()
        self.destroy()

    # ------------------------------------------------------------------ step 1: computer

    def refresh_computer(self) -> None:
        def work(_job):
            return env.check_computer()

        def done(result):
            self.checks, self.found = result
            adb = self.found.get("adb")
            self.adb = env.Adb(adb) if adb and Path(adb).is_file() else None
            self.show_checks()

        self.run_job("Checking your computer…", work, done)

    def show_checks(self) -> None:
        for child in self.check_rows.winfo_children():
            child.destroy()
        for check in self.checks:
            row = tk.Frame(self.check_rows, bg=CARD)
            row.pack(fill="x", pady=1)
            self._label(row, "✓" if check.ok else "✕", 11, GOOD if check.ok else BAD, bold=True,
                        width=2).pack(side="left")
            self._label(row, check.title, 10, bold=True, width=22).pack(side="left")
            self._label(row, check.detail, 10, MUTED).pack(side="left", fill="x", expand=True)
            if check.fix:
                ttk.Button(row, text="Fix", style="Fix.TButton",
                           command=lambda c=check: self.fix(c)).pack(side="right")
        refresh = tk.Label(self.check_rows, text="Check again", bg=CARD, fg=ACCENT, cursor="hand2",
                           font=(FONT, 9, "underline"))
        refresh.pack(anchor="e", pady=(4, 0))
        refresh.bind("<Button-1>", lambda _e: self.refresh_computer())
        missing = sum(not c.ok for c in self.checks)
        self.computer_card.set_badge("Ready ✓" if not missing else f"{missing} to fix", GOOD if not missing else BAD)
        if self.adb is None:
            self.phone_text.config(text="Finish step 1 first", fg=MUTED)
            self.phone_help.config(text="I need Android Studio's Platform-Tools to talk to your phone.")
            self.phone_card.set_badge("Waiting", WARN)
        self.update_actions()

    def fix(self, check: env.Check) -> None:
        if check.fix == "git":
            messagebox.showinfo("Install Git for Windows",
                                "Your browser will open the Git download page.\n\nRun the installer and keep "
                                "every default option. Then come back and press 'Check again'.", parent=self)
            webbrowser.open(env.GIT_URL)
        elif check.fix == "python":
            messagebox.showinfo("Install Python",
                                "Your browser will open the Python download page.\n\nIMPORTANT: on the first "
                                "installer screen, tick 'Add python.exe to PATH'. Then come back and press "
                                "'Check again'.", parent=self)
            webbrowser.open(env.PYTHON_URL)
        elif check.fix == "studio":
            messagebox.showinfo("Install Android Studio",
                                "Your browser will open the Android Studio page.\n\nInstall it, open it once and "
                                "let it finish its setup wizard. Then come back and press 'Check again'.",
                                parent=self)
            webbrowser.open(env.STUDIO_URL)
        elif check.fix == "sdk_parts":
            self.install_sdk_parts()
        elif check.fix == "keystore":
            self.run_job("Creating the app signing key…", lambda _job: env.create_debug_keystore(),
                         lambda _path: self.refresh_computer())

    def install_sdk_parts(self) -> None:
        sdk = self.found.get("sdk")
        manager = env.find_sdkmanager(sdk) if sdk else None
        if manager is None:
            messagebox.showinfo(
                "Install the missing Android parts",
                "Open Android Studio and choose More Actions > SDK Manager.\n\n"
                "1. On the 'SDK Platforms' tab tick Android 16 (API 36).\n"
                "2. On the 'SDK Tools' tab tick 'Show Package Details', then tick:\n"
                f"     • NDK (Side by side) > {env.NDK_VERSION}\n"
                "     • Android SDK Build-Tools > 36.1.0\n"
                "     • Android SDK Command-line Tools\n"
                "     • Android SDK Platform-Tools\n"
                "3. Press OK and wait for the download.\n\n"
                "Then come back and press 'Check again'.", parent=self)
            return
        if not messagebox.askokcancel(
                "Install the missing Android parts",
                "I will download the missing Android parts now (about 1 GB) and accept Google's SDK "
                "licences for you.\n\nContinue?", parent=self):
            return

        keytool = env.find_keytool()
        java = {"JAVA_HOME": str(keytool.parent.parent)} if keytool and not os.environ.get("JAVA_HOME") else {}

        def work(job: Job):
            code, _ = job.stream([manager, "--install", *env.SDK_PACKAGES], sdk, java, stdin_text="y\n" * 30)
            if code != 0 and not job.cancelled.is_set():
                raise RuntimeError("The Android download failed. Open 'Show details' to see why.")

        self.run_job("Downloading Android parts. This can take a while…", work, lambda _r: self.refresh_computer())

    # ------------------------------------------------------------------ step 2: phone

    def poll_phones(self) -> None:
        last = None
        while not self.stop.is_set():
            adb = self.adb
            if adb is not None:
                try:
                    phones = adb.phones()
                except (OSError, subprocess.TimeoutExpired):
                    phones = []
                key = [(p.serial, p.state, p.abi, p.sdk) for p in phones]
                if key != last:
                    last = key
                    self.post("phones", phones)
            self.stop.wait(PHONE_POLL_SECONDS)

    def show_phones(self, phones: list[env.Phone]) -> None:
        self.phones = phones
        if not phones:
            self.phone_text.config(text="No phone found", fg=BAD)
            self.phone_help.config(text=(
                "1. Plug your phone into this PC with a USB cable.\n"
                "2. On the phone open Settings > About phone and tap 'Build number' 7 times.\n"
                "3. Open Settings > Developer options and turn on 'USB debugging'.\n"
                "4. Tap 'Allow' when the phone asks about USB debugging.\n"
                "This page updates by itself when the phone appears."))
            self.phone_card.set_badge("Waiting", WARN)
        if len(phones) > 1:
            current = self.phone_pick.get()
            values = [p.label for p in phones]
            self.phone_pick.config(values=values)
            self.phone_pick.set(current if current in values else values[0])
            self.phone_pick.pack(anchor="w", pady=(6, 0))
        else:
            self.phone_pick.pack_forget()
        if phones:
            self.show_selected_phone()
        self.update_actions()

    def selected_phone(self) -> env.Phone | None:
        if len(self.phones) > 1:
            label = self.phone_pick.get()
            return next((p for p in self.phones if p.label == label), self.phones[0])
        return self.phones[0] if self.phones else None

    def show_selected_phone(self) -> None:
        phone = self.selected_phone()
        if phone is None:
            return
        problem = phone.problem()
        self.phone_text.config(text=phone.label, fg=INK if not problem else BAD)
        self.phone_help.config(text=problem or "Connected and ready.")
        self.phone_card.set_badge("Ready ✓" if not problem else "Needs attention", GOOD if not problem else BAD)

    # ------------------------------------------------------------------ step 3: game

    def load_saved_game(self) -> None:
        saved = games.read_config(self.project)
        if saved:
            self.forced = self.settings.get("forced_app") == str(saved.app)
            self.set_game(saved, None)
            self.check_saved_encryption(saved)

    def check_saved_encryption(self, game: games.Game) -> None:
        def work(_job):
            return games.check_encryption(game.app, game.app)

        self.run_job("Checking that the game is decrypted…", work, self.show_encryption)

    def ask_ipa(self) -> None:
        path = filedialog.askopenfilename(parent=self, title="Choose your decrypted .ipa file",
                                          initialdir=self.settings.get("last_dir") or str(Path.home()),
                                          filetypes=[("iOS app", "*.ipa"), ("All files", "*.*")])
        if path:
            self.choose(Path(path))

    def ask_folder(self) -> None:
        path = filedialog.askdirectory(parent=self, title="Choose the unpacked game folder (or the .app folder)",
                                       initialdir=self.settings.get("last_dir") or str(Path.home()))
        if path:
            self.choose(Path(path))

    def choose(self, choice: Path, force: bool = False) -> None:
        self.settings["last_dir"] = str(choice.parent)
        save_settings(self.settings)
        games_dir = self.project / "games"

        if choice.is_file():
            if choice.suffix.lower() != ".ipa":
                self.reject(choice, None, f"✕ {choice.name} is not an .ipa file.", allow_override=False)
                return

            def work(job: Job):
                job.progress(None, "Checking that the game is decrypted…")
                try:
                    report = games.check_encryption(choice, None)
                except (ValueError, OSError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
                    if not force:
                        return Rejected(choice, None, f"✕ I could not read this file as an iOS app ({error}).")
                    report = None
                if not force and not report.usable:
                    return Rejected(choice, report)
                root = games.extract_ipa(choice, games_dir, job.progress)
                candidates = games.bundle_candidates(root / "Payload")
                if not candidates:
                    raise ValueError(f"There is no app inside {choice.name}.")
                app = self.pick_bundle(candidates)
                if app is None:
                    job.cancelled.set()
                return report, app

            self.run_job("Checking that the game is decrypted…", work, lambda r: self.after_pick(r, force))
        elif choice.is_dir():
            candidates = games.bundle_candidates(choice)
            if not candidates:
                self.reject(choice, None, "✕ There is no iOS app in this folder. Pick the folder that contains "
                                          "'Payload', the 'Payload' folder itself, or a folder ending in '.app'.",
                            allow_override=False)
                return

            def work(job: Job):
                app = self.pick_bundle(candidates)
                if app is None:
                    job.cancelled.set()
                    return None, None
                report = games.check_encryption(app, app)
                if not force and not report.usable:
                    return Rejected(app, report)
                root = games.adopt_loose_app(app, games_dir, job.progress)
                return report, root / "Payload" / app.name

            self.run_job("Checking that the game is decrypted…", work, lambda r: self.after_pick(r, force))
        else:
            messagebox.showerror("Not found", f"There is nothing at\n{choice}", parent=self)

    def pick_bundle(self, candidates: list[Path]) -> Path | None:
        """Called from a worker; asks on the UI thread when the file holds several apps."""
        if len(candidates) == 1:
            return candidates[0]
        answer: dict = {}
        ready = threading.Event()

        def ask():
            dialog = tk.Toplevel(self, bg=CARD, padx=20, pady=16)
            dialog.title("Which app?")
            dialog.transient(self)
            dialog.grab_set()
            self._label(dialog, "This contains more than one app. Which one is the game?", 11, bold=True).pack()
            listbox = tk.Listbox(dialog, height=min(8, len(candidates)), font=(FONT, 10))
            for app in candidates:
                listbox.insert("end", app.stem)
            listbox.selection_set(0)
            listbox.pack(fill="x", pady=10)

            def close(choice):
                answer["app"] = choice
                dialog.destroy()
                ready.set()

            ttk.Button(dialog, text="Use this one", style="Accent.TButton",
                       command=lambda: close(candidates[listbox.curselection()[0]] if listbox.curselection()
                                             else candidates[0])).pack()
            dialog.protocol("WM_DELETE_WINDOW", lambda: close(None))

        self.post("call", ask)
        ready.wait()
        return answer.get("app")

    def after_pick(self, result, force: bool = False) -> None:
        if isinstance(result, Rejected):
            self.reject(result.choice, result.report, result.message)
            return
        report, app = result
        if app is None:
            return
        try:
            game = games.Game(app.parent.parent, app, games.load_info(app))
        except ValueError as error:
            self.reject(app, report, f"✕ {error}", allow_override=False)
            return
        games.write_config(self.project, game)
        self.forced = force
        self.settings["forced_app"] = str(app) if force else ""
        save_settings(self.settings)
        self.set_game(game, report)
        if force:
            self.set_status(f"'{game.label}' is set up anyway. It might not work.", WARN)
        else:
            self.set_status(f"'{game.label}' is ready. Press 'Put the game on my phone'.", GOOD)

    def reject(self, choice: Path, report, message: str = "", allow_override: bool = True) -> None:
        """Shows what was picked and why it was refused, instead of leaving the previous game on screen."""
        self.game = None
        self.package = ""
        self.forced = False
        self.rejected = choice if allow_override else None
        self.icon_image = None
        self.icon_label.config(image="", width=9, height=4)
        self.game_title.config(text=choice.name)
        self.game_detail.config(text=f"This cannot be used as it is.\n{choice}")
        if report is not None and not message:
            self.show_encryption(report)
        else:
            self.encryption_ok = False
            self.game_crypt.config(fg=BAD, text=message)
            self.game_card.set_badge("Not usable", BAD)
        if allow_override:
            self.override_button.pack(fill="x", pady=(6, 0))
        else:
            self.override_button.pack_forget()
        self.set_status("That game cannot be used. Choose another one"
                        + (", or press 'Use it anyway…'." if allow_override else "."), BAD)
        self.update_actions()

    def use_anyway(self) -> None:
        choice = self.rejected
        if choice is None:
            return
        if not messagebox.askyesno(
                "Use it anyway?",
                "The check says this app is encrypted, unreadable or not 64-bit ARM, so it will most likely "
                "not work. The check can be wrong, though.\n\nUse it anyway?", icon="warning", parent=self):
            return
        self.choose(choice, force=True)


    def set_game(self, game: games.Game, report) -> None:
        self.game = game
        self.package = games.package_id(self.project, game.info)
        kind = "Unity game" if game.is_unity else "Regular iOS app (app mode, experimental)"
        version = f"  ·  version {game.version}" if game.version else ""
        self.game_title.config(text=game.label)
        self.game_detail.config(text=f"{kind}{version}\nAndroid app id: {self.package}\n{game.app}")
        icon = games.icon_png(self.project, game)
        self.icon_image = None
        if icon:
            try:
                image = tk.PhotoImage(data=base64.b64encode(icon))
                factor = max(1, image.width() // self.px(72))
                self.icon_image = image.subsample(factor, factor)
            except tk.TclError:
                self.icon_image = None
        if self.icon_image:
            self.icon_label.config(image=self.icon_image, width=self.icon_image.width(),
                                   height=self.icon_image.height())
        else:
            self.icon_label.config(image="", width=9, height=4)
        self.rejected = None
        self.override_button.pack_forget()
        if report is not None:
            self.show_encryption(report)
        elif self.forced:
            self.encryption_ok = False
            self.game_crypt.config(fg=WARN, text="! Check skipped because you chose 'Use it anyway'. It might not work.")
            self.game_card.set_badge("Used anyway", WARN)
        else:
            self.game_crypt.config(text="")
            self.game_card.set_badge("", MUTED)
        self.update_actions()

    def show_encryption(self, report) -> None:
        main = report.main
        if report.encrypted:
            names = ", ".join(b.name for b in report.encrypted)
            self.encryption_ok = False
            self.game_crypt.config(fg=BAD, text=(
                f"✕ Encrypted ({names}). This copy came straight from the App Store and cannot run. "
                "Use a decrypted copy instead (dumped from a jailbroken device)."))
            self.game_card.set_badge("Encrypted", BAD)
        elif main is None or main.encrypted is None:
            self.encryption_ok = None
            reason = main.problem if main else "no program file"
            self.game_crypt.config(fg=WARN, text=f"? I could not check this app ({reason}). It may not work.")
            self.game_card.set_badge("Unchecked", WARN)
        elif not main.arm64:
            self.encryption_ok = False
            self.game_crypt.config(fg=BAD, text="✕ This app has no 64-bit ARM (arm64) code, so it cannot run.")
            self.game_card.set_badge("Not supported", BAD)
        else:
            self.encryption_ok = True
            extra = f" ({len(report.binaries)} program files checked)" if len(report.binaries) > 1 else ""
            self.game_crypt.config(fg=GOOD, text=f"✓ Decrypted{extra}")
            self.game_card.set_badge("Ready ✓", GOOD)
        if report.usable:
            self.override_button.pack_forget()
        elif self.game is not None and self.forced:
            self.game_crypt.config(fg=WARN, text=self.game_crypt["text"] + "\nYou chose 'Use it anyway', so it might not work.")
            self.game_card.set_badge("Used anyway", WARN)
        elif self.game is not None:
            self.rejected = self.game.app
            self.override_button.pack(fill="x", pady=(6, 0))
        self.update_actions()

    # ------------------------------------------------------------------ actions

    def update_actions(self) -> None:
        busy = self.job is not None
        computer_ok = bool(self.checks) and all(c.ok for c in self.checks)
        phone = self.selected_phone()
        phone_ok = phone is not None and phone.usable
        game_ok = self.game is not None and (bool(self.encryption_ok) or self.forced)
        if phone is not None:
            self.show_selected_phone()

        def enable(widget, on: bool) -> None:
            widget.state(["!disabled"] if on else ["disabled"])

        enable(self.go_button, not busy and computer_ok and phone_ok and game_ok)
        enable(self.more_buttons["apk"], not busy and computer_ok and game_ok)
        enable(self.more_buttons["save"], not busy and (self.project / "build" / "xnuroid.apk").is_file())
        for key in ("start", "logs", "remove"):
            enable(self.more_buttons[key], not busy and phone_ok and bool(self.package))
        enable(self.pick_ipa, not busy)
        enable(self.pick_folder, not busy)
        if busy:
            self.cancel_button.pack(side="left", padx=10)
        else:
            self.cancel_button.pack_forget()
        if not busy and (self.status_is_hint or not self.status_text["text"]):
            self.set_status(self.next_hint(computer_ok, phone_ok, game_ok), hint=True)

    def next_hint(self, computer_ok: bool, phone_ok: bool, game_ok: bool) -> str:
        if not computer_ok:
            return "Step 1: press the red 'Fix' buttons."
        if not phone_ok:
            return "Step 2: connect your phone."
        if not game_ok:
            return "Step 3: choose a decrypted game."
        return "All set. Press 'Put the game on my phone'."

    def build_env(self) -> dict:
        extra = {}
        if self.found.get("sdk"):
            extra["ANDROID_SDK"] = str(self.found["sdk"])
        phone = self.selected_phone()
        if phone is not None:
            extra["ANDROID_SERIAL"] = phone.serial
        return extra

    def build(self, install: bool) -> None:
        if self.game is None or self.found.get("bash") is None:
            return
        if not (self.project / "build" / "libsysc++.so").is_file() and not (
                self.selected_phone() and self.selected_phone().usable):
            messagebox.showinfo("Phone needed once",
                                "The first build copies one file from your phone. Plug the phone in, then try again.",
                                parent=self)
            return
        games.write_config(self.project, self.game)
        command = "./build_apk.sh install" if install else "./build_apk.sh"
        bash = self.found["bash"]
        log_file = self.project / "build" / "helper_last_build.log"

        def work(job: Job):
            started = time.monotonic()
            job.log(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')}  {command}")
            code, output = job.stream([bash, "-lc", command], self.project, self.build_env())
            try:
                log_file.parent.mkdir(exist_ok=True)
                log_file.write_text(output, encoding="utf-8")
            except OSError:
                pass
            if job.cancelled.is_set():
                return None
            if code != 0 or (install and "Failure [" in output):
                hint = next((text for marker, text in BUILD_HINTS if marker in output), "")
                raise RuntimeError((hint or "The build stopped with an error.") +
                                   "\n\nOpen 'Show details' below to see the full log, or press 'Copy details' "
                                   "to share it.")
            return time.monotonic() - started

        def done(seconds):
            if seconds is None:
                return
            self.progress["value"] = 1.0
            minutes = f"{seconds / 60:.0f} min" if seconds >= 90 else f"{seconds:.0f} s"
            if install:
                self.set_status(f"Done in {minutes}. '{self.game.label}' is starting on your phone.", GOOD)
                self.watch_for_crash()
            else:
                self.set_status(f"APK built in {minutes}. Use 'Save APK…' to copy it anywhere.", GOOD)

        self.run_job("Building the Android app. The first build takes a few minutes…", work, done)

    def on_build_line(self, line: str) -> None:
        for marker, fraction, text in BUILD_STAGES:
            if line.startswith(marker) or marker in line:
                self.show_progress(fraction, text)
                return

    def watch_for_crash(self) -> None:
        phone, package, adb = self.selected_phone(), self.package, self.adb
        if phone is None or adb is None:
            return

        def watch():
            started = time.monotonic()
            while time.monotonic() - started < CRASH_WATCH_SECONDS and not self.stop.is_set():
                self.stop.wait(1.5)
                try:
                    exited = adb.read_app_file(phone.serial, package, "exit.txt") or (
                        time.monotonic() - started > 4 and not adb.is_running(phone.serial, package))
                except (OSError, subprocess.TimeoutExpired):
                    return
                if exited:
                    self.post("call", self.offer_crash_log)
                    return

        threading.Thread(target=watch, daemon=True).start()

    def offer_crash_log(self) -> None:
        self.set_status("The game closed right after starting.", BAD)
        if messagebox.askyesno("The game closed", "The game closed right after it started.\n\n"
                                                  "Do you want to see what it reported?", parent=self):
            self.show_phone_log()

    def start_game(self) -> None:
        phone, package = self.selected_phone(), self.package

        def work(_job):
            if not self.adb.is_installed(phone.serial, package):
                raise RuntimeError("The game is not on this phone yet. Press 'Put the game on my phone' first.")
            return self.adb.launch(phone.serial, package)

        def done(_out):
            self.set_status("Started. Look at your phone.", GOOD)
            self.watch_for_crash()

        self.run_job("Starting the game…", work, done)

    def show_phone_log(self) -> None:
        phone, package = self.selected_phone(), self.package

        def work(_job):
            return {name: self.adb.read_app_file(phone.serial, package, name)
                    for name in ("exit.txt", "err.txt", "out.txt")}

        self.run_job("Reading the game's log from the phone…", work, self.open_log_window)

    def open_log_window(self, logs: dict) -> None:
        self.set_status("")
        if not any(logs.values()):
            messagebox.showinfo("No log yet", "The game has not written a log on this phone yet.\n\n"
                                              "Start it once, then try again.", parent=self)
            return
        window = tk.Toplevel(self, bg=BG, padx=14, pady=12)
        window.title(f"Log from the phone · {self.game.label if self.game else self.package}")
        window.geometry(f"{self.px(860)}x{self.px(560)}")
        exit_text = (logs.get("exit.txt") or "").strip()
        summary = (f"The game stopped ({exit_text})." if exit_text else "The game is still running or was killed.")
        self._label(window, summary, 11, bold=True).pack(fill="x")
        self._label(window, "Send this text to whoever helps you; it shows where the game stopped.", 10,
                    MUTED).pack(fill="x", pady=(0, 8))
        text = ScrolledText(window, font=("Consolas", 9), bg="#1d1b20", fg="#e6e0e9", relief="flat")
        text.pack(fill="both", expand=True)
        combined = []
        for name in ("err.txt", "out.txt"):
            body = logs.get(name)
            if body:
                lines = body.splitlines()
                combined.append(f"===== {name} (last {min(400, len(lines))} of {len(lines)} lines) =====")
                combined.extend(lines[-400:])
        text.insert("end", "\n".join(combined))
        text.see("end")
        text.config(state="disabled")
        buttons = tk.Frame(window, bg=BG)
        buttons.pack(fill="x", pady=(8, 0))

        def copy():
            self.clipboard_clear()
            self.clipboard_append("\n".join(combined))

        def save():
            target = filedialog.asksaveasfilename(parent=window, defaultextension=".txt",
                                                  initialfile="xnuroid-log.txt", filetypes=[("Text", "*.txt")])
            if target:
                Path(target).write_text("\n".join(combined), encoding="utf-8")

        ttk.Button(buttons, text="Copy", style="Soft.TButton", command=copy).pack(side="left")
        ttk.Button(buttons, text="Save to file…", style="Soft.TButton", command=save).pack(side="left", padx=6)
        ttk.Button(buttons, text="Close", style="Soft.TButton", command=window.destroy).pack(side="right")

    def save_apk(self) -> None:
        apk = self.project / "build" / "xnuroid.apk"
        name = f"{self.game.label if self.game else 'xnuroid'}.apk"
        target = filedialog.asksaveasfilename(parent=self, defaultextension=".apk", initialfile=name,
                                              filetypes=[("Android app", "*.apk")])
        if not target:
            return
        try:
            shutil.copyfile(apk, target)
        except OSError as error:
            messagebox.showerror("Could not save", str(error), parent=self)
            return
        self.set_status(f"Saved to {target}. Copy it to any 64-bit Android 12+ phone to install.", GOOD)

    def uninstall(self) -> None:
        if not messagebox.askyesno("Remove from phone",
                                   f"Remove '{self.game.label if self.game else self.package}' from your phone?\n\n"
                                   "Its saved progress on the phone is deleted too.", parent=self):
            return
        phone, package = self.selected_phone(), self.package

        def work(_job):
            return self.adb.uninstall(phone.serial, package)

        def done(out):
            ok = "Success" in out
            self.set_status("Removed from your phone." if ok else "It was not installed on this phone.",
                            GOOD if ok else WARN)

        self.run_job("Removing the game from your phone…", work, done)


def ask_project() -> Path | None:
    root = tk.Tk()
    root.withdraw()
    messagebox.showinfo("Where is xnuroid?",
                        "I could not find the xnuroid project folder.\n\nDownload 'Source code (zip)' from the same "
                        "release page, unzip it, and put this program inside that folder. Or press OK and show me "
                        "the folder now.", parent=root)
    while True:
        chosen = filedialog.askdirectory(parent=root, title="Choose the xnuroid folder (it contains build_apk.sh)")
        if not chosen:
            root.destroy()
            return None
        if env.is_project_root(Path(chosen)):
            root.destroy()
            return Path(chosen)
        if not messagebox.askretrycancel("Wrong folder", "That folder has no build_apk.sh. Pick the xnuroid folder.",
                                         parent=root):
            root.destroy()
            return None


def main() -> None:
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    project = guess_project() or ask_project()
    if project is None:
        return
    start_with = Path(sys.argv[1].strip('"')) if len(sys.argv) > 1 else None
    Helper(project, start_with).mainloop()


if __name__ == "__main__":
    main()
