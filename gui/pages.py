"""The pages of the xnuroid helper wizard. Each page draws itself from the state kept on the app object
(`xnuroid_gui.Helper`) and tells the footer which buttons to show."""
from __future__ import annotations

import time
import tkinter as tk
from dataclasses import dataclass
from typing import Callable

import m3 as m
from m3 import C, px

BUILD_PLAN = [("Compile the app", 0.80), ("Pack the game into the app", 0.88),
              ("Install it on your phone", 0.97), ("Start the game", 1.00)]


@dataclass
class Footer:
    primary: str = ""
    primary_enabled: bool = True
    primary_action: Callable | None = None
    back: str = ""
    back_action: Callable | None = None


class Page:
    title = ""
    subtitle = ""
    header = True

    def __init__(self, app, parent):
        self.app = app
        self.frame = tk.Frame(parent, bg=C.surface)
        self.build()

    def build(self) -> None: ...
    def refresh(self) -> None: ...
    def on_show(self) -> None: self.refresh()
    def footer(self) -> Footer: return Footer()
    def hint(self) -> str: return ""
    def on_progress(self, fraction, text) -> None: ...

    # small helpers shared by the pages
    def clear(self, widget: tk.Widget) -> None:
        for child in widget.winfo_children():
            child.destroy()

    def row(self, parent, bg: str) -> tk.Frame:
        row = tk.Frame(parent, bg=bg)
        row.pack(fill="x", pady=px(6))
        return row


# ====================================================================================================== 1. set up
class SetupPage(Page):
    title = "Welcome"
    header = False

    def build(self) -> None:
        hero = m.Surface(self.frame, fill=C.primary_container, radius=28, outline=None, pad=(30, 26))
        hero.pack(fill="x", pady=(px(28), px(16)))
        hero.body.columnconfigure(0, weight=1)
        text = tk.Frame(hero.body, bg=C.primary_container)
        text.grid(row=0, column=0, sticky="nsew")
        m.Label(text, "Play your iOS game on Android", "display", C.on_primary_container, wrap=True).pack(fill="x")
        m.Label(text, "xnuroid turns an iOS app into an Android app and puts it on your phone. "
                      "First, a quick check that this computer has everything it needs.", "body_l",
                C.on_primary_container, wrap=True).pack(fill="x", pady=(px(10), 0))
        m.Label(text, "It does not include or download any apps. Use a copy you are licensed to use.", "small",
                C.on_primary_container, wrap=True).pack(fill="x", pady=(px(10), 0))

        self.card = m.card(self.frame, radius=20, pad=(24, 20))
        self.card.pack(fill="x")
        head = tk.Frame(self.card.body, bg=self.card.fill)
        head.pack(fill="x")
        m.Label(head, "This computer", "title").pack(side="left")
        self.recheck = m.Button(head, "Check again", self.app.refresh_computer, kind="text", compact=True)
        self.recheck.pack(side="right")
        self.summary = m.Label(self.card.body, "", "body", C.on_surface_variant, wrap=True)
        self.summary.pack(fill="x", pady=(px(2), px(8)))
        self.rows = tk.Frame(self.card.body, bg=self.card.fill)
        self.rows.pack(fill="x")
        self._signature = None

    def refresh(self) -> None:
        app = self.app
        checks = app.checks
        self.recheck.set_enabled(app.job is None)
        signature = tuple((c.key, c.ok, c.detail) for c in checks)
        if signature != self._signature:
            self._signature = signature
            self.clear(self.rows)
            for check in checks:
                row = tk.Frame(self.rows, bg=self.card.fill)
                row.pack(fill="x", pady=px(7))
                m.StatusIcon(row, "ok" if check.ok else "bad").pack(side="left", padx=(0, px(14)))
                if check.fix:
                    m.Button(row, "Fix", lambda c=check: app.fix(c), kind="danger", compact=True).pack(side="right")
                col = tk.Frame(row, bg=self.card.fill)
                col.pack(side="left", fill="x", expand=True)
                m.Label(col, check.title, "subtitle").pack(fill="x")
                m.Label(col, check.detail, "body", C.on_surface_variant, wrap=True).pack(fill="x")
        missing = sum(not c.ok for c in checks)
        if not checks:
            self.summary.config(text="Checking your computer…")
        elif missing:
            self.summary.config(text=f"{missing} thing{'s' if missing > 1 else ''} to fix before you can continue.",
                                fg=C.error)
        else:
            self.summary.config(text="Everything is ready.", fg=C.primary)

    def footer(self) -> Footer:
        return Footer("Choose a game", self.app.computer_ok and self.app.job is None, lambda: self.app.go(1))

    def hint(self) -> str:
        if not self.app.checks:
            return "Checking your computer…"
        return "" if self.app.computer_ok else "Press Fix on the red items, then Check again."


# ====================================================================================================== 2. game
class GamePage(Page):
    title = "Choose your game"
    subtitle = "xnuroid checks that the game can run before it builds anything."

    def build(self) -> None:
        self.holder = tk.Frame(self.frame, bg=C.surface)
        self.holder.pack(fill="x")

    def refresh(self) -> None:
        self.clear(self.holder)
        app = self.app
        if app.game is None and app.reject_path is None:
            self._empty()
        else:
            self._selected()

    def _pick_buttons(self, parent, bg: str, kind_first: str) -> None:
        busy = self.app.job is not None
        first = m.Button(parent, "Choose .ipa file…", self.app.ask_ipa, kind=kind_first)
        first.set_enabled(not busy)
        first.pack(side="left")
        second = m.Button(parent, "Choose folder…", self.app.ask_folder, kind="tonal")
        second.set_enabled(not busy)
        second.pack(side="left", padx=(px(10), 0))

    def _empty(self) -> None:
        zone = m.Surface(self.holder, fill=C.surface_low, radius=24, outline=C.outline, dash=True, pad=(30, 40))
        zone.pack(fill="x", pady=(px(4), 0))
        body = zone.body
        art = tk.Canvas(body, width=px(84), height=px(84), bg=body.cget("bg"), highlightthickness=0)
        art.pack()
        art.create_oval(0, 0, px(84), px(84), fill=C.primary_container, outline="")
        m.draw_gamepad(art, px(42), px(42), px(46), C.on_primary_container, C.primary_container)
        m.Label(body, "Choose your game", "title", wrap=True).pack(fill="x", pady=(px(14), 0))
        for label in body.winfo_children()[-1:]:
            label.config(anchor="center", justify="center")
        note = m.Label(body, "Pick an .ipa file you are licensed to use, or a folder you already unpacked.", "body_l",
                       C.on_surface_variant, wrap=True)
        note.config(anchor="center", justify="center")
        note.pack(fill="x", pady=(px(4), px(18)))
        buttons = tk.Frame(body, bg=body.cget("bg"))
        buttons.pack()
        self._pick_buttons(buttons, body.cget("bg"), "filled")
        tip = m.Label(body, "Tip: you can also drop an .ipa file onto xnuroid-helper.exe.", "small",
                      C.on_surface_variant, wrap=True)
        tip.config(anchor="center", justify="center")
        tip.pack(fill="x", pady=(px(16), 0))

    # --- verdict and rows are derived from the app state -----------------------------------------------------
    def _verdict(self) -> tuple[str, str]:
        app = self.app
        report = app.report
        if app.job_kind == "game":
            return "info", "Checking…"
        if app.game is not None and report is None and not app.forced:
            return "info", "Not checked"
        if report is not None and report.usable:
            return "ok", "Compatible"
        if app.forced:
            return "warn", "Used anyway"
        if report is not None and app.encryption_ok is None:
            return "warn", "Could not check"
        return "bad", "Not compatible"

    def _rows(self) -> list[tuple[str, str, str]]:
        app, report = self.app, self.app.report
        rows: list[tuple[str, str, str]] = []
        if report is None:
            if app.reject_message:
                rows.append(("bad", "Cannot use this", app.reject_message))
            elif app.forced:
                rows.append(("warn", "Check skipped", "You chose to use this game anyway, so it might not work."))
            elif app.game is not None and app.job_kind != "game":
                rows.append(("info", "Check not run yet", "Choose the game again to check it."))
            return rows
        main = report.main
        unreadable = main is None or main.encrypted is None
        reason = (main.problem if main else "no program file") or "unreadable"
        if report.encrypted:
            names = ", ".join(b.name for b in report.encrypted)
            rows.append(("bad", "Code readable", f"Platform protection is still in place ({names}). This bundle "
                                              "cannot be loaded; use a copy you are licensed to run."))
        elif unreadable:
            rows.append(("warn", "Code readable", f"Could not check ({reason}). It may not load."))
        else:
            extra = f" ({len(report.binaries)} program files checked)" if len(report.binaries) > 1 else ""
            rows.append(("ok", "Code readable", f"The app's code is readable{extra}."))
        if unreadable:
            rows.append(("warn", "64-bit ARM code", "Could not check."))
        elif main.arm64:
            rows.append(("ok", "64-bit ARM code", "Found, so the code can run on a modern Android phone."))
        else:
            rows.append(("bad", "64-bit ARM code", "This app has no 64-bit ARM (arm64) code, so it cannot run."))
        if app.game is not None:
            if app.game.is_unity:
                rows.append(("ok", "Game engine", "Unity. This is what xnuroid supports best."))
            else:
                rows.append(("warn", "Game engine", "Regular iOS app. App mode is experimental."))
        return rows

    def _selected(self) -> None:
        app = self.app
        busy = app.job is not None
        game = app.game
        top = m.card(self.holder, radius=20, pad=(24, 20))
        top.pack(fill="x", pady=(px(4), px(14)))
        row = tk.Frame(top.body, bg=top.fill)
        row.pack(fill="x")
        icon = tk.Label(row, bg=top.fill, bd=0)
        if game is not None and app.icon_image is not None:
            icon.config(image=app.icon_image)
            icon.pack(side="left", anchor="n")
        else:
            art = tk.Canvas(row, width=px(72), height=px(72), bg=top.fill, highlightthickness=0)
            m.round_rect(art, 0, 0, px(72), px(72), px(18), fill=C.surface_high, outline="")
            m.draw_gamepad(art, px(36), px(36), px(40), C.on_surface_variant, C.surface_high)
            art.pack(side="left", anchor="n")
        col = tk.Frame(row, bg=top.fill)
        col.pack(side="left", fill="x", expand=True, padx=px(16))
        kind, text = self._verdict()
        chip = m.Chip(row, text, kind)
        chip.pack(side="right", anchor="n")
        title = game.label if game is not None else (app.reject_path.name if app.reject_path else "")
        m.Label(col, title, "title", wrap=True).pack(fill="x")
        if game is not None:
            what = "Unity game" if game.is_unity else "Regular iOS app (experimental)"
            version = f"  ·  version {game.version}" if game.version else ""
            m.Label(col, what + version, "body_l", C.on_surface_variant, wrap=True).pack(fill="x")
            m.Label(col, f"Android app id: {app.package}", "body", C.on_surface_variant, wrap=True).pack(fill="x")
            m.Label(col, str(game.app), "small", C.on_surface_variant, wrap=True).pack(fill="x", pady=(px(2), 0))
        else:
            m.Label(col, str(app.reject_path), "body", C.on_surface_variant, wrap=True).pack(fill="x")
        actions = tk.Frame(top.body, bg=top.fill)
        actions.pack(fill="x", pady=(px(16), 0))
        for text_, command in (("Choose another .ipa…", app.ask_ipa), ("Choose another folder…", app.ask_folder)):
            button = m.Button(actions, text_, command, kind="outlined", compact=True)
            button.set_enabled(not busy)
            button.pack(side="left", padx=(0, px(8)))

        rows = self._rows()
        if rows:
            comp = m.card(self.holder, radius=20, pad=(24, 18))
            comp.pack(fill="x")
            m.Label(comp.body, "Compatibility", "title").pack(fill="x", pady=(0, px(4)))
            for status, name, detail in rows:
                line = tk.Frame(comp.body, bg=comp.fill)
                line.pack(fill="x", pady=px(6))
                m.StatusIcon(line, status).pack(side="left", padx=(0, px(14)), anchor="n")
                text_col = tk.Frame(line, bg=comp.fill)
                text_col.pack(side="left", fill="x", expand=True)
                m.Label(text_col, name, "subtitle").pack(fill="x")
                m.Label(text_col, detail, "body", C.on_surface_variant, wrap=True).pack(fill="x")
            if app.rejected is not None:
                tk.Frame(comp.body, bg=C.outline_variant, height=1).pack(fill="x", pady=(px(10), px(10)))
                m.Label(comp.body, "The check can be wrong. If you know this game works, you can use it anyway.",
                        "body", C.on_surface_variant, wrap=True).pack(fill="x")
                button = m.Button(comp.body, "Use it anyway…", app.use_anyway, kind="danger", compact=True)
                button.set_enabled(not busy)
                button.pack(anchor="w", pady=(px(8), 0))

    def footer(self) -> Footer:
        return Footer("Choose a phone", self.app.game_ok and self.app.job is None, lambda: self.app.go(2),
                      "Back", lambda: self.app.go(0))

    def hint(self) -> str:
        if self.app.game is None:
            return "Choose an app you are licensed to use to continue."
        if not self.app.game_ok:
            return "This game cannot be used as it is."
        return "The game is ready."


# ====================================================================================================== 3. phone
class PhonePage(Page):
    title = "Connect your phone"
    subtitle = "Only the first build needs the phone. After that you can build without it."

    def build(self) -> None:
        self.holder = tk.Frame(self.frame, bg=C.surface)
        self.holder.pack(fill="x")

    def refresh(self) -> None:
        self.clear(self.holder)
        app = self.app
        if app.adb is None:
            self._note("Finish the setup first", "xnuroid needs Android's Platform-Tools to talk to your phone. "
                       "Go back to step 1 and fix the red items.", "warn")
            return
        if not app.phones:
            self._waiting()
        else:
            self._list()
        self._skip()

    def _note(self, title: str, body: str, kind: str = "info") -> None:
        box = m.Surface(self.holder, fill=C.surface_low, radius=20, pad=(24, 20))
        box.pack(fill="x", pady=(px(4), 0))
        row = tk.Frame(box.body, bg=box.fill)
        row.pack(fill="x")
        m.StatusIcon(row, kind).pack(side="left", anchor="n", padx=(0, px(14)))
        col = tk.Frame(row, bg=box.fill)
        col.pack(side="left", fill="x", expand=True)
        m.Label(col, title, "subtitle").pack(fill="x")
        m.Label(col, body, "body", C.on_surface_variant, wrap=True).pack(fill="x")

    def _waiting(self) -> None:
        box = m.card(self.holder, radius=20, pad=(24, 22))
        box.pack(fill="x", pady=(px(4), 0))
        row = tk.Frame(box.body, bg=box.fill)
        row.pack(fill="x")
        art = tk.Canvas(row, width=px(70), height=px(116), bg=box.fill, highlightthickness=0)
        m.draw_phone(art, px(8), px(4), px(54), px(108), C.surface_highest, C.surface_low, C.outline)
        art.pack(side="left", anchor="n")
        col = tk.Frame(row, bg=box.fill)
        col.pack(side="left", fill="x", expand=True, padx=(px(18), 0))
        m.Label(col, "Looking for your phone…", "title").pack(fill="x")
        m.Label(col, "This page updates by itself when the phone appears.", "body", C.on_surface_variant,
                wrap=True).pack(fill="x", pady=(0, px(8)))
        steps = ["Plug your phone into this PC with a USB cable.",
                 "Open Settings > About phone and tap Build number 7 times.",
                 "Open Settings > Developer options and turn on USB debugging.",
                 "Tap Allow when the phone asks about USB debugging."]
        for index, text in enumerate(steps, 1):
            line = tk.Frame(col, bg=box.fill)
            line.pack(fill="x", pady=px(3))
            badge = tk.Canvas(line, width=px(24), height=px(24), bg=box.fill, highlightthickness=0)
            badge.create_oval(1, 1, px(24) - 1, px(24) - 1, fill=C.secondary_container, outline="")
            badge.create_text(px(12), px(12), text=str(index), font=m.font("label"), fill=C.on_secondary_container)
            badge.pack(side="left", anchor="n", padx=(0, px(12)))
            m.Label(line, text, "body_l", wrap=True).pack(side="left", fill="x", expand=True)
        bar = m.LinearProgress(col)
        bar.pack(fill="x", pady=(px(14), 0))
        bar.start()

    def _list(self) -> None:
        app = self.app
        m.Label(self.holder, "Phones found" if len(app.phones) > 1 else "Phone found", "title").pack(
            fill="x", pady=(px(4), px(8)))
        selected = app.selected_phone()
        for phone in app.phones:
            problem = phone.problem()
            if problem:
                subtitle = problem
            else:
                subtitle = f"Connected and ready  ·  Android API {phone.sdk}  ·  {phone.abi}"
            tile = m.ChoiceTile(self.holder, phone.label, subtitle, on_click=lambda p=phone: app.pick_phone(p))
            tile.set(selected=phone is selected, enabled=len(app.phones) > 1 or True)
            tile.pack(fill="x", pady=px(4))
        if not app.has_helper_lib:
            tk.Frame(self.holder, bg=C.surface, height=px(8)).pack()
            self._note("The first build needs this phone", "It copies one file from the phone, so keep it plugged in "
                       "until the first build is done.", "info")

    def _skip(self) -> None:
        row = tk.Frame(self.holder, bg=C.surface)
        row.pack(fill="x", pady=(px(14), 0))
        if self.app.has_helper_lib:
            m.Label(row, "Not ready to plug it in?", "body", C.on_surface_variant).pack(side="left")
            m.Button(row, "Build the APK only", self.app.skip_phone, kind="text", compact=True).pack(side="left")
        else:
            m.Label(row, "The first build needs a phone, because it copies one file from it.", "small",
                    C.on_surface_variant).pack(side="left")

    def footer(self) -> Footer:
        return Footer("Review", self.app.phone_ok, self.app.use_phone, "Back", lambda: self.app.go(1))

    def hint(self) -> str:
        if self.app.phone_ok:
            return "Your phone is ready."
        phone = self.app.selected_phone()
        return phone.problem() if phone and phone.problem() else "Waiting for a phone."


# ====================================================================================================== 4. review
class ReviewPage(Page):
    title = "Ready to build"
    subtitle = "Check the summary, pick what to do, then start."

    def build(self) -> None:
        self.summary = m.card(self.frame, radius=20, pad=(24, 18))
        self.summary.pack(fill="x", pady=(px(4), px(14)))
        self.summary_rows = tk.Frame(self.summary.body, bg=self.summary.fill)
        self.summary_rows.pack(fill="x")

        options = m.card(self.frame, radius=20, pad=(24, 18))
        options.pack(fill="x")
        m.Label(options.body, "What should happen", "title").pack(fill="x", pady=(0, px(8)))
        self.tile_install = m.ChoiceTile(options.body, "Build and install on my phone", "", lambda: self.pick(True))
        self.tile_install.pack(fill="x", pady=px(4))
        self.tile_apk = m.ChoiceTile(options.body, "Only build the APK file",
                                     "Saves an app file you can copy to a phone yourself.", lambda: self.pick(False))
        self.tile_apk.pack(fill="x", pady=px(4))
        tk.Frame(options.body, bg=C.outline_variant, height=1).pack(fill="x", pady=(px(14), px(10)))
        line = tk.Frame(options.body, bg=options.fill)
        line.pack(fill="x")
        m.Switch(line, self.app.skip_splash, self.app.remember_splash).pack(side="right", padx=(px(16), 0))
        col = tk.Frame(line, bg=options.fill)
        col.pack(side="left", fill="x", expand=True)
        m.Label(col, "Skip the launch video", "subtitle").pack(fill="x")
        m.Label(col, "The game starts at its main menu. Turn this off to let the launch video play first.", "body",
                C.on_surface_variant, wrap=True).pack(fill="x")

    def pick(self, install: bool) -> None:
        self.app.install_mode = install
        self.refresh()
        self.app.update_actions()

    def _summary_line(self, status: str, title: str, detail: str) -> None:
        row = tk.Frame(self.summary_rows, bg=self.summary.fill)
        row.pack(fill="x", pady=px(6))
        m.StatusIcon(row, status).pack(side="left", anchor="n", padx=(0, px(14)))
        col = tk.Frame(row, bg=self.summary.fill)
        col.pack(side="left", fill="x", expand=True)
        m.Label(col, title, "subtitle").pack(fill="x")
        m.Label(col, detail, "body", C.on_surface_variant, wrap=True).pack(fill="x")

    def refresh(self) -> None:
        app = self.app
        self.clear(self.summary_rows)
        self._summary_line("ok" if app.computer_ok else "bad", "This computer",
                           "Everything is set up." if app.computer_ok else "Something is missing. Go back to step 1.")
        if app.game is not None:
            self._summary_line("ok" if app.game_ok else "warn", app.game.label,
                               f"{app.package}" + (f"  ·  version {app.game.version}" if app.game.version else ""))
        else:
            self._summary_line("bad", "No game chosen", "Go back to step 2.")
        phone = app.selected_phone()
        if app.phone_ok:
            self._summary_line("ok", phone.label, "Connected and ready.")
        else:
            self._summary_line("info", "No phone", "You can still build the APK and copy it over yourself.")
        install_ok = app.phone_ok
        self.tile_install.set(subtitle=(f"Puts the game on {phone.label} and starts it." if install_ok
                                        else "Connect a phone to use this."),
                              selected=app.install_mode and install_ok, enabled=install_ok)
        self.tile_apk.set(selected=not (app.install_mode and install_ok), enabled=True)

    def footer(self) -> Footer:
        install = self.app.install_mode and self.app.phone_ok
        ready = self.app.computer_ok and self.app.game_ok and self.app.job is None
        return Footer("Build and install" if install else "Build the APK", ready,
                      lambda: self.app.build(install), "Back", lambda: self.app.go(2))

    def hint(self) -> str:
        if self.app.has_helper_lib:
            return "The first build takes a few minutes. Later ones are faster."
        return "The first build takes a few minutes."


# ====================================================================================================== 5. build
class BuildPage(Page):
    title = "Building"
    subtitle = "Keep this window open. You can watch the details if you are curious."

    def build(self) -> None:
        self.card = m.card(self.frame, radius=24, pad=(28, 24))
        self.card.pack(fill="x", pady=(px(4), px(14)))
        body = self.card.body
        self.heading = m.Label(body, "Building your app", "title", wrap=True)
        self.heading.pack(fill="x")
        self.detail = m.Label(body, "", "body_l", C.on_surface_variant, wrap=True)
        self.detail.pack(fill="x", pady=(px(2), px(16)))
        self.bar = m.LinearProgress(body, height=8)
        self.bar.pack(fill="x")
        meta = tk.Frame(body, bg=self.card.fill)
        meta.pack(fill="x", pady=(px(10), 0))
        self.elapsed = m.Label(meta, "Elapsed 0:00", "body", C.on_surface_variant)
        self.elapsed.pack(side="left")
        m.Button(meta, "Show details", self.app.toggle_details, kind="text", compact=True).pack(side="right")

        self.stages = m.card(self.frame, radius=20, pad=(24, 18))
        self.stages.pack(fill="x", pady=(0, px(14)))
        self.stage_rows = tk.Frame(self.stages.body, bg=self.stages.fill)
        self.stage_rows.pack(fill="x")

        self.error = m.Surface(self.frame, fill=C.error_container, radius=20, outline=None, pad=(24, 18))
        self.error_title = m.Label(self.error.body, "", "title", C.on_error_container, wrap=True)
        self.error_title.pack(fill="x")
        self.error_text = m.Label(self.error.body, "", "body_l", C.on_error_container, wrap=True)
        self.error_text.pack(fill="x", pady=(px(4), px(10)))
        m.Button(self.error.body, "Copy details", self.app.copy_details, kind="danger", compact=True).pack(anchor="w")
        self._tick_job = None

    def on_show(self) -> None:
        self.bar.start()
        self.detail.config(text="The first build takes a few minutes…")
        self.refresh()
        self._tick()

    def _tick(self) -> None:
        if self._tick_job:
            self.frame.after_cancel(self._tick_job)
            self._tick_job = None
        app = self.app
        if app.build_state == "running":
            seconds = int(time.monotonic() - app.build_started)
            self.elapsed.config(text=f"Elapsed {seconds // 60}:{seconds % 60:02d}")
            self._tick_job = self.frame.after(1000, self._tick)

    def on_progress(self, fraction, text) -> None:
        if self.app.job_kind != "build":
            return
        if fraction is None:
            self.bar.start()
        else:
            self.bar.set(fraction)
        if text:
            self.detail.config(text=text)
        self.refresh_stages()

    def refresh_stages(self) -> None:
        app = self.app
        plan = BUILD_PLAN if app.build_install else BUILD_PLAN[:2]
        self.clear(self.stage_rows)
        active_found = False
        for name, threshold in plan:
            done = app.stage >= threshold
            if done:
                kind = "ok"
            elif not active_found and app.build_state == "running":
                kind, active_found = "active", True
            elif not active_found and app.build_state in ("failed", "cancelled"):
                kind, active_found = "bad" if app.build_state == "failed" else "warn", True
            else:
                kind = "pending"
            row = tk.Frame(self.stage_rows, bg=self.stages.fill)
            row.pack(fill="x", pady=px(5))
            m.StatusIcon(row, kind, 24).pack(side="left", padx=(0, px(12)))
            m.Label(row, name, "body_l", C.on_surface if kind in ("ok", "active") else C.on_surface_variant).pack(
                side="left")

    def refresh(self) -> None:
        app = self.app
        state = app.build_state
        self.refresh_stages()
        if state == "running":
            self.heading.config(text="Building your app")
            self.error.pack_forget()
        elif state == "failed":
            self.bar.stop()
            self.heading.config(text="The build stopped")
            self.detail.config(text="Nothing was installed.")
            self.error_title.config(text="What went wrong")
            self.error_text.config(text=app.build_error)
            self.error.pack(fill="x")
        elif state == "cancelled":
            self.bar.stop()
            self.heading.config(text="Build cancelled")
            self.detail.config(text="Nothing was installed.")
            self.error.pack_forget()

    def footer(self) -> Footer:
        if self.app.build_state == "running":
            return Footer()
        return Footer("Try again", self.app.job is None, lambda: self.app.build(self.app.build_install),
                      "Back to review", lambda: self.app.go(3))

    def hint(self) -> str:
        return ""


# ====================================================================================================== 6. play
class DonePage(Page):
    title = "All done"
    header = False

    def build(self) -> None:
        self.holder = tk.Frame(self.frame, bg=C.surface)
        self.holder.pack(fill="x")

    def refresh(self) -> None:
        self.clear(self.holder)
        app = self.app
        result = app.build_result
        if result is None:
            return
        install = app.build_install
        minutes = f"{result / 60:.0f} min" if result >= 90 else f"{result:.0f} s"
        hero = m.Surface(self.holder, fill=C.primary_container, radius=28, outline=None, pad=(30, 28))
        hero.pack(fill="x", pady=(px(28), px(14)))
        row = tk.Frame(hero.body, bg=C.primary_container)
        row.pack(fill="x")
        badge = tk.Canvas(row, width=px(64), height=px(64), bg=C.primary_container, highlightthickness=0)
        badge.create_oval(0, 0, px(64), px(64), fill=C.primary, outline="")
        badge.create_line(px(19), px(33), px(28), px(42), px(46), px(23), fill=C.on_primary, width=px(5),
                          capstyle="round", joinstyle="round")
        badge.pack(side="left", anchor="n")
        col = tk.Frame(row, bg=C.primary_container)
        col.pack(side="left", fill="x", expand=True, padx=(px(20), 0))
        label = app.game.label if app.game else "Your game"
        if install:
            phone = app.selected_phone()
            m.Label(col, f"{label} is on your phone", "display", C.on_primary_container, wrap=True).pack(fill="x")
            m.Label(col, f"Built and installed in {minutes}. It is starting on "
                         f"{phone.label if phone else 'your phone'} now.", "body_l", C.on_primary_container,
                    wrap=True).pack(fill="x", pady=(px(6), 0))
        else:
            m.Label(col, "Your APK is ready", "display", C.on_primary_container, wrap=True).pack(fill="x")
            m.Label(col, f"Built in {minutes}. Copy it to a 64-bit Android 12+ phone and open it to install.",
                    "body_l", C.on_primary_container, wrap=True).pack(fill="x", pady=(px(6), 0))

        if app.crashed:
            banner = m.Surface(self.holder, fill=C.error_container, radius=20, outline=None, pad=(24, 16))
            banner.pack(fill="x", pady=(0, px(14)))
            m.Label(banner.body, "The game closed right after it started", "subtitle", C.on_error_container,
                    wrap=True).pack(fill="x")
            m.Label(banner.body, "The game's own log shows where it stopped.", "body", C.on_error_container,
                    wrap=True).pack(fill="x", pady=(0, px(8)))
            m.Button(banner.body, "See what it reported", app.show_phone_log, kind="danger", compact=True).pack(
                anchor="w")

        actions = m.card(self.holder, radius=20, pad=(24, 20))
        actions.pack(fill="x")
        body = actions.body
        busy = app.job is not None
        buttons = tk.Frame(body, bg=actions.fill)
        if install:
            m.Label(body, "What next?", "title").pack(fill="x", pady=(0, px(10)))
            buttons.pack(fill="x")
            start = m.Button(buttons, "Start the game", app.start_game, kind="filled")
            start.set_enabled(not busy)
            start.pack(side="left")
            logs = m.Button(buttons, "Game log", app.show_phone_log, kind="tonal")
            logs.set_enabled(not busy)
            logs.pack(side="left", padx=(px(10), 0))
            remove = m.Button(buttons, "Remove from phone", app.uninstall, kind="danger_text")
            remove.set_enabled(not busy)
            remove.pack(side="right")
        else:
            m.Label(body, "Where is it?", "title").pack(fill="x")
            m.Label(body, app.apk_path(), "body", C.on_surface_variant, wrap=True).pack(fill="x", pady=(px(2), px(12)))
            buttons.pack(fill="x")
            m.Button(buttons, "Save a copy…", app.save_apk, kind="filled").pack(side="left")
            m.Button(buttons, "Show in folder", app.show_apk_folder, kind="tonal").pack(side="left", padx=(px(10), 0))

    def footer(self) -> Footer:
        return Footer("Done", True, self.app.on_close, "Build another game", lambda: self.app.go(1))

    def hint(self) -> str:
        return ""
