"""A small Material 3 toolkit for tkinter: colour roles, type scale, rounded surfaces, buttons, switch, progress
bar, step rail, dialogs and snackbar. Pure tkinter (no extra packages), so the packaged exe stays small.

Call `init(root)` once after creating the Tk root. All sizes are given in 96-dpi pixels and scaled by `px()`.
"""
from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont
from tkinter import ttk
from typing import Callable


# ---------------------------------------------------------------------------------------------- colour roles
class C:
    """Light scheme from a green seed (Android green). Names follow the Material 3 colour roles."""
    primary = "#006D45"
    on_primary = "#FFFFFF"
    primary_container = "#8FF7C0"
    on_primary_container = "#002112"
    secondary = "#4D6357"
    secondary_container = "#D0E8D9"
    on_secondary_container = "#0A1F15"
    tertiary = "#3B6470"
    tertiary_container = "#C3E9F7"
    on_tertiary_container = "#001F28"
    error = "#BA1A1A"
    on_error = "#FFFFFF"
    error_container = "#FFDAD6"
    on_error_container = "#410002"
    warn = "#7C5800"
    warn_container = "#FFDEA6"
    on_warn_container = "#271900"
    surface = "#F5FBF5"
    surface_lowest = "#FFFFFF"
    surface_low = "#EFF6EF"
    surface_container = "#E9F0E9"
    surface_high = "#E3EAE3"
    surface_highest = "#DDE4DD"
    on_surface = "#171D19"
    on_surface_variant = "#3F4943"
    outline = "#6F7973"
    outline_variant = "#BFC9C1"
    inverse_surface = "#2C322E"
    inverse_on_surface = "#EDF2EC"
    log_bg = "#171D19"
    log_fg = "#D6E4DA"


def mix(a: str, b: str, amount: float) -> str:
    """Blends colour `a` toward colour `b` (the Material 'state layer')."""
    pa = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    pb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * amount):02x}" for x, y in zip(pa, pb))


# ---------------------------------------------------------------------------------------------- scale and type
SCALE = 1.0
FAMILY = "TkDefaultFont"
MEDIUM = ""          # family used for medium weight (empty: use bold of FAMILY)
MONO = "TkFixedFont"

ROLES = {            # role: (point size, weight)
    "display": (24, "medium"),
    "headline": (19, "medium"),
    "title": (13, "medium"),
    "subtitle": (11, "medium"),
    "body_l": (11, "regular"),
    "body": (10, "regular"),
    "label": (10, "medium"),
    "small": (9, "regular"),
}
_measure_cache: dict = {}


def px(value: float) -> int:
    return int(round(value * SCALE))


def font(role: str = "body") -> tuple:
    size, weight = ROLES[role]
    if weight == "medium":
        return (MEDIUM, size) if MEDIUM else (FAMILY, size, "bold")
    return (FAMILY, size)


def text_width(text: str, spec: tuple) -> int:
    key = (text, spec)
    if key not in _measure_cache:
        _measure_cache[key] = tkfont.Font(font=spec).measure(text)
    return _measure_cache[key]


def init(root: tk.Tk) -> None:
    global SCALE, FAMILY, MEDIUM, MONO
    SCALE = root.winfo_fpixels("1i") / 96
    families = set(tkfont.families(root))
    FAMILY = next((f for f in ("Roboto", "Segoe UI Variable Text", "Segoe UI", "Helvetica Neue", "Noto Sans",
                               "DejaVu Sans") if f in families), "TkDefaultFont")
    MEDIUM = next((f for f in (FAMILY + " Semibold", "Segoe UI Semibold", "Roboto Medium") if f in families
                   and f.startswith(FAMILY.split()[0])), "")
    MONO = next((f for f in ("Cascadia Mono", "Consolas", "DejaVu Sans Mono", "Courier New") if f in families),
                "TkFixedFont")
    style = ttk.Style(root)
    style.theme_use("clam")
    for name, trough, thumb, hover in (("M3", C.surface, C.outline_variant, C.outline),
                                       ("M3Dark", C.log_bg, "#4B554F", "#6F7973")):
        full = f"{name}.Vertical.TScrollbar"
        style.layout(full, [("Vertical.Scrollbar.trough", {"sticky": "ns", "children": [
            ("Vertical.Scrollbar.thumb", {"expand": "1", "sticky": "nswe"})]})])
        style.configure(full, troughcolor=trough, background=thumb, bordercolor=trough, lightcolor=thumb,
                        darkcolor=thumb, width=px(10), relief="flat", borderwidth=0)
        style.map(full, background=[("disabled", trough), ("active", hover)], lightcolor=[("disabled", trough),
                  ("active", hover)], darkcolor=[("disabled", trough), ("active", hover)])
    root.bind_all("<MouseWheel>", _wheel)
    root.bind_all("<Button-4>", _wheel)
    root.bind_all("<Button-5>", _wheel)


def _wheel(event) -> None:
    widget = event.widget.winfo_containing(event.x_root, event.y_root)
    while widget is not None:
        if isinstance(widget, tk.Text):
            return
        target = getattr(widget, "_m3_scroll", None)
        if target is not None:
            target.scroll(event)
            return
        widget = widget.master


# ---------------------------------------------------------------------------------------------- drawing helpers
def round_rect(canvas: tk.Canvas, x1, y1, x2, y2, radius, **kw) -> int:
    r = max(0, min(radius, (x2 - x1) / 2, (y2 - y1) / 2))
    points = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2, x1, y2,
              x1, y2 - r, x1, y1 + r, x1, y1]
    return canvas.create_polygon(points, smooth=True, splinesteps=14, **kw)


def draw_gamepad(canvas: tk.Canvas, cx: float, cy: float, size: float, color: str, cut: str) -> None:
    s = size
    round_rect(canvas, cx - .5 * s, cy - .30 * s, cx + .5 * s, cy + .30 * s, .28 * s, fill=color, outline="")
    arm = .07 * s
    canvas.create_rectangle(cx - .31 * s - arm, cy - .13 * s, cx - .31 * s + arm, cy + .13 * s, fill=cut, outline="")
    canvas.create_rectangle(cx - .31 * s - .13 * s, cy - arm, cx - .31 * s + .13 * s, cy + arm, fill=cut, outline="")
    for dx, dy in ((.20, -.07), (.31, .06)):
        canvas.create_oval(cx + dx * s - .06 * s, cy + dy * s - .06 * s, cx + dx * s + .06 * s, cy + dy * s + .06 * s,
                           fill=cut, outline="")


def draw_phone(canvas: tk.Canvas, x: float, y: float, w: float, h: float, body: str, screen: str, accent: str) -> None:
    round_rect(canvas, x, y, x + w, y + h, .16 * w, fill=body, outline="")
    inset = .09 * w
    round_rect(canvas, x + inset, y + inset * 1.7, x + w - inset, y + h - inset * 1.7, .09 * w, fill=screen, outline="")
    cx, cy, t = x + w / 2, y + h / 2, .16 * w
    canvas.create_polygon(cx - t * .6, cy - t, cx - t * .6, cy + t, cx + t, cy, fill=accent, outline="")


def draw_bridge(canvas: tk.Canvas, w: float, h: float) -> None:
    """Welcome illustration: a game on the left, a dashed hop to an Android phone on the right."""
    bg = canvas.cget("bg")
    tile = mix(bg, "#FFFFFF", .55)
    size = .42 * w
    round_rect(canvas, 0, h / 2 - size / 2, size, h / 2 + size / 2, size * .26, fill=tile, outline="")
    draw_gamepad(canvas, size / 2, h / 2, size * .62, C.on_primary_container, tile)
    px_w = .27 * w
    draw_phone(canvas, w - px_w, h / 2 - .46 * h, px_w, .92 * h, C.primary, tile, C.primary)
    x1, x2 = size + px(8), w - px_w - px(8)
    canvas.create_line(x1, h / 2, x2 - px(6), h / 2, fill=C.primary, width=max(2, px(2)), dash=(px(5), px(4)))
    canvas.create_polygon(x2, h / 2, x2 - px(9), h / 2 - px(6), x2 - px(9), h / 2 + px(6), fill=C.primary, outline="")


# ---------------------------------------------------------------------------------------------- text
class Label(tk.Label):
    """`wrap=True` re-wraps to the width the label is given (use with fill="x"); an int wraps at that many px."""

    def __init__(self, parent, text: str = "", role: str = "body", color: str | None = None, wrap=False, **kw):
        bg = kw.pop("bg", None) or parent.cget("bg")
        super().__init__(parent, text=text, bg=bg, fg=color or C.on_surface, font=font(role), justify="left",
                         anchor="w", bd=0, **kw)
        if wrap is True:
            self.configure(width=1)
            self.bind("<Configure>", self._rewrap)
        elif wrap:
            self.configure(wraplength=int(wrap))

    def _rewrap(self, event) -> None:
        target = max(20, event.width - 2)
        if abs(int(float(str(self.cget("wraplength")) or 0)) - target) > 1:
            self.configure(wraplength=target)


# ---------------------------------------------------------------------------------------------- surfaces
class Surface(tk.Canvas):
    """A rounded container. Put children into `.body`; the surface grows with them."""

    def __init__(self, parent, fill: str | None = None, radius: int = 16, outline="default", pad=(20, 16),
                 dash=None):
        self.fill = fill or C.surface_lowest
        self.radius = px(radius)
        self.outline = C.outline_variant if outline == "default" else outline
        self.dash = dash
        self.padx, self.pady = px(pad[0]), px(pad[1])
        super().__init__(parent, bg=parent.cget("bg"), highlightthickness=0, bd=0, height=2 * self.pady + px(8))
        self.body = tk.Frame(self, bg=self.fill)
        self._win = self.create_window(self.padx, self.pady, window=self.body, anchor="nw")
        self.bind("<Configure>", self._on_canvas)
        self.body.bind("<Configure>", self._on_body)

    def _on_canvas(self, event) -> None:
        self.itemconfigure(self._win, width=max(1, event.width - 2 * self.padx))
        self.delete("bg")
        inset = 1 if self.outline else 0
        extra = {"outline": self.outline, "width": max(1, px(1)) if not self.dash else max(2, px(2))} \
            if self.outline else {"outline": ""}
        if self.dash:
            extra["dash"] = (px(6), px(5))
        item = round_rect(self, inset, inset, event.width - inset, event.height - inset, self.radius,
                          fill=self.fill, tags="bg", **extra)
        self.tag_lower(item)

    def _on_body(self, _event) -> None:
        want = self.body.winfo_reqheight() + 2 * self.pady
        if want != int(self.cget("height")):
            self.configure(height=want)


def card(parent, **kw) -> Surface:
    return Surface(parent, **kw)


class Chip(tk.Canvas):
    KINDS = {"ok": (C.primary_container, C.on_primary_container), "bad": (C.error_container, C.on_error_container),
             "warn": (C.warn_container, C.on_warn_container), "info": (C.surface_high, C.on_surface_variant)}

    def __init__(self, parent, text: str, kind: str = "info"):
        fill, fg = self.KINDS[kind]
        spec = font("label")
        w, h = text_width(text, spec) + px(24), px(28)
        super().__init__(parent, bg=parent.cget("bg"), width=w, height=h, highlightthickness=0, bd=0)
        round_rect(self, 0, 0, w, h, px(8), fill=fill, outline="")
        self.create_text(w / 2, h / 2, text=text, font=spec, fill=fg)


class StatusIcon(tk.Canvas):
    """A round status glyph drawn with lines, so it looks the same in every font."""

    def __init__(self, parent, kind: str, size: int = 28):
        s = px(size)
        super().__init__(parent, bg=parent.cget("bg"), width=s, height=s, highlightthickness=0, bd=0)
        lw = max(2, px(size / 11))

        def pt(x, y):
            return x * s, y * s

        def line(*coords, color):
            flat = [v for xy in coords for v in pt(*xy)]
            self.create_line(*flat, fill=color, width=lw, capstyle="round", joinstyle="round")

        def disc(fill, outline=""):
            self.create_oval(1, 1, s - 1, s - 1, fill=fill, outline=outline, width=max(2, px(2)))

        if kind == "ok":
            disc(C.primary_container)
            line((.28, .52), (.44, .67), (.73, .35), color=C.primary)
        elif kind == "bad":
            disc(C.error_container)
            line((.34, .34), (.66, .66), color=C.error)
            line((.66, .34), (.34, .66), color=C.error)
        elif kind == "warn":
            disc(C.warn_container)
            line((.5, .27), (.5, .58), color=C.warn)
            self.create_oval(s * .5 - lw / 2, s * .70 - lw / 2, s * .5 + lw / 2, s * .70 + lw / 2, fill=C.warn, outline="")
        elif kind == "active":
            disc("", C.primary)
            self.create_oval(s * .34, s * .34, s * .66, s * .66, fill=C.primary, outline="")
        elif kind == "pending":
            disc("", C.outline_variant)
        else:
            disc(C.surface_high)
            self.create_oval(s * .5 - lw / 2, s * .30 - lw / 2, s * .5 + lw / 2, s * .30 + lw / 2,
                             fill=C.on_surface_variant, outline="")
            line((.5, .46), (.5, .72), color=C.on_surface_variant)


# ---------------------------------------------------------------------------------------------- buttons
class Button(tk.Canvas):
    # kind: (container, content, outline, state-layer colour)
    KINDS = {
        "filled": (C.primary, C.on_primary, None, C.on_primary),
        "tonal": (C.secondary_container, C.on_secondary_container, None, C.on_secondary_container),
        "outlined": (None, C.primary, C.outline, C.primary),
        "text": (None, C.primary, None, C.primary),
        "danger": (C.error_container, C.on_error_container, None, C.on_error_container),
        "danger_text": (None, C.error, None, C.error),
    }

    def __init__(self, parent, text: str, command: Callable | None = None, kind: str = "filled",
                 compact: bool = False, min_width: int = 0):
        self.parent_bg = parent.cget("bg")
        super().__init__(parent, bg=self.parent_bg, highlightthickness=0, bd=0, takefocus=1, cursor="hand2")
        self.text, self.command, self.kind, self.compact = text, command, kind, compact
        self.min_width = px(min_width)
        self.enabled, self._hover, self._down, self._focus = True, False, False, False
        self.bw = self.bh = 1
        self._fit()
        for sequence, handler in (("<Enter>", self._enter), ("<Leave>", self._leave), ("<ButtonPress-1>", self._press),
                                  ("<ButtonRelease-1>", self._release), ("<FocusIn>", self._focus_in),
                                  ("<FocusOut>", self._focus_out), ("<space>", self._key), ("<Return>", self._key),
                                  ("<Configure>", self._on_configure)):
            self.bind(sequence, handler)

    def _fit(self) -> None:
        pad = px(12 if self.kind in ("text", "danger_text") else 14 if self.compact else 24)
        self.bh = px(32 if self.compact else 40)
        self.bw = max(self.min_width, text_width(self.text, font("label")) + 2 * pad)
        self.configure(width=self.bw, height=self.bh)
        self.draw()

    def set_text(self, text: str) -> None:
        if text != self.text:
            self.text = text
            self._fit()

    def set_enabled(self, enabled: bool) -> None:
        if enabled != self.enabled:
            self.enabled = enabled
            self.configure(cursor="hand2" if enabled else "arrow", takefocus=1 if enabled else 0)
            self.draw()

    def draw(self) -> None:
        self.delete("all")
        w, h, pb = self.bw, self.bh, self.parent_bg
        fill, fg, outline, layer = self.KINDS[self.kind]
        if not self.enabled:
            fill = mix(pb, C.on_surface, .12) if fill else None
            fg = mix(pb, C.on_surface, .38)
            outline = mix(pb, C.on_surface, .12) if outline else None
        else:
            amount = .12 if self._down else .08 if self._hover else .10 if self._focus else 0
            if amount:
                fill = mix(fill or pb, layer, amount)
        extra = {"outline": outline, "width": max(1, px(1))} if outline else {"outline": ""}
        round_rect(self, 1, 1, w - 1, h - 1, h / 2, fill=fill or "", **extra)
        if self._focus and self.enabled:
            round_rect(self, 1, 1, w - 1, h - 1, h / 2, fill="", outline=C.primary, width=max(2, px(2)))
        self.create_text(w / 2, h / 2, text=self.text, font=font("label"), fill=fg)

    def invoke(self) -> None:
        if self.enabled and self.command:
            self.command()

    def _enter(self, _e): self._hover = True; self.draw()
    def _leave(self, _e): self._hover = self._down = False; self.draw()
    def _press(self, _e):
        if self.enabled:
            self._down = True
            self.draw()
    def _focus_in(self, _e): self._focus = True; self.draw()
    def _focus_out(self, _e): self._focus = False; self.draw()
    def _key(self, _e): self.invoke()

    def _release(self, event) -> None:
        was_down, self._down = self._down, False
        self.draw()
        if was_down and 0 <= event.x <= self.bw and 0 <= event.y <= self.bh:
            self.invoke()

    def _on_configure(self, event) -> None:
        if (event.width, event.height) != (self.bw, self.bh):
            self.bw, self.bh = event.width, event.height
            self.draw()


class Switch(tk.Canvas):
    def __init__(self, parent, variable: tk.BooleanVar, command: Callable | None = None):
        self.var, self.command = variable, command
        self.w, self.h = px(52), px(32)
        super().__init__(parent, bg=parent.cget("bg"), width=self.w, height=self.h, highlightthickness=0, bd=0,
                         cursor="hand2", takefocus=1)
        self.bind("<Button-1>", self.toggle)
        self.bind("<space>", self.toggle)
        self.var.trace_add("write", lambda *_: self.draw())
        self.draw()

    def toggle(self, _event=None) -> None:
        self.var.set(not self.var.get())
        if self.command:
            self.command()

    def draw(self) -> None:
        self.delete("all")
        on, w, h = bool(self.var.get()), self.w, self.h
        if on:
            round_rect(self, 1, 1, w - 1, h - 1, h / 2, fill=C.primary, outline="")
            r, cx = px(12), w - px(16)
            self.create_oval(cx - r, h / 2 - r, cx + r, h / 2 + r, fill=C.on_primary, outline="")
        else:
            round_rect(self, 2, 2, w - 2, h - 2, h / 2, fill=C.surface_highest, outline=C.outline, width=max(2, px(2)))
            r, cx = px(8), px(16)
            self.create_oval(cx - r, h / 2 - r, cx + r, h / 2 + r, fill=C.outline, outline="")


class ChoiceTile(tk.Canvas):
    """A selectable row with a radio mark, a title and a supporting line."""

    def __init__(self, parent, title: str, subtitle: str = "", on_click: Callable | None = None, height: int = 68):
        self.parent_bg = parent.cget("bg")
        super().__init__(parent, bg=self.parent_bg, highlightthickness=0, bd=0, height=px(height), cursor="hand2",
                         takefocus=1)
        self.title, self.subtitle, self.on_click = title, subtitle, on_click
        self.selected, self.enabled, self._hover, self.cw = False, True, False, 1
        self.bind("<Configure>", lambda e: (setattr(self, "cw", e.width), self.draw()))
        self.bind("<Enter>", lambda _e: (setattr(self, "_hover", True), self.draw()))
        self.bind("<Leave>", lambda _e: (setattr(self, "_hover", False), self.draw()))
        self.bind("<Button-1>", self._click)
        self.bind("<space>", self._click)

    def set(self, title=None, subtitle=None, selected=None, enabled=None) -> None:
        if title is not None: self.title = title
        if subtitle is not None: self.subtitle = subtitle
        if selected is not None: self.selected = selected
        if enabled is not None:
            self.enabled = enabled
            self.configure(cursor="hand2" if enabled else "arrow")
        self.draw()

    def _click(self, _e) -> None:
        if self.enabled and self.on_click:
            self.on_click()

    def draw(self) -> None:
        self.delete("all")
        w, h = self.cw, int(self.cget("height"))
        fill = C.secondary_container if self.selected else C.surface_lowest
        if self._hover and self.enabled and not self.selected:
            fill = mix(fill, C.primary, .06)
        outline = C.primary if self.selected else C.outline_variant
        fg, sub = C.on_surface, C.on_surface_variant
        if not self.enabled:
            fill, outline, fg, sub = C.surface_low, C.outline_variant, mix(C.surface, C.on_surface, .38), \
                mix(C.surface, C.on_surface, .38)
        round_rect(self, 1, 1, w - 1, h - 1, px(14), fill=fill, outline=outline, width=2 if self.selected else 1)
        cx, r = px(30), px(10)
        ring = C.primary if self.selected and self.enabled else C.outline if self.enabled else outline
        self.create_oval(cx - r, h / 2 - r, cx + r, h / 2 + r, outline=ring, width=2, fill="")
        if self.selected:
            d = px(5)
            self.create_oval(cx - d, h / 2 - d, cx + d, h / 2 + d, fill=ring, outline="")
        left, wrap = cx + px(24), max(40, w - cx - px(24) - px(16))
        self.create_text(left, px(14), text=self.title, anchor="nw", font=font("subtitle"), fill=fg, width=wrap)
        if self.subtitle:
            self.create_text(left, px(36), text=self.subtitle, anchor="nw", font=font("body"), fill=sub, width=wrap)


# ---------------------------------------------------------------------------------------------- progress
class LinearProgress(tk.Canvas):
    def __init__(self, parent, height: int = 6):
        self.parent_bg = parent.cget("bg")
        super().__init__(parent, bg=self.parent_bg, highlightthickness=0, bd=0, height=px(height))
        self.value, self.animating, self._phase, self._after_id, self.cw = 0.0, False, 0.0, None, 1
        self.bind("<Configure>", lambda e: (setattr(self, "cw", e.width), self.draw()))
        self.bind("<Destroy>", lambda _e: self.stop())

    def set(self, value: float) -> None:
        self.stop()
        self.value = max(0.0, min(1.0, value))
        self.draw()

    def start(self) -> None:
        if self.animating:
            return
        self.animating = True
        self._tick()

    def stop(self) -> None:
        self.animating = False
        if self._after_id:
            try:
                self.after_cancel(self._after_id)
            except tk.TclError:
                pass
            self._after_id = None

    def _tick(self) -> None:
        if not self.animating:
            return
        self._phase = (self._phase + .018) % 1.0
        self.draw()
        self._after_id = self.after(24, self._tick)

    def draw(self) -> None:
        self.delete("all")
        w, h = self.cw, int(self.cget("height"))
        round_rect(self, 0, 0, w, h, h / 2, fill=C.secondary_container, outline="")
        if self.animating:
            span = w * .38
            x0 = -span + (w + span) * self._phase
            a, b = max(0, x0), min(w, x0 + span)
        else:
            a, b = 0, w * self.value
        if b - a >= 1:
            round_rect(self, a, 0, max(b, a + h), h, h / 2, fill=C.primary, outline="")


# ---------------------------------------------------------------------------------------------- scrolling
class ScrollFrame(tk.Frame):
    """A vertically scrolling area. Put children into `.inner`."""

    def __init__(self, parent, bg: str):
        super().__init__(parent, bg=bg)
        self.canvas = tk.Canvas(self, bg=bg, highlightthickness=0, bd=0, yscrollincrement=px(24))
        self.canvas._m3_scroll = self
        self.bar = ttk.Scrollbar(self, orient="vertical", style="M3.Vertical.TScrollbar", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self._on_scroll)
        self.inner = tk.Frame(self.canvas, bg=bg)
        self._win = self.canvas.create_window(0, 0, window=self.inner, anchor="nw")
        self.bar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.inner.bind("<Configure>", lambda _e: self._sync())
        self.canvas.bind("<Configure>", self._on_canvas)

    def _on_canvas(self, event) -> None:
        self.canvas.itemconfigure(self._win, width=event.width)
        self._sync()

    def _sync(self) -> None:
        height = max(self.inner.winfo_reqheight(), self.canvas.winfo_height())
        self.canvas.configure(scrollregion=(0, 0, self.canvas.winfo_width(), height))

    def _on_scroll(self, first, last) -> None:
        self.bar.set(first, last)
        self.bar.state(["disabled"] if float(first) <= 0 and float(last) >= 1 else ["!disabled"])

    def scroll(self, event) -> None:
        first, last = self.canvas.yview()
        if first <= 0 and last >= 1:
            return
        if event.num == 4:
            steps = -2
        elif event.num == 5:
            steps = 2
        else:
            steps = -2 if event.delta > 0 else 2
        self.canvas.yview_scroll(steps, "units")

    def to_top(self) -> None:
        self.canvas.yview_moveto(0)


class LogView(tk.Frame):
    def __init__(self, parent, height: int = 10):
        super().__init__(parent, bg=C.log_bg)
        self.text = tk.Text(self, height=height, font=(MONO, 9), bg=C.log_bg, fg=C.log_fg, insertbackground=C.log_fg,
                            relief="flat", bd=0, padx=px(14), pady=px(10), wrap="word", state="disabled",
                            selectbackground="#2F5E49", highlightthickness=0)
        bar = ttk.Scrollbar(self, orient="vertical", style="M3Dark.Vertical.TScrollbar", command=self.text.yview)
        self.text.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        self.text.pack(side="left", fill="both", expand=True)

    def append(self, lines: list[str]) -> None:
        self.text.config(state="normal")
        self.text.insert("end", "\n".join(lines) + "\n")
        if int(self.text.index("end-1c").split(".")[0]) > 5000:
            self.text.delete("1.0", "1000.0")
        self.text.see("end")
        self.text.config(state="disabled")

    def set_text(self, body: str) -> None:
        self.text.config(state="normal")
        self.text.delete("1.0", "end")
        self.text.insert("end", body)
        self.text.see("end")
        self.text.config(state="disabled")


# ---------------------------------------------------------------------------------------------- step rail
class StepRail(tk.Canvas):
    """Navigation rail: brand on top, then the steps as a vertical stepper."""

    def __init__(self, parent, labels: list[str], command: Callable[[int], None], can_go: Callable[[int], bool],
                 width: int = 216):
        super().__init__(parent, bg=C.surface_container, highlightthickness=0, bd=0, width=px(width))
        self.labels, self.command, self.can_go = labels, command, can_go
        self.current, self.reached = 0, 0
        self.row_h, self.top = px(54), px(96)
        self.bind("<Configure>", lambda _e: self.draw())
        self.bind("<Button-1>", self._click)
        self.bind("<Motion>", lambda e: self.configure(cursor="hand2" if self._hit(e.y) is not None else "arrow"))

    def set_state(self, current: int, reached: int) -> None:
        self.current, self.reached = current, reached
        self.draw()

    def _hit(self, y: int):
        index = (y - self.top) // self.row_h
        if 0 <= index < len(self.labels) and self.can_go(index):
            return index
        return None

    def _click(self, event) -> None:
        index = self._hit(event.y)
        if index is not None:
            self.command(index)

    def draw(self) -> None:
        self.delete("all")
        w = int(self.cget("width"))
        self.create_text(px(28), px(44), text="xnuroid", anchor="w", font=font("headline"), fill=C.on_surface)
        self.create_text(px(29), px(68), text="iOS games on Android", anchor="w", font=font("small"),
                         fill=C.on_surface_variant)
        cx, r = px(40), px(12)
        for i, label in enumerate(self.labels):
            cy = self.top + i * self.row_h + self.row_h / 2
            if i == self.current:
                round_rect(self, px(12), cy - px(22), w - px(12), cy + px(22), px(22), fill=C.secondary_container,
                           outline="")
        for i in range(len(self.labels) - 1):
            cy = self.top + i * self.row_h + self.row_h / 2
            self.create_line(cx, cy + r + px(4), cx, cy + self.row_h - r - px(4), width=2,
                             fill=C.primary if i < self.reached else C.outline_variant)
        for i, label in enumerate(self.labels):
            cy = self.top + i * self.row_h + self.row_h / 2
            done = i < self.reached and i != self.current
            if i == self.current:
                self.create_oval(cx - r, cy - r, cx + r, cy + r, fill=C.primary, outline="")
                self.create_text(cx, cy, text=str(i + 1), font=font("label"), fill=C.on_primary)
            elif done:
                self.create_oval(cx - r, cy - r, cx + r, cy + r, fill=C.primary_container, outline="")
                self.create_line(cx - r * .42, cy + r * .02, cx - r * .1, cy + r * .36, cx + r * .48, cy - r * .32,
                                 fill=C.primary, width=2, capstyle="round", joinstyle="round")
            else:
                self.create_oval(cx - r, cy - r, cx + r, cy + r, outline=C.outline, width=2)
                self.create_text(cx, cy, text=str(i + 1), font=font("label"), fill=C.on_surface_variant)
            bold = i == self.current
            self.create_text(px(66), cy, text=label, anchor="w", font=font("subtitle" if bold else "body_l"),
                             fill=C.on_secondary_container if bold else C.on_surface if done else C.on_surface_variant)


# ---------------------------------------------------------------------------------------------- dialogs
class Dialog(tk.Toplevel):
    """A Material dialog. Fill `.body`, add actions, then call `present()`."""

    def __init__(self, parent, title: str, heading: str | None = None, width: int = 440):
        super().__init__(parent, bg=C.surface_high)
        self.title(title)
        self.transient(parent)
        self.resizable(False, False)
        self.result = None
        self.on_close: Callable | None = None
        self.parent, self.min_w, self.inner_w = parent, px(width), px(width - 48)
        pad = tk.Frame(self, bg=C.surface_high, padx=px(24), pady=px(24))
        pad.pack(fill="both", expand=True)
        if heading:
            Label(pad, heading, role="title", wrap=self.inner_w).pack(fill="x", pady=(0, px(12)))
        self.body = tk.Frame(pad, bg=C.surface_high)
        self.body.pack(fill="both", expand=True)
        self.actions = tk.Frame(pad, bg=C.surface_high)
        self.actions.pack(fill="x", pady=(px(24), 0))
        self.protocol("WM_DELETE_WINDOW", lambda: self.close(None))
        self.bind("<Escape>", lambda _e: self.close(None))

    def add_action(self, text: str, value, kind: str = "text") -> Button:
        button = Button(self.actions, text, command=lambda: self.close(value), kind=kind)
        button.pack(side="right", padx=(px(8), 0))
        return button

    def present(self, wait: bool = False):
        self.update_idletasks()
        w, h = max(self.winfo_reqwidth(), self.min_w), self.winfo_reqheight()
        x = self.parent.winfo_rootx() + (self.parent.winfo_width() - w) // 2
        y = self.parent.winfo_rooty() + max(px(40), (self.parent.winfo_height() - h) // 3)
        self.geometry(f"{w}x{h}+{max(0, x)}+{max(0, y)}")
        try:
            self.wait_visibility()
            self.grab_set()
        except tk.TclError:
            pass
        self.focus_set()
        if wait:
            self.wait_window()
        return self.result

    def close(self, value) -> None:
        if self.result is not None and value is None:
            value = self.result
        self.result = value
        try:
            self.grab_release()
        except tk.TclError:
            pass
        callback = self.on_close
        self.destroy()
        if callback:
            callback(value)


def ask(parent, title: str, body: str, ok: str = "OK", cancel: str | None = None, danger: bool = False,
        heading: str | None = None) -> bool:
    """Shows a modal message. Returns True for the main action."""
    dialog = Dialog(parent, title, heading or title)
    Label(dialog.body, body, role="body_l", color=C.on_surface_variant, wrap=dialog.inner_w).pack(fill="x")
    dialog.add_action(ok, True, kind="danger_text" if danger else "text")
    if cancel:
        dialog.add_action(cancel, False, kind="text")
    return bool(dialog.present(wait=True))


class Snackbar:
    def __init__(self, host: tk.Widget):
        self.host, self.canvas, self._job = host, None, None

    def show(self, text: str, ms: int = 3500) -> None:
        self.hide()
        spec = font("body_l")
        w, h = min(text_width(text, spec) + px(48), self.host.winfo_width() - px(48)), px(48)
        self.canvas = tk.Canvas(self.host, width=w, height=h, bg=self.host.cget("bg"), highlightthickness=0, bd=0)
        round_rect(self.canvas, 0, 0, w, h, px(10), fill=C.inverse_surface, outline="")
        self.canvas.create_text(px(24), h / 2, text=text, anchor="w", font=spec, fill=C.inverse_on_surface,
                                width=w - px(36))
        self.canvas.place(relx=.5, rely=1.0, anchor="s", y=-px(96))
        self._job = self.host.after(ms, self.hide)

    def hide(self) -> None:
        if self._job:
            self.host.after_cancel(self._job)
            self._job = None
        if self.canvas is not None:
            self.canvas.destroy()
            self.canvas = None
