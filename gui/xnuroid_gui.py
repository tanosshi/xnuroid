"""xnuroid helper: a step-by-step front end for turning an iOS app into an Android app.

Run with `python gui/xnuroid_gui.py`, or the packaged xnuroid-helper.exe placed in the project folder.
Dropping an .ipa onto the exe opens it straight away.

The window is a six-step wizard (Set up, Game, Phone, Review, Build, Play). The look is in m3.py, one class per step
is in pages.py, and this file holds the state and the work: jobs, adb, the encryption check and the build.
"""
from __future__ import annotations

import base64
import json
import os
import plistlib
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
import traceback
import webbrowser
import zipfile
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox

sys.path.insert(0, str(Path(__file__).resolve().parent))
import environment as env  # noqa: E402
import game as games  # noqa: E402
import m3 as m  # noqa: E402
import pages  # noqa: E402
from m3 import C, px  # noqa: E402

SETTINGS = Path(os.environ.get("APPDATA", Path.home())) / "xnuroid" / "helper.json"
PHONE_POLL_SECONDS = 3
CRASH_WATCH_SECONDS = 10

STEPS = ["Set up", "Game", "Phone", "Review", "Build", "Play"]
S_SETUP, S_GAME, S_PHONE, S_REVIEW, S_BUILD, S_DONE = range(6)

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
     "An older copy of this game, signed on another PC, is on the phone. Uninstall the old app on the phone "
     "(Settings > Apps), then try again."),
    ("INSTALL_FAILED_INSUFFICIENT_STORAGE", "Your phone is out of space. Free some space and try again."),
    ("INSTALL_FAILED_USER_RESTRICTED",
     "Your phone blocked the install. On Xiaomi/Redmi/POCO phones turn on 'Install via USB' in Developer options."),
    ("INSTALL_FAILED_NO_MATCHING_ABIS", "This phone is not a 64-bit ARM phone, so the app cannot run on it."),
    ("debug.keystore", "The app signing key is missing. Go back to step 1 and press Fix next to 'App signing key'."),
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


class Helper(tk.Tk):
    def __init__(self, project: Path, start_with: Path | None):
        super().__init__()
        m.init(self)
        self.project = project
        self.settings = load_settings()
        self.settings["project"] = str(project)
        save_settings(self.settings)
        self.events: queue.Queue = queue.Queue()
        self.stop = threading.Event()
        self.job: Job | None = None
        self.job_kind = ""
        self.queued: list[tuple] = []
        self.log_lines: list[str] = []
        self.status_is_hint = False
        self.pending_start = start_with is not None

        # computer and phone
        self.found: dict = {}
        self.checks: list[env.Check] = []
        self.adb: env.Adb | None = None
        self.phones: list[env.Phone] = []
        self.phone_serial = ""

        # game
        self.game: games.Game | None = None
        self.package = ""
        self.report = None                 # the encryption report of the chosen game
        self.encryption_ok: bool | None = None
        self.forced = False
        self.rejected: Path | None = None  # set when the user may override a refusal
        self.reject_path: Path | None = None
        self.reject_message = ""
        self.icon_image: tk.PhotoImage | None = None

        # build and play
        self.install_mode = True
        self.skip_splash = tk.BooleanVar(value=bool(self.settings.get("skip_splash", True)))
        self.build_install = True
        self.build_state = "idle"          # idle, running, failed, cancelled, done
        self.build_error = ""
        self.build_started = 0.0
        self.build_result: float | None = None
        self.stage = 0.0
        self.crashed = False

        self.step = S_SETUP
        self.reached = S_SETUP

        self.title("xnuroid helper")
        self.configure(bg=C.surface)
        self._size_window()
        self._layout()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(100, self.drain)
        self.go(S_SETUP)
        self.refresh_computer()
        self.load_saved_game()
        threading.Thread(target=self.poll_phones, daemon=True).start()
        if start_with:
            self.after(300, lambda: self.choose(start_with))

    # ------------------------------------------------------------------ derived state

    @property
    def computer_ok(self) -> bool:
        return bool(self.checks) and all(c.ok for c in self.checks)

    @property
    def game_ok(self) -> bool:
        return self.game is not None and (bool(self.encryption_ok) or self.forced)

    @property
    def phone_ok(self) -> bool:
        phone = self.selected_phone()
        return phone is not None and phone.usable

    @property
    def has_helper_lib(self) -> bool:
        return (self.project / "build" / "libsysc++.so").is_file()

    @property
    def page(self) -> pages.Page:
        return self.pages[self.step]

    def apk_path(self) -> str:
        return str(self.project / "build" / "xnuroid.apk")

    # ------------------------------------------------------------------ layout

    def _size_window(self) -> None:
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        w, h = min(px(1000), sw - px(60)), min(px(720), sh - px(110))
        self.geometry(f"{w}x{h}+{(sw - w) // 2}+{max(0, (sh - h) // 2 - px(20))}")
        self.minsize(min(px(940), w), min(px(640), h))

    def _layout(self) -> None:
        self.rail = m.StepRail(self, STEPS, self.go, self.can_go)
        self.rail.pack(side="left", fill="y")
        self.main = tk.Frame(self, bg=C.surface)
        self.main.pack(side="left", fill="both", expand=True)
        main = self.main

        # bottom bar
        self.footer = tk.Frame(main, bg=C.surface_low, padx=px(32), pady=px(14))
        self.footer.pack(side="bottom", fill="x")
        self.divider = tk.Frame(main, bg=C.outline_variant, height=1)
        self.divider.pack(side="bottom", fill="x")
        footer = self.footer
        footer.columnconfigure(1, weight=1)
        tools = tk.Frame(footer, bg=C.surface_low)
        tools.grid(row=0, column=0, sticky="w")
        self.details_button = m.Button(tools, "Show details", self.toggle_details, kind="text", compact=True)
        self.details_button.pack(side="left")
        self.problems_button = m.Button(tools, "Check for problems", self.check_problems, kind="text",
                                        compact=True)
        self.problems_button.pack(side="left", padx=(px(6), 0))
        middle = tk.Frame(footer, bg=C.surface_low)
        middle.grid(row=0, column=1, sticky="ew", padx=px(14))
        self.status = m.Label(middle, "", "body", C.on_surface_variant, wrap=True)
        self.status.pack(fill="x")
        self.busy_row = tk.Frame(middle, bg=C.surface_low)
        self.progress = m.LinearProgress(self.busy_row)
        self.cancel_button = m.Button(self.busy_row, "Cancel", self.cancel, kind="text", compact=True)
        self.cancel_button.pack(side="right", padx=(px(10), 0))
        self.progress.pack(side="left", fill="x", expand=True)
        buttons = tk.Frame(footer, bg=C.surface_low)
        buttons.grid(row=0, column=2, sticky="e")
        self.back_button = m.Button(buttons, "Back", kind="text")
        self.primary_button = m.Button(buttons, "Continue", kind="filled")

        # page header and scrolling body
        self.header = tk.Frame(main, bg=C.surface)
        self.title_label = m.Label(self.header, "", "headline", wrap=True)
        self.title_label.pack(fill="x")
        self.subtitle_label = m.Label(self.header, "", "body_l", C.on_surface_variant, wrap=True)
        self.subtitle_label.pack(fill="x", pady=(px(2), 0))
        self.scroll = m.ScrollFrame(main, C.surface)
        self.scroll.pack(fill="both", expand=True)
        self.pages = [pages.SetupPage(self, self.scroll.inner), pages.GamePage(self, self.scroll.inner),
                      pages.PhonePage(self, self.scroll.inner), pages.ReviewPage(self, self.scroll.inner),
                      pages.BuildPage(self, self.scroll.inner), pages.DonePage(self, self.scroll.inner)]
        self.drawer = m.LogView(main, height=9)
        self.snack = m.Snackbar(main)

    def toggle_details(self) -> None:
        if self.drawer.winfo_ismapped():
            self.drawer.pack_forget()
            self.details_button.set_text("Show details")
        else:
            self.drawer.pack(side="bottom", fill="x", before=self.scroll)
            self.drawer.text.see("end")
            self.details_button.set_text("Hide details")

    # ------------------------------------------------------------------ navigation

    def can_go(self, index: int) -> bool:
        if index == self.step or self.job_kind == "build":
            return False
        if index == S_BUILD:
            return False
        if index == S_DONE:
            return self.build_result is not None
        return index <= self.reached

    def go(self, index: int) -> None:
        current = getattr(self, "_shown", None)
        if current is not None:
            current.frame.pack_forget()
        self.step = index
        self.reached = max(self.reached, index)
        page = self.pages[index]
        self._shown = page
        if page.header:
            self.title_label.config(text=page.title)
            self.subtitle_label.config(text=page.subtitle)
            self.header.pack(fill="x", padx=px(32), pady=(px(28), px(4)), before=self.scroll)
        else:
            self.header.pack_forget()
        page.frame.pack(fill="x", padx=px(32), pady=(px(4), px(28)))
        self.scroll.to_top()
        self.rail.set_state(index, self.reached)
        page.on_show()
        self.update_actions()

    def refresh_page(self) -> None:
        self.page.refresh()
        self.update_actions()

    def update_actions(self) -> None:
        busy = self.job is not None
        footer = self.page.footer()
        self.back_button.pack_forget()
        self.primary_button.pack_forget()
        if footer.back:
            self.back_button.set_text(footer.back)
            self.back_button.command = footer.back_action
            self.back_button.set_enabled(not busy)
            self.back_button.pack(side="left", padx=(0, px(8)))
        if footer.primary:
            self.primary_button.set_text(footer.primary)
            self.primary_button.command = footer.primary_action
            self.primary_button.set_enabled(footer.primary_enabled)
            self.primary_button.pack(side="left")
        if busy:
            if not self.busy_row.winfo_ismapped():
                self.busy_row.pack(fill="x", pady=(px(6), 0))
            if self.step == S_BUILD:
                self.progress.pack_forget()
            elif not self.progress.winfo_ismapped():
                self.progress.pack(side="left", fill="x", expand=True)
        else:
            self.busy_row.pack_forget()
        self.rail.set_state(self.step, self.reached)
        self.problems_button.set_text("Check for problems" if self.phones else "What went wrong?")
        if not busy and (self.status_is_hint or not self.status["text"]):
            self.set_status(self.page.hint(), hint=True)

    # ------------------------------------------------------------------ events and jobs

    def post(self, kind: str, payload=None) -> None:
        self.events.put((kind, payload))

    def drain(self) -> None:
        pending_log = []
        try:
            while True:
                try:
                    kind, payload = self.events.get_nowait()
                except queue.Empty:
                    break
                try:
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
                except Exception:  # one failing handler must not stop the pump, or the window stays busy for good
                    pending_log.extend(traceback.format_exc().splitlines())
            if pending_log:
                self.append_log(pending_log)
        finally:
            if not self.stop.is_set():
                self.after(80, self.drain)

    def append_log(self, lines: list[str]) -> None:
        self.log_lines.extend(lines)
        self.drawer.append(lines)

    def set_status(self, text: str, color: str = C.on_surface_variant, hint: bool = False) -> None:
        self.status.config(text=text, fg=color)
        self.status_is_hint = hint

    def show_progress(self, fraction: float | None, text: str) -> None:
        if fraction is None:
            self.progress.start()
        else:
            self.progress.set(fraction)
        if text:
            self.set_status(text)
        self.page.on_progress(fraction, text)

    def run_job(self, title: str, work, done=None, on_error=None, on_cancel=None, kind: str = "") -> None:
        if self.job:
            self.queued.append((title, work, done, on_error, on_cancel, kind))
            return
        job = Job(self.post)
        self.job = job
        self.job_kind = kind
        self.show_progress(None, title)
        self.refresh_page()

        def worker():
            try:
                result, error = work(job), None
            except Exception as exc:  # shown to the user, never swallowed
                result, error = None, exc
            self.post("call", lambda: self.finish_job(job, result, error, done, on_error, on_cancel))

        threading.Thread(target=worker, daemon=True).start()

    def finish_job(self, job: Job, result, error, done, on_error, on_cancel) -> None:
        self.job = None
        self.job_kind = ""
        self.progress.set(0.0)
        self.set_status("")
        if job.cancelled.is_set():
            self.set_status("Cancelled.", C.warn)
            if on_cancel:
                on_cancel()
        else:
            if error is None and done:
                try:
                    done(result)
                except Exception as exc:  # shown like a failed job, e.g. a read-only project folder
                    error = exc
            if error is not None:
                text = str(error) or repr(error)
                self.set_status(text.splitlines()[0], C.error)
                if on_error:
                    on_error(text)
                else:
                    m.ask(self, "Something went wrong", text)
        if self.queued and not self.job:
            self.run_job(*self.queued.pop(0))
        self.refresh_page()

    def cancel(self) -> None:
        if self.job:
            self.job.kill()

    def on_close(self) -> None:
        if self.job and not m.ask(self, "Still working", "xnuroid is still working. Stop and close anyway?",
                                  ok="Stop and close", cancel="Keep working", danger=True):
            return
        if self.job:
            self.job.kill()
        self.stop.set()
        self.destroy()

    def check_problems(self) -> None:
        """Footer entry point: the game's own log on a phone, or this helper's log when no phone is connected."""
        phone = self.selected_phone()
        if phone is None:
            if not self.drawer.winfo_ismapped():
                self.toggle_details()
            return
        problem = phone.problem()
        if problem:
            self.snack.show(problem)
            return
        if not self.package:
            self.snack.show("Choose a game first, then check for problems.")
            return
        self.show_phone_log()

    def remember_splash(self) -> None:
        self.settings["skip_splash"] = bool(self.skip_splash.get())
        save_settings(self.settings)

    def copy_details(self) -> None:
        self.clipboard_clear()
        self.clipboard_append("\n".join(self.log_lines))
        self.snack.show("Details copied. Paste them wherever you ask for help.")

    # ------------------------------------------------------------------ step 1: computer

    def refresh_computer(self) -> None:
        def work(_job):
            return env.check_computer()

        def done(result):
            self.checks, self.found = result
            adb = self.found.get("adb")
            self.adb = env.Adb(adb) if adb and Path(adb).is_file() else None
            if self.pending_start and self.computer_ok:
                self.pending_start = False
                if self.step == S_SETUP:
                    self.go(S_GAME)

        self.run_job("Checking your computer…", work, done, kind="setup")

    def fix(self, check: env.Check) -> None:
        def open_page(title: str, text: str, url: str) -> None:
            if m.ask(self, title, text, ok="Open download page", cancel="Not now"):
                webbrowser.open(url)

        if check.fix == "git":
            open_page("Install Git for Windows",
                      "Your browser will open the Git download page.\n\nRun the installer and keep every default "
                      "option. Then come back and press Check again.", env.GIT_URL)
        elif check.fix == "python":
            open_page("Install Python",
                      "Your browser will open the Python download page.\n\nIMPORTANT: on the first installer "
                      "screen, tick 'Add python.exe to PATH'. Then come back and press Check again.", env.PYTHON_URL)
        elif check.fix == "studio":
            open_page("Install Android Studio",
                      "Your browser will open the Android Studio page.\n\nInstall it, open it once and let it "
                      "finish its setup wizard. Then come back and press Check again.", env.STUDIO_URL)
        elif check.fix == "sdk_parts":
            self.install_sdk_parts()
        elif check.fix == "keystore":
            self.run_job("Creating the app signing key…", lambda _job: env.create_debug_keystore(),
                         lambda _path: self.refresh_computer(), kind="setup")

    def install_sdk_parts(self) -> None:
        sdk = self.found.get("sdk")
        manager = env.find_sdkmanager(sdk) if sdk else None
        if manager is None:
            m.ask(self, "Install the missing Android parts",
                  "Open Android Studio and choose More Actions > SDK Manager.\n\n"
                  "1. On the 'SDK Platforms' tab tick Android 16 (API 36).\n"
                  "2. On the 'SDK Tools' tab tick 'Show Package Details', then tick:\n"
                  f"     • NDK (Side by side) > {env.NDK_VERSION}\n"
                  "     • Android SDK Build-Tools > 36.1.0\n"
                  "     • Android SDK Command-line Tools\n"
                  "     • Android SDK Platform-Tools\n"
                  "3. Press OK and wait for the download.\n\n"
                  "Then come back and press Check again.")
            return
        if not m.ask(self, "Install the missing Android parts",
                     "xnuroid will download the missing Android parts now (about 1 GB) and accept Google's SDK "
                     "licences for you.\n\nContinue?", ok="Download", cancel="Cancel"):
            return

        keytool = env.find_keytool()
        java = {"JAVA_HOME": str(keytool.parent.parent)} if keytool and not os.environ.get("JAVA_HOME") else {}

        def work(job: Job):
            code, _ = job.stream([manager, "--install", *env.SDK_PACKAGES], sdk, java, stdin_text="y\n" * 30)
            if code != 0 and not job.cancelled.is_set():
                raise RuntimeError("The Android download failed. Press Show details to see why.")

        self.run_job("Downloading Android parts. This can take a while…", work, lambda _r: self.refresh_computer(),
                     kind="setup")

    # ------------------------------------------------------------------ step 3: phone

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
        if self.phone_serial not in {p.serial for p in phones}:
            self.phone_serial = ""
        if self.step in (S_PHONE, S_REVIEW):
            self.page.refresh()
        self.update_actions()

    def selected_phone(self) -> env.Phone | None:
        if not self.phones:
            return None
        match = next((p for p in self.phones if p.serial == self.phone_serial), None)
        return match or next((p for p in self.phones if p.usable), self.phones[0])

    def pick_phone(self, phone: env.Phone) -> None:
        self.phone_serial = phone.serial
        self.refresh_page()

    def use_phone(self) -> None:
        self.install_mode = True
        self.go(S_REVIEW)

    def skip_phone(self) -> None:
        self.install_mode = False
        self.go(S_REVIEW)

    # ------------------------------------------------------------------ step 2: game

    def load_saved_game(self) -> None:
        saved = games.read_config(self.project)
        if saved:
            self.forced = self.settings.get("forced_app") == str(saved.app)
            self.set_game(saved, None)
            self.check_saved_encryption(saved)

    def check_saved_encryption(self, game: games.Game) -> None:
        def work(_job):
            return games.check_encryption(game.app, game.app)

        self.run_job("Checking the app bundle…", work, self.apply_report, kind="game")

    def ask_ipa(self) -> None:
        path = filedialog.askopenfilename(parent=self, title="Choose your .ipa file",
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
        title = "Checking the app bundle…"

        if choice.is_file():
            if choice.suffix.lower() != ".ipa":
                self.reject(choice, None, f"{choice.name} is not an .ipa file.", allow_override=False)
                return

            def work(job: Job):
                job.progress(None, title)
                try:
                    report = games.check_encryption(choice, None)
                except (ValueError, OSError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
                    if not force:
                        return Rejected(choice, None, f"This file could not be read as an iOS app ({error}).")
                    report = None
                if not force and not report.usable:
                    return Rejected(choice, report)
                root = games.extract_ipa(choice, games_dir, job.progress, job.cancelled.is_set)
                candidates = games.bundle_candidates(root / "Payload")
                if not candidates:
                    raise ValueError(f"There is no app inside {choice.name}.")
                app = self.pick_bundle(candidates)
                if app is None:
                    job.cancelled.set()
                elif report is not None and app.name != report.app_name:
                    # The check above looked at the first app of the file; the user picked another one.
                    report = games.check_encryption(app, app)
                    if not force and not report.usable:
                        return Rejected(app, report)
                return report, app

            self.run_job(title, work, lambda r: self.after_pick(r, force), kind="game")
        elif choice.is_dir():
            candidates = games.bundle_candidates(choice)
            if not candidates:
                self.reject(choice, None, "There is no iOS app in this folder. Pick the folder that contains "
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

            self.run_job(title, work, lambda r: self.after_pick(r, force), kind="game")
        else:
            m.ask(self, "Not found", f"There is nothing at\n{choice}")

    def pick_bundle(self, candidates: list[Path]) -> Path | None:
        """Called from a worker; asks on the UI thread when the file holds several apps."""
        answer: dict = {}
        if len(candidates) == 1:
            return candidates[0]
        ready = threading.Event()

        def ask():
            dialog = m.Dialog(self, "Which app?", "Which one is the game?")
            m.Label(dialog.body, "This file contains more than one app.", "body_l", C.on_surface_variant,
                    wrap=dialog.inner_w).pack(fill="x", pady=(0, px(8)))
            chosen = {"index": 0}
            tiles: list[m.ChoiceTile] = []

            def select(index: int) -> None:
                chosen["index"] = index
                for i, tile in enumerate(tiles):
                    tile.set(selected=i == index)

            for index, app in enumerate(candidates[:8]):
                tile = m.ChoiceTile(dialog.body, app.stem, app.name, on_click=lambda i=index: select(i), height=62)
                tile.pack(fill="x", pady=px(3))
                tiles.append(tile)
            select(0)

            def closed(value):
                answer["app"] = candidates[chosen["index"]] if value == "ok" else None
                ready.set()

            dialog.on_close = closed
            dialog.add_action("Use this one", "ok", kind="filled")
            dialog.add_action("Cancel", None)
            dialog.present()

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
            self.reject(app, report, str(error), allow_override=False)
            return
        games.write_config(self.project, game)
        self.forced = force
        self.settings["forced_app"] = str(app) if force else ""
        save_settings(self.settings)
        self.set_game(game, report)
        if force:
            self.set_status(f"'{game.label}' is set up anyway. It might not work.", C.warn)
        else:
            self.set_status(f"'{game.label}' is ready.", C.primary)

    def apply_report(self, report) -> None:
        """Takes the result of the encryption check into the state the pages read."""
        self.report = report
        main = report.main
        if report.encrypted:
            self.encryption_ok = False
        elif main is None or main.encrypted is None:
            self.encryption_ok = None
        elif not main.arm64:
            self.encryption_ok = False
        else:
            self.encryption_ok = True
        if report.usable:
            self.rejected = None
        elif self.game is not None and not self.forced:
            self.rejected = self.game.app

    def reject(self, choice: Path, report, message: str = "", allow_override: bool = True) -> None:
        """Shows what was picked and why it was refused, instead of leaving the previous game on screen."""
        self.game = None
        self.package = ""
        self.forced = False
        self.rejected = choice if allow_override else None
        self.reject_path = choice
        self.reject_message = message
        self.icon_image = None
        if report is not None and not message:
            self.apply_report(report)
        else:
            self.report = report
            self.encryption_ok = False
        self.set_status("That game cannot be used. Choose another one"
                        + (", or press Use it anyway." if allow_override else "."), C.error)
        self.refresh_page()

    def use_anyway(self) -> None:
        choice = self.rejected
        if choice is None:
            return
        if not m.ask(self, "Use it anyway?",
                     "The check says this app is encrypted, unreadable or not 64-bit ARM, so it will most likely "
                     "not work. The check can be wrong, though.\n\nUse it anyway?", ok="Use it anyway",
                     cancel="Cancel", danger=True):
            return
        self.choose(choice, force=True)

    def set_game(self, game: games.Game, report) -> None:
        self.game = game
        self.package = games.package_id(self.project, game.info)
        icon = games.icon_png(self.project, game)
        self.icon_image = None
        if icon:
            try:
                image = tk.PhotoImage(data=base64.b64encode(icon))
                factor = max(1, image.width() // px(72))
                self.icon_image = image.subsample(factor, factor)
            except tk.TclError:
                self.icon_image = None
        self.rejected = None
        self.reject_path = None
        self.reject_message = ""
        self.report = None
        if report is not None:
            self.apply_report(report)
        elif self.forced:
            self.encryption_ok = False

    # ------------------------------------------------------------------ step 5: build

    def build_env(self) -> dict:
        extra = {}
        if self.found.get("sdk"):
            extra["ANDROID_SDK"] = str(self.found["sdk"])
        phone = self.selected_phone()
        if phone is not None:
            extra["ANDROID_SERIAL"] = phone.serial
        extra.update(env.java_env() or {})
        if not self.skip_splash.get():
            extra["KEEP_SPLASH"] = "1"
        return extra

    def build(self, install: bool) -> None:
        if self.game is None or self.found.get("bash") is None:
            return
        if not self.has_helper_lib and not self.phone_ok:
            m.ask(self, "Phone needed once", "The first build copies one file from your phone. Plug the phone in, "
                                              "then try again.")
            return
        games.write_config(self.project, self.game)
        command = "./build_apk.sh install" if install else "./build_apk.sh"
        bash = self.found["bash"]
        log_file = self.project / "build" / "helper_last_build.log"
        self.build_install = install
        self.build_state = "running"
        self.build_error = ""
        self.build_result = None
        self.build_started = time.monotonic()
        self.stage = 0.0
        self.crashed = False
        self.go(S_BUILD)

        def work(job: Job):
            started = time.monotonic()
            job.log(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')}  {command}")
            job.log("splash: skipped" if self.skip_splash.get() else "splash: kept (the game plays its launch video)")
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
                                   "\n\nPress Show details to see the full log, or Copy details to share it.")
            job.log(f"APK: {self.apk_path()}")
            return time.monotonic() - started

        def done(seconds):
            if seconds is None:
                return
            self.build_state = "done"
            self.build_result = seconds
            self.stage = 1.0
            self.go(S_DONE)
            if install:
                self.watch_for_crash()

        def failed(text: str):
            self.build_state = "failed"
            self.build_error = text

        def cancelled():
            self.build_state = "cancelled"

        self.run_job("Building the Android app. The first build takes a few minutes…", work, done,
                     on_error=failed, on_cancel=cancelled, kind="build")

    def on_build_line(self, line: str) -> None:
        for marker, fraction, text in BUILD_STAGES:
            if line.startswith(marker) or marker in line:
                self.stage = max(self.stage, fraction)
                self.show_progress(fraction, text)
                return

    # ------------------------------------------------------------------ step 6: play

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
                    self.post("call", self.on_crash)
                    return

        threading.Thread(target=watch, daemon=True).start()

    def on_crash(self) -> None:
        self.crashed = True
        self.set_status("The game closed right after starting.", C.error)
        if self.step == S_DONE:
            self.page.refresh()

    def start_game(self) -> None:
        phone, package = self.selected_phone(), self.package

        def work(_job):
            if not self.adb.is_installed(phone.serial, package):
                raise RuntimeError("The game is not on this phone yet. Build and install it first.")
            return self.adb.launch(phone.serial, package)

        def done(_out):
            self.crashed = False
            self.set_status("Started. Look at your phone.", C.primary)
            self.watch_for_crash()

        self.run_job("Starting the game…", work, done, kind="play")

    def uninstall(self) -> None:
        phone, package = self.selected_phone(), self.package
        if phone is None or not hasattr(self.adb, "uninstall"):
            return
        label = self.game.label if self.game else package
        if not m.ask(self, "Remove from phone?", f"This removes '{label}' and its saved data from {phone.label}.",
                     ok="Remove", cancel="Keep it", danger=True):
            return

        def done(_out):
            self.build_result = None
            self.set_status("Removed from your phone.", C.primary)
            self.go(S_REVIEW)

        self.run_job("Removing the game from your phone…", lambda _job: self.adb.uninstall(phone.serial, package),
                     done, kind="play")

    def show_phone_log(self) -> None:
        phone, package = self.selected_phone(), self.package

        def work(_job):
            return {name: self.adb.read_app_file(phone.serial, package, name)
                    for name in ("exit.txt", "err.txt", "out.txt")}

        self.run_job("Reading the game's log from the phone…", work, self.open_log_window, kind="play")

    def open_log_window(self, logs: dict) -> None:
        self.set_status("")
        if not any(logs.values()):
            m.ask(self, "No log yet", "The game has not written a log on this phone yet.\n\nStart it once, then "
                                      "try again.")
            return
        exit_text = (logs.get("exit.txt") or "").strip()
        summary = f"The game stopped ({exit_text})." if exit_text else "The game is still running or was killed."
        dialog = m.Dialog(self, f"Log from the phone · {self.game.label if self.game else self.package}",
                          "Game log", width=820)
        m.Label(dialog.body, summary, "subtitle", wrap=px(772)).pack(fill="x")
        m.Label(dialog.body, "Send this text to whoever helps you. It shows where the game stopped.", "body",
                C.on_surface_variant, wrap=px(772)).pack(fill="x", pady=(0, px(10)))
        combined = []
        for name in ("err.txt", "out.txt"):
            body = logs.get(name)
            if body:
                lines = body.splitlines()
                combined.append(f"===== {name} (last {min(400, len(lines))} of {len(lines)} lines) =====")
                combined.extend(lines[-400:])
        view = m.LogView(dialog.body, height=18)
        view.pack(fill="both", expand=True)
        view.set_text("\n".join(combined))

        def copy():
            self.clipboard_clear()
            self.clipboard_append("\n".join(combined))
            self.snack.show("Log copied.")

        def save():
            target = filedialog.asksaveasfilename(parent=dialog, defaultextension=".txt",
                                                  initialfile="xnuroid-log.txt", filetypes=[("Text", "*.txt")])
            if target:
                Path(target).write_text("\n".join(combined), encoding="utf-8")

        dialog.add_action("Close", True)
        save_button = m.Button(dialog.actions, "Save to file…", save, kind="text")
        save_button.pack(side="right")
        m.Button(dialog.actions, "Copy", copy, kind="tonal").pack(side="right", padx=(0, px(8)))
        dialog.present()

    def save_apk(self) -> None:
        source = Path(self.apk_path())
        if not source.is_file():
            m.ask(self, "No APK found", f"There is no file at\n{source}")
            return
        name = "".join(ch for ch in (self.game.label if self.game else "xnuroid") if ch.isalnum() or ch in " -_")
        target = filedialog.asksaveasfilename(parent=self, defaultextension=".apk", initialfile=f"{name.strip()}.apk",
                                              filetypes=[("Android app", "*.apk")])
        if target:
            try:
                shutil.copyfile(source, target)
                self.snack.show("APK saved.")
            except OSError as error:
                m.ask(self, "Could not save", str(error))

    def show_apk_folder(self) -> None:
        apk = Path(self.apk_path())
        if sys.platform == "win32":
            subprocess.Popen(["explorer", "/select,", str(apk)])
        else:
            webbrowser.open(apk.parent.as_uri())


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
