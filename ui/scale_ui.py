#!/usr/bin/env python3
"""NRLive launcher UI — Lossless Scaling–inspired dark theme."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

# ── palette (Lossless Scaling–like) ──────────────────────────────────────────
BG = "#1a1b1e"
BG_SIDE = "#141518"
BG_CARD = "#222328"
BG_INPUT = "#2a2b30"
BG_HOVER = "#2e2f36"
BG_SEL = "#3a2a4a"
ACCENT = "#b44dff"
ACCENT_HOVER = "#c66bff"
TEXT = "#e8e8ec"
TEXT_DIM = "#9a9aa3"
BORDER = "#32333a"
SUCCESS = "#5cde8a"

def app_root() -> Path:
    """Folder containing this UI (script dir, or the .exe dir when frozen)."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


ROOT = app_root()
PROFILES_DIR = ROOT / "profiles"
CONFIG_PATH = ROOT / "ui_config.json"


def default_nrlive_exe() -> str:
    """NRLive.exe next to the UI by default."""
    candidate = ROOT / "NRLive.exe"
    return str(candidate)


DEFAULT_PROFILE = {
    "target_mode": "picker",  # picker | front | pid | pname | window
    "pid": "",
    "process": "",
    "window": "",
    "delay": 5,
    "no_overlay": False,
    "motion": "amdof",  # amdof | fast
    "stop_key": "ctrl+shift+a",
    "overlay_key": "ctrl+home",
    "bind_bypass": "home,insert,end,pageup,pagedown",
    "exe_path": "",  # filled at runtime with default_nrlive_exe()
}


def ensure_dirs() -> None:
    PROFILES_DIR.mkdir(parents=True, exist_ok=True)


def load_ui_config() -> dict:
    if CONFIG_PATH.is_file():
        try:
            return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {"last_profile": "Default", "exe_path": default_nrlive_exe()}


def save_ui_config(cfg: dict) -> None:
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def list_profiles() -> list[str]:
    ensure_dirs()
    names = sorted(p.stem for p in PROFILES_DIR.glob("*.json"))
    if "Default" not in names:
        save_profile("Default", dict(DEFAULT_PROFILE))
        names = sorted(p.stem for p in PROFILES_DIR.glob("*.json"))
    return names


def profile_path(name: str) -> Path:
    safe = re.sub(r'[<>:"/\\|?*]', "_", name.strip()) or "Default"
    return PROFILES_DIR / f"{safe}.json"


def load_profile(name: str) -> dict:
    path = profile_path(name)
    data = dict(DEFAULT_PROFILE)
    if path.is_file():
        try:
            data.update(json.loads(path.read_text(encoding="utf-8")))
        except Exception:
            pass
    return data


def save_profile(name: str, data: dict) -> None:
    ensure_dirs()
    profile_path(name).write_text(json.dumps(data, indent=2), encoding="utf-8")


def delete_profile(name: str) -> None:
    if name == "Default":
        return
    p = profile_path(name)
    if p.is_file():
        p.unlink()


def build_cli_args(data: dict, exe: str) -> list[str]:
    args = [exe]
    mode = data.get("target_mode", "picker")
    if mode == "front":
        args.append("-front")
    elif mode == "pid":
        pid = str(data.get("pid", "")).strip()
        if pid:
            args.extend(["-pid", pid])
    elif mode == "pname":
        proc = str(data.get("process", "")).strip()
        if proc:
            args.extend(["-pname", proc])
    elif mode == "window":
        win = str(data.get("window", "")).strip()
        if win:
            args.extend(["-window", win])
    else:
        args.append("-picker")

    delay = data.get("delay", 0)
    try:
        delay = int(delay)
    except (TypeError, ValueError):
        delay = 0
    if delay > 0:
        args.extend(["-delay", str(delay)])

    if data.get("no_overlay"):
        args.append("-nooverlay")

    mv = data.get("motion", "amdof")
    if mv in ("amdof", "fast"):
        args.extend(["--mv", mv])

    key = str(data.get("stop_key", "")).strip()
    if key:
        args.extend(["--key", key])

    okey = str(data.get("overlay_key", "")).strip()
    if okey:
        args.extend(["--overlaykey", okey])

    bypass = str(data.get("bind_bypass", "")).strip()
    if bypass:
        args.extend(["--bindbypass", bypass])

    return args


class Toggle(tk.Frame):
    """Simple On/Off pill switch."""

    def __init__(self, master, variable: tk.BooleanVar, **kw):
        super().__init__(master, bg=BG_CARD, **kw)
        self.var = variable
        self.canvas = tk.Canvas(
            self, width=44, height=24, bg=BG_CARD, highlightthickness=0, bd=0
        )
        self.canvas.pack()
        self.canvas.bind("<Button-1>", self._toggle)
        self.var.trace_add("write", lambda *_: self._draw())
        self._draw()

    def _toggle(self, _=None):
        self.var.set(not self.var.get())

    def _draw(self):
        self.canvas.delete("all")
        on = self.var.get()
        fill = ACCENT if on else "#4a4b52"
        self.canvas.create_oval(2, 2, 22, 22, fill=fill, outline="")
        self.canvas.create_oval(22, 2, 42, 22, fill=fill, outline="")
        self.canvas.create_rectangle(12, 2, 32, 22, fill=fill, outline="")
        cx = 32 if on else 12
        self.canvas.create_oval(cx - 8, 4, cx + 8, 20, fill="#fff", outline="")


class Card(tk.Frame):
    def __init__(self, master, title: str, **kw):
        super().__init__(master, bg=BG_CARD, **kw)
        tk.Label(
            self,
            text=title,
            bg=BG_CARD,
            fg=TEXT,
            font=("Segoe UI", 11, "bold"),
            anchor="w",
        ).pack(fill="x", padx=14, pady=(12, 8))
        self.body = tk.Frame(self, bg=BG_CARD)
        self.body.pack(fill="both", expand=True, padx=14, pady=(0, 12))


class NRLiveUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("NRLive")
        self.geometry("980x620")
        self.minsize(860, 520)
        self.configure(bg=BG)
        self.ui_cfg = load_ui_config()
        self.current_profile = self.ui_cfg.get("last_profile") or "Default"
        self._vars: dict = {}
        self._building = False

        self._style()
        self._build()
        self._reload_list()
        self._select_profile(self.current_profile)

    def _style(self):
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(
            "Side.TFrame", background=BG_SIDE
        )
        style.configure(
            "TCombobox",
            fieldbackground=BG_INPUT,
            background=BG_INPUT,
            foreground=TEXT,
            arrowcolor=TEXT,
        )
        style.map(
            "TCombobox",
            fieldbackground=[("readonly", BG_INPUT)],
            foreground=[("readonly", TEXT)],
        )

    def _build(self):
        # Top bar
        top = tk.Frame(self, bg=BG, height=52)
        top.pack(fill="x")
        top.pack_propagate(False)
        tk.Label(
            top,
            text="✦  NRLive",
            bg=BG,
            fg=ACCENT,
            font=("Segoe UI", 14, "bold"),
        ).pack(side="left", padx=18, pady=12)

        scale_btn = tk.Button(
            top,
            text="Scale",
            bg=ACCENT,
            fg="#fff",
            activebackground=ACCENT_HOVER,
            activeforeground="#fff",
            font=("Segoe UI", 10, "bold"),
            bd=0,
            padx=22,
            pady=6,
            cursor="hand2",
            command=self._launch,
        )
        scale_btn.pack(side="right", padx=18, pady=10)

        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True)

        # Sidebar
        side = tk.Frame(body, bg=BG_SIDE, width=220)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)

        tk.Label(
            side,
            text="Game Profiles",
            bg=BG_SIDE,
            fg=TEXT_DIM,
            font=("Segoe UI", 9),
            anchor="w",
        ).pack(fill="x", padx=14, pady=(14, 6))

        self.profile_list = tk.Listbox(
            side,
            bg=BG_SIDE,
            fg=TEXT,
            selectbackground=BG_SEL,
            selectforeground=TEXT,
            activestyle="none",
            highlightthickness=0,
            bd=0,
            font=("Segoe UI", 10),
            exportselection=False,
        )
        self.profile_list.pack(fill="both", expand=True, padx=8)
        self.profile_list.bind("<<ListboxSelect>>", self._on_profile_select)

        tools = tk.Frame(side, bg=BG_SIDE)
        tools.pack(fill="x", padx=10, pady=10)
        for text, cmd in (("＋", self._add_profile), ("✎", self._rename_profile), ("🗑", self._delete_profile)):
            tk.Button(
                tools,
                text=text,
                bg=ACCENT if text == "＋" else BG_INPUT,
                fg="#fff" if text == "＋" else TEXT,
                bd=0,
                width=3,
                cursor="hand2",
                command=cmd,
            ).pack(side="left", padx=3)

        tk.Frame(side, bg=BORDER, height=1).pack(fill="x", padx=12, pady=4)
        tk.Label(
            side,
            text="  Manual",
            bg=BG_SIDE,
            fg=TEXT_DIM,
            font=("Segoe UI", 9),
            anchor="w",
            cursor="hand2",
        ).pack(fill="x", padx=8, pady=2)
        tk.Label(
            side,
            text="  ⚙  Settings",
            bg=BG_SIDE,
            fg=TEXT_DIM,
            font=("Segoe UI", 9),
            anchor="w",
            cursor="hand2",
        ).pack(fill="x", padx=8, pady=(2, 12))
        # click settings → exe path
        side.winfo_children()[-1].bind("<Button-1>", lambda e: self._pick_exe())

        # Main
        main_wrap = tk.Frame(body, bg=BG)
        main_wrap.pack(side="left", fill="both", expand=True)

        canvas = tk.Canvas(main_wrap, bg=BG, highlightthickness=0)
        scroll = ttk.Scrollbar(main_wrap, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        self.main = tk.Frame(canvas, bg=BG)
        self._main_win = canvas.create_window((0, 0), window=self.main, anchor="nw")
        self.main.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        canvas.bind(
            "<Configure>",
            lambda e: canvas.itemconfigure(self._main_win, width=e.width),
        )

        self.title_lbl = tk.Label(
            self.main,
            text='Profile: "Default"',
            bg=BG,
            fg=TEXT,
            font=("Segoe UI", 16, "bold"),
            anchor="w",
        )
        self.title_lbl.pack(fill="x", padx=24, pady=(18, 12))

        grid = tk.Frame(self.main, bg=BG)
        grid.pack(fill="both", expand=True, padx=24, pady=(0, 24))
        grid.columnconfigure(0, weight=1, uniform="c")
        grid.columnconfigure(1, weight=1, uniform="c")

        # Left column
        left = tk.Frame(grid, bg=BG)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        right = tk.Frame(grid, bg=BG)
        right.grid(row=0, column=1, sticky="nsew", padx=(8, 0))

        # Target card
        c = Card(left, "Target")
        c.pack(fill="x", pady=(0, 12))
        self._vars["target_mode"] = tk.StringVar(value="picker")
        self._row_combo(
            c.body,
            "Mode",
            self._vars["target_mode"],
            [
                ("picker", "Picker (countdown)"),
                ("front", "Follow foreground (-front)"),
                ("pid", "Process ID (-pid)"),
                ("pname", "Process name (-pname)"),
                ("window", "Window title (-window)"),
            ],
        )
        self._vars["pid"] = tk.StringVar()
        self._vars["process"] = tk.StringVar()
        self._vars["window"] = tk.StringVar()
        self._row_entry(c.body, "PID", self._vars["pid"])
        self._row_entry(c.body, "Process regex", self._vars["process"])
        self._row_entry(c.body, "Window regex", self._vars["window"])
        self._vars["delay"] = tk.StringVar(value="5")
        self._row_entry(c.body, "Delay (sec)", self._vars["delay"])

        # Motion card
        c = Card(left, "Motion vectors")
        c.pack(fill="x", pady=(0, 12))
        self._vars["motion"] = tk.StringVar(value="amdof")
        self._row_combo(
            c.body,
            "Implementation",
            self._vars["motion"],
            [("amdof", "AMDOF (optical flow)"), ("fast", "Fast MV")],
        )

        # Overlay / HUD
        c = Card(left, "Overlay")
        c.pack(fill="x", pady=(0, 12))
        self._vars["no_overlay"] = tk.BooleanVar(value=False)
        self._row_toggle(c.body, "Disable HUD (-nooverlay)", self._vars["no_overlay"])
        self._vars["overlay_key"] = tk.StringVar(value="ctrl+home")
        self._row_entry(c.body, "Overlay toggle key", self._vars["overlay_key"])
        tk.Label(
            c.body,
            text="Steam-style: open overlay to use OptiScaler / ReShade menus.\nCursor is clipped to the game view while open.",
            bg=BG_CARD,
            fg=TEXT_DIM,
            font=("Segoe UI", 8),
            justify="left",
            wraplength=360,
            anchor="w",
        ).pack(fill="x", pady=(4, 0))

        # Hotkeys
        c = Card(right, "Hotkeys")
        c.pack(fill="x", pady=(0, 12))
        self._vars["stop_key"] = tk.StringVar(value="ctrl+shift+a")
        self._row_entry(c.body, "Quit key", self._vars["stop_key"])
        self._vars["bind_bypass"] = tk.StringVar(
            value="home,insert,end,pageup,pagedown"
        )
        self._row_entry(c.body, "Bind bypass (comma list)", self._vars["bind_bypass"])
        tk.Label(
            c.body,
            text="Keys not sent to the game; used for OptiScaler/ReShade when\noverlay is closed. Disabled automatically while overlay is open.",
            bg=BG_CARD,
            fg=TEXT_DIM,
            font=("Segoe UI", 8),
            justify="left",
            wraplength=360,
            anchor="w",
        ).pack(fill="x", pady=(4, 0))

        # Executable
        c = Card(right, "Executable")
        c.pack(fill="x", pady=(0, 12))
        self._vars["exe_path"] = tk.StringVar(
            value=self.ui_cfg.get("exe_path") or default_nrlive_exe()
        )
        row = tk.Frame(c.body, bg=BG_CARD)
        row.pack(fill="x", pady=3)
        tk.Label(
            row, text="NRLive.exe", bg=BG_CARD, fg=TEXT_DIM, width=16, anchor="w",
            font=("Segoe UI", 9),
        ).pack(side="left")
        tk.Entry(
            row,
            textvariable=self._vars["exe_path"],
            bg=BG_INPUT,
            fg=TEXT,
            insertbackground=TEXT,
            bd=0,
            font=("Segoe UI", 9),
        ).pack(side="left", fill="x", expand=True, ipady=5, padx=(0, 6))
        tk.Button(
            row,
            text="…",
            bg=BG_INPUT,
            fg=TEXT,
            bd=0,
            width=3,
            cursor="hand2",
            command=self._pick_exe,
        ).pack(side="left")

        # Preview command
        c = Card(right, "Command preview")
        c.pack(fill="x", pady=(0, 12))
        self.cmd_preview = tk.Text(
            c.body,
            height=5,
            bg=BG_INPUT,
            fg=SUCCESS,
            insertbackground=TEXT,
            bd=0,
            font=("Consolas", 9),
            wrap="word",
        )
        self.cmd_preview.pack(fill="x")
        self.cmd_preview.configure(state="disabled")

        # Save bar
        bar = tk.Frame(self.main, bg=BG)
        bar.pack(fill="x", padx=24, pady=(0, 16))
        tk.Button(
            bar,
            text="Save profile",
            bg=BG_INPUT,
            fg=TEXT,
            bd=0,
            padx=14,
            pady=6,
            cursor="hand2",
            command=self._save_current,
        ).pack(side="left")
        self.status = tk.Label(bar, text="", bg=BG, fg=TEXT_DIM, font=("Segoe UI", 9))
        self.status.pack(side="left", padx=12)

        # live preview
        for v in self._vars.values():
            if hasattr(v, "trace_add"):
                v.trace_add("write", lambda *_: self._update_preview())

    def _row_combo(self, parent, label, var, choices):
        row = tk.Frame(parent, bg=BG_CARD)
        row.pack(fill="x", pady=3)
        tk.Label(
            row, text=label, bg=BG_CARD, fg=TEXT_DIM, width=16, anchor="w",
            font=("Segoe UI", 9),
        ).pack(side="left")
        values = [c[1] for c in choices]
        self._combo_map = getattr(self, "_combo_map", {})
        self._combo_map[str(var)] = {label: key for key, label in choices}
        self._combo_map[str(var) + "_rev"] = {key: label for key, label in choices}
        cb = ttk.Combobox(row, values=values, state="readonly", width=28)
        cb.pack(side="right")

        def on_sel(_e=None, v=var, box=cb, key=str(var)):
            lab = box.get()
            for k, lb in self._combo_map[key].items():
                if lb == lab or k == lab:
                    # map stored as label->key incorrectly above
                    pass
            # fix: choices is list of (key, label)
            for k, lb in choices:
                if lb == lab:
                    v.set(k)
                    break

        # store choices on widget
        cb._choices = choices  # type: ignore
        cb.bind("<<ComboboxSelected>>", on_sel)
        var._combo = cb  # type: ignore
        var._choices = choices  # type: ignore

        def sync(*_):
            rev = {k: lb for k, lb in choices}
            val = var.get()
            cb.set(rev.get(val, values[0] if values else ""))

        var.trace_add("write", lambda *_: sync())
        sync()

    def _row_entry(self, parent, label, var):
        row = tk.Frame(parent, bg=BG_CARD)
        row.pack(fill="x", pady=3)
        tk.Label(
            row, text=label, bg=BG_CARD, fg=TEXT_DIM, width=16, anchor="w",
            font=("Segoe UI", 9),
        ).pack(side="left")
        tk.Entry(
            row,
            textvariable=var,
            bg=BG_INPUT,
            fg=TEXT,
            insertbackground=TEXT,
            bd=0,
            font=("Segoe UI", 9),
            width=28,
        ).pack(side="right", ipady=5)

    def _row_toggle(self, parent, label, var):
        row = tk.Frame(parent, bg=BG_CARD)
        row.pack(fill="x", pady=4)
        tk.Label(
            row, text=label, bg=BG_CARD, fg=TEXT_DIM, anchor="w",
            font=("Segoe UI", 9),
        ).pack(side="left")
        Toggle(row, var).pack(side="right")

    def _reload_list(self):
        self.profile_list.delete(0, "end")
        for name in list_profiles():
            self.profile_list.insert("end", name)

    def _select_profile(self, name: str):
        names = list_profiles()
        if name not in names:
            name = names[0] if names else "Default"
        self.current_profile = name
        try:
            idx = names.index(name)
            self.profile_list.selection_clear(0, "end")
            self.profile_list.selection_set(idx)
            self.profile_list.see(idx)
        except ValueError:
            pass
        self.title_lbl.configure(text=f'Profile: "{name}"')
        self._load_into_form(load_profile(name))
        self.ui_cfg["last_profile"] = name
        save_ui_config(self.ui_cfg)

    def _on_profile_select(self, _=None):
        if self._building:
            return
        sel = self.profile_list.curselection()
        if not sel:
            return
        name = self.profile_list.get(sel[0])
        if name != self.current_profile:
            self._save_current(silent=True)
            self._select_profile(name)

    def _load_into_form(self, data: dict):
        self._building = True
        try:
            self._vars["target_mode"].set(data.get("target_mode", "picker"))
            self._vars["pid"].set(str(data.get("pid", "")))
            self._vars["process"].set(str(data.get("process", "")))
            self._vars["window"].set(str(data.get("window", "")))
            self._vars["delay"].set(str(data.get("delay", 5)))
            self._vars["no_overlay"].set(bool(data.get("no_overlay", False)))
            self._vars["motion"].set(data.get("motion", "amdof"))
            self._vars["stop_key"].set(data.get("stop_key", "ctrl+shift+a"))
            self._vars["overlay_key"].set(data.get("overlay_key", "ctrl+home"))
            self._vars["bind_bypass"].set(
                data.get("bind_bypass", "home,insert,end,pageup,pagedown")
            )
            exe = (
                data.get("exe_path")
                or self.ui_cfg.get("exe_path")
                or default_nrlive_exe()
            )
            if not exe:
                exe = default_nrlive_exe()
            self._vars["exe_path"].set(exe)
        finally:
            self._building = False
        self._update_preview()

    def _form_data(self) -> dict:
        return {
            "target_mode": self._vars["target_mode"].get(),
            "pid": self._vars["pid"].get().strip(),
            "process": self._vars["process"].get().strip(),
            "window": self._vars["window"].get().strip(),
            "delay": self._vars["delay"].get().strip() or "0",
            "no_overlay": bool(self._vars["no_overlay"].get()),
            "motion": self._vars["motion"].get(),
            "stop_key": self._vars["stop_key"].get().strip(),
            "overlay_key": self._vars["overlay_key"].get().strip(),
            "bind_bypass": self._vars["bind_bypass"].get().strip(),
            "exe_path": self._vars["exe_path"].get().strip(),
        }

    def _save_current(self, silent: bool = False):
        data = self._form_data()
        save_profile(self.current_profile, data)
        self.ui_cfg["exe_path"] = data.get("exe_path", "")
        self.ui_cfg["last_profile"] = self.current_profile
        save_ui_config(self.ui_cfg)
        self._update_preview()
        if not silent:
            self.status.configure(text="Saved.", fg=SUCCESS)
            self.after(2000, lambda: self.status.configure(text=""))

    def _update_preview(self):
        if self._building:
            return
        data = self._form_data()
        exe = data.get("exe_path") or "NRLive.exe"
        try:
            args = build_cli_args(data, exe)
            text = subprocess.list2cmdline(args) if os.name == "nt" else " ".join(
                f'"{a}"' if " " in a else a for a in args
            )
        except Exception as e:
            text = f"(preview error: {e})"
        self.cmd_preview.configure(state="normal")
        self.cmd_preview.delete("1.0", "end")
        self.cmd_preview.insert("1.0", text)
        self.cmd_preview.configure(state="disabled")

    def _add_profile(self):
        name = self._ask_name("New profile", "Profile name:")
        if not name:
            return
        if profile_path(name).is_file():
            messagebox.showerror("Exists", f'Profile "{name}" already exists.')
            return
        self._save_current(silent=True)
        data = self._form_data()
        save_profile(name, data)
        self._reload_list()
        self._select_profile(name)

    def _rename_profile(self):
        if self.current_profile == "Default":
            messagebox.showinfo("Rename", "Default profile cannot be renamed.")
            return
        name = self._ask_name("Rename", "New name:", self.current_profile)
        if not name or name == self.current_profile:
            return
        if profile_path(name).is_file():
            messagebox.showerror("Exists", f'Profile "{name}" already exists.')
            return
        data = load_profile(self.current_profile)
        data.update(self._form_data())
        save_profile(name, data)
        delete_profile(self.current_profile)
        self._reload_list()
        self._select_profile(name)

    def _delete_profile(self):
        if self.current_profile == "Default":
            messagebox.showinfo("Delete", "Default profile cannot be deleted.")
            return
        if not messagebox.askyesno("Delete", f'Delete profile "{self.current_profile}"?'):
            return
        delete_profile(self.current_profile)
        self._reload_list()
        self._select_profile("Default")

    def _ask_name(self, title: str, prompt: str, initial: str = "") -> str | None:
        win = tk.Toplevel(self)
        win.title(title)
        win.configure(bg=BG)
        win.resizable(False, False)
        win.transient(self)
        win.grab_set()
        tk.Label(win, text=prompt, bg=BG, fg=TEXT, font=("Segoe UI", 10)).pack(
            padx=16, pady=(16, 6)
        )
        var = tk.StringVar(value=initial)
        ent = tk.Entry(
            win, textvariable=var, bg=BG_INPUT, fg=TEXT, insertbackground=TEXT, bd=0,
            font=("Segoe UI", 10), width=28,
        )
        ent.pack(padx=16, ipady=6)
        ent.focus_set()
        result: list[str | None] = [None]

        def ok():
            result[0] = var.get().strip()
            win.destroy()

        def cancel():
            result[0] = None
            win.destroy()

        bf = tk.Frame(win, bg=BG)
        bf.pack(pady=12)
        tk.Button(bf, text="OK", bg=ACCENT, fg="#fff", bd=0, padx=16, command=ok).pack(
            side="left", padx=4
        )
        tk.Button(bf, text="Cancel", bg=BG_INPUT, fg=TEXT, bd=0, padx=12, command=cancel).pack(
            side="left", padx=4
        )
        win.bind("<Return>", lambda e: ok())
        win.bind("<Escape>", lambda e: cancel())
        self.wait_window(win)
        return result[0]

    def _pick_exe(self):
        path = filedialog.askopenfilename(
            title="Select NRLive.exe",
            filetypes=[("Executable", "*.exe"), ("All", "*.*")],
        )
        if path:
            self._vars["exe_path"].set(path)
            self.ui_cfg["exe_path"] = path
            save_ui_config(self.ui_cfg)
            self._update_preview()

    def _launch(self):
        self._save_current(silent=True)
        data = self._form_data()
        exe = data.get("exe_path") or self.ui_cfg.get("exe_path", "")
        if not exe or not Path(exe).is_file():
            messagebox.showerror(
                "Executable",
                "Set the path to NRLive.exe (Executable card or Settings).",
            )
            self._pick_exe()
            exe = self._vars["exe_path"].get()
            if not exe or not Path(exe).is_file():
                return
            data["exe_path"] = exe

        mode = data.get("target_mode")
        if mode == "pid" and not data.get("pid"):
            messagebox.showerror("Target", "PID mode requires a process id.")
            return
        if mode == "pname" and not data.get("process"):
            messagebox.showerror("Target", "Process mode requires a name/regex.")
            return
        if mode == "window" and not data.get("window"):
            messagebox.showerror("Target", "Window mode requires a title regex.")
            return

        args = build_cli_args(data, exe)
        try:
            subprocess.Popen(
                args,
                cwd=str(Path(exe).parent),
                shell=False,
            )
            self.status.configure(text="Launched.", fg=SUCCESS)
            self.after(2500, lambda: self.status.configure(text=""))
        except Exception as e:
            messagebox.showerror("Launch failed", str(e))


def main():
    ensure_dirs()
    app = NRLiveUI()
    app.mainloop()


if __name__ == "__main__":
    main()
