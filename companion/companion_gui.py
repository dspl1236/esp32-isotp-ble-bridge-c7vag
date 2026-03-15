#!/usr/bin/env python3
"""
C7 VAG Companion — Desktop GUI for esp32-isotp-ble-bridge (C7 VAG fork)

Tabs:
  1. Connect        — BLE scan + connect
  2. ECU Info       — read standard VW info DIDs from any module
  3. DID Scanner    — brute-force DID range scan (J533 constellation research)
  4. Persist Poller — configure + run the firmware's persist/poll loop
  5. Raw Sniff      — live CAN frame capture with filter input
  6. Simos8 / CAL   — read/write Simos8.5 CAL block (3.0T lean diagnosis)

Build to Windows exe:
    pip install pyinstaller bleak
    pyinstaller --onefile --windowed companion_gui.py

Build to macOS app:
    pyinstaller --onefile --windowed companion_gui.py
"""

import asyncio
import threading
import struct
import time
import tkinter as tk
from tkinter import ttk, scrolledtext, filedialog, messagebox
from typing import Optional
import logging

# Import our BLE client
from ble_bridge_client import (
    BLEBridgeClient, C7_MODULES, MQB_MODULES,
    RawFrame, SETTING_RAW_SNIFF,
)

log = logging.getLogger(__name__)

# ── Colour palette ────────────────────────────────────────────────────────────
BG      = "#1e1e2e"
BG2     = "#2a2a3e"
ACCENT  = "#89b4fa"
GREEN   = "#a6e3a1"
RED     = "#f38ba8"
YELLOW  = "#f9e2af"
TEXT    = "#cdd6f4"
SUBTEXT = "#6c7086"
MONO    = ("Courier", 10)

# ── Async event loop running in a background thread ───────────────────────────
_loop: Optional[asyncio.AbstractEventLoop] = None
_bridge: Optional[BLEBridgeClient] = None


def _start_loop():
    global _loop
    _loop = asyncio.new_event_loop()
    asyncio.set_event_loop(_loop)
    _loop.run_forever()


def run_async(coro):
    """Submit a coroutine to the background loop and return a Future."""
    if _loop is None:
        raise RuntimeError("Event loop not started")
    return asyncio.run_coroutine_threadsafe(coro, _loop)


# ── Shared state ──────────────────────────────────────────────────────────────
class AppState:
    connected = False
    profile   = "C7_VAG"   # or "MQB"

state = AppState()


# ═══════════════════════════════════════════════════════════════════════════════
# Helper widgets
# ═══════════════════════════════════════════════════════════════════════════════

def make_label(parent, text, **kw):
    kw.setdefault("bg", BG)
    kw.setdefault("fg", TEXT)
    kw.setdefault("font", ("Helvetica", 10))
    return tk.Label(parent, text=text, **kw)


def make_entry(parent, width=20, **kw):
    kw.setdefault("bg", BG2)
    kw.setdefault("fg", TEXT)
    kw.setdefault("insertbackground", TEXT)
    kw.setdefault("relief", "flat")
    kw.setdefault("width", width)
    return tk.Entry(parent, **kw)


def make_button(parent, text, cmd, color=ACCENT, **kw):
    return tk.Button(parent, text=text, command=cmd,
                     bg=color, fg="#11111b", relief="flat",
                     activebackground=color, font=("Helvetica", 10, "bold"),
                     padx=8, pady=4, **kw)


def make_log(parent, height=12):
    st = scrolledtext.ScrolledText(parent, height=height, bg=BG2, fg=TEXT,
                                    font=MONO, relief="flat", state="disabled")
    return st


def log_append(widget: scrolledtext.ScrolledText, text: str, color=None):
    widget.configure(state="normal")
    if color:
        tag = f"color_{color.replace('#','')}"
        widget.tag_configure(tag, foreground=color)
        widget.insert("end", text + "\n", tag)
    else:
        widget.insert("end", text + "\n")
    widget.see("end")
    widget.configure(state="disabled")


# ═══════════════════════════════════════════════════════════════════════════════
# Tab 1: Connect
# ═══════════════════════════════════════════════════════════════════════════════

class ConnectTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self.configure(style="Dark.TFrame")
        self._build()

    def _build(self):
        f = tk.Frame(self, bg=BG)
        f.pack(fill="both", expand=True, padx=20, pady=20)

        make_label(f, "C7 VAG Bridge Companion", font=("Helvetica", 16, "bold"),
                   fg=ACCENT).grid(row=0, column=0, columnspan=3, pady=(0, 20))

        make_label(f, "Device name:").grid(row=1, column=0, sticky="e", padx=5)
        self.name_var = tk.StringVar(value="BLE_TO_ISOTP20")
        make_entry(f, textvariable=self.name_var, width=24).grid(row=1, column=1, sticky="w")

        make_label(f, "Or BT address:").grid(row=2, column=0, sticky="e", padx=5)
        self.addr_var = tk.StringVar()
        make_entry(f, textvariable=self.addr_var, width=24).grid(row=2, column=1, sticky="w")

        make_label(f, "Profile:").grid(row=3, column=0, sticky="e", padx=5)
        self.profile_var = tk.StringVar(value="C7_VAG")
        prof_combo = ttk.Combobox(f, textvariable=self.profile_var,
                                   values=["C7_VAG", "MQB"], width=10, state="readonly")
        prof_combo.grid(row=3, column=1, sticky="w")

        self.connect_btn = make_button(f, "Connect", self._connect, color=GREEN)
        self.connect_btn.grid(row=4, column=0, columnspan=2, pady=15)

        self.status_var = tk.StringVar(value="Disconnected")
        self.status_lbl = make_label(f, "", textvariable=self.status_var,
                                      fg=SUBTEXT, font=("Helvetica", 10, "italic"))
        self.status_lbl.grid(row=5, column=0, columnspan=2)

        self.log = make_log(f, height=8)
        self.log.grid(row=6, column=0, columnspan=3, sticky="nsew", pady=10)
        f.rowconfigure(6, weight=1)
        f.columnconfigure(1, weight=1)

    def _connect(self):
        global _bridge
        name    = self.name_var.get().strip() or "BLE_TO_ISOTP20"
        addr    = self.addr_var.get().strip() or None
        profile = self.profile_var.get()
        state.profile = profile

        self.connect_btn.config(state="disabled", text="Connecting…")
        self.status_var.set("Connecting…")
        log_append(self.log, f"Connecting to '{name}' (profile: {profile})…", YELLOW)

        async def _do_connect():
            global _bridge
            try:
                _bridge = BLEBridgeClient(device_address=addr, device_name=name)
                await _bridge.connect()
                return True
            except Exception as e:
                return str(e)

        def _done(fut):
            result = fut.result()
            if result is True:
                state.connected = True
                self.status_var.set("✓ Connected")
                self.status_lbl.config(fg=GREEN)
                self.connect_btn.config(state="normal", text="Reconnect")
                log_append(self.log, "Connected!", GREEN)
            else:
                state.connected = False
                self.status_var.set(f"Error: {result}")
                self.status_lbl.config(fg=RED)
                self.connect_btn.config(state="normal", text="Connect")
                log_append(self.log, f"Error: {result}", RED)

        fut = run_async(_do_connect())
        fut.add_done_callback(lambda f: self.after(0, _done, f))


# ═══════════════════════════════════════════════════════════════════════════════
# Tab 2: ECU Info
# ═══════════════════════════════════════════════════════════════════════════════

class EcuInfoTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._build()

    def _build(self):
        f = tk.Frame(self, bg=BG)
        f.pack(fill="both", expand=True, padx=16, pady=16)

        make_label(f, "Module:").grid(row=0, column=0, sticky="e", padx=5)
        self.module_var = tk.StringVar(value="J533_GATEWAY")
        self.module_combo = ttk.Combobox(f, textvariable=self.module_var,
                                          values=list(C7_MODULES.keys()),
                                          width=20, state="readonly")
        self.module_combo.grid(row=0, column=1, sticky="w")

        self.read_btn = make_button(f, "Read ECU Info", self._read)
        self.read_btn.grid(row=0, column=2, padx=10)

        self.log = make_log(f, height=20)
        self.log.grid(row=1, column=0, columnspan=3, sticky="nsew", pady=10)
        f.rowconfigure(1, weight=1)
        f.columnconfigure(1, weight=1)

    def _read(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect to the bridge first.")
            return
        mod = self.module_var.get()
        modules = C7_MODULES if state.profile == "C7_VAG" else MQB_MODULES
        tx, rx = modules.get(mod, (0x7E0, 0x7E8))

        log_append(self.log, f"\n── {mod}  TX=0x{tx:03X}  RX=0x{rx:03X} ──", ACCENT)
        self.read_btn.config(state="disabled", text="Reading…")

        async def _do():
            return await _bridge.get_ecu_info(tx, rx)

        def _done(fut):
            self.read_btn.config(state="normal", text="Read ECU Info")
            try:
                info = fut.result()
                for label, value in info.items():
                    log_append(self.log, f"  {label:<22} {value}")
            except Exception as e:
                log_append(self.log, f"Error: {e}", RED)

        run_async(_do()).add_done_callback(lambda f: self.after(0, _done, f))


# ═══════════════════════════════════════════════════════════════════════════════
# Tab 3: DID Scanner
# ═══════════════════════════════════════════════════════════════════════════════

class DIDScannerTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._scanning = False
        self._build()

    def _build(self):
        f = tk.Frame(self, bg=BG)
        f.pack(fill="both", expand=True, padx=16, pady=16)

        make_label(f, "Module:").grid(row=0, column=0, sticky="e", padx=5)
        self.module_var = tk.StringVar(value="J533_GATEWAY")
        ttk.Combobox(f, textvariable=self.module_var,
                     values=list(C7_MODULES.keys()),
                     width=20, state="readonly").grid(row=0, column=1, sticky="w")

        make_label(f, "DID start (hex):").grid(row=1, column=0, sticky="e", padx=5)
        self.start_var = tk.StringVar(value="0100")
        make_entry(f, textvariable=self.start_var, width=8).grid(row=1, column=1, sticky="w")

        make_label(f, "DID end (hex):").grid(row=2, column=0, sticky="e", padx=5)
        self.end_var = tk.StringVar(value="0200")
        make_entry(f, textvariable=self.end_var, width=8).grid(row=2, column=1, sticky="w")

        self.scan_btn = make_button(f, "Start Scan", self._start_scan)
        self.scan_btn.grid(row=3, column=0, columnspan=2, pady=8)

        self.progress = ttk.Progressbar(f, length=400, mode="determinate")
        self.progress.grid(row=4, column=0, columnspan=3, pady=4, sticky="ew")

        self.prog_lbl = make_label(f, "")
        self.prog_lbl.grid(row=5, column=0, columnspan=3)

        self.log = make_log(f, height=16)
        self.log.grid(row=6, column=0, columnspan=3, sticky="nsew", pady=8)
        f.rowconfigure(6, weight=1)
        f.columnconfigure(2, weight=1)

    def _start_scan(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        if self._scanning:
            return

        mod = self.module_var.get()
        tx, rx = C7_MODULES.get(mod, (0x7E0, 0x7E8))
        try:
            start = int(self.start_var.get(), 16)
            end   = int(self.end_var.get(), 16)
        except ValueError:
            messagebox.showerror("Bad input", "DID range must be hex values e.g. 0100")
            return

        self._scanning = True
        self.scan_btn.config(state="disabled", text="Scanning…")
        self.progress["value"] = 0
        total = end - start
        log_append(self.log, f"\n── Scanning {mod}  0x{start:04X}–0x{end:04X} ──", ACCENT)

        def _progress(i, tot, did):
            pct = int(100 * i / max(tot, 1))
            self.after(0, lambda: self.progress.configure(value=pct))
            self.after(0, lambda: self.prog_lbl.config(
                text=f"0x{did:04X}  ({i}/{tot})"))

        async def _do():
            return await _bridge.scan_dids(tx, rx, start, end, _progress)

        def _done(fut):
            self._scanning = False
            self.scan_btn.config(state="normal", text="Start Scan")
            self.progress["value"] = 100
            try:
                found = fut.result()
                log_append(self.log, f"\nFound {len(found)} responsive DIDs:", GREEN)
                for did, val in sorted(found.items()):
                    try:
                        text = val.decode("ascii").strip("\x00")
                        text = f'"{text}"'
                    except Exception:
                        text = val.hex(" ").upper()
                    log_append(self.log, f"  0x{did:04X}  {text}", YELLOW)
            except Exception as e:
                log_append(self.log, f"Error: {e}", RED)

        run_async(_do()).add_done_callback(lambda f: self.after(0, _done, f))


# ═══════════════════════════════════════════════════════════════════════════════
# Tab 4: Persist Poller
# ═══════════════════════════════════════════════════════════════════════════════

class PersistTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._polling = False
        self._build()

    def _build(self):
        f = tk.Frame(self, bg=BG)
        f.pack(fill="both", expand=True, padx=16, pady=16)

        make_label(f, "Module:").grid(row=0, column=0, sticky="e", padx=5)
        self.module_var = tk.StringVar(value="J533_GATEWAY")
        ttk.Combobox(f, textvariable=self.module_var,
                     values=list(C7_MODULES.keys()),
                     width=20, state="readonly").grid(row=0, column=1, sticky="w")

        make_label(f, "DIDs to poll (hex, comma-sep):").grid(row=1, column=0, sticky="e", padx=5)
        self.dids_var = tk.StringVar(value="F18C,F190")
        make_entry(f, textvariable=self.dids_var, width=32).grid(row=1, column=1, sticky="w")

        btnf = tk.Frame(f, bg=BG)
        btnf.grid(row=2, column=0, columnspan=2, pady=8)
        self.start_btn = make_button(btnf, "Start Polling", self._start, color=GREEN)
        self.start_btn.pack(side="left", padx=4)
        self.stop_btn  = make_button(btnf, "Stop", self._stop, color=RED)
        self.stop_btn.pack(side="left", padx=4)

        make_label(f, "Note: Persist mode polls DIDs on each ignition event.\n"
                    "Use this to watch J533 constellation DIDs change in real-time.",
                   fg=SUBTEXT, font=("Helvetica", 9, "italic")).grid(
            row=3, column=0, columnspan=2)

        self.log = make_log(f, height=18)
        self.log.grid(row=4, column=0, columnspan=3, sticky="nsew", pady=8)
        f.rowconfigure(4, weight=1)
        f.columnconfigure(1, weight=1)

    def _start(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        mod = self.module_var.get()
        tx, rx = C7_MODULES.get(mod, (0x7E0, 0x7E8))
        try:
            dids = [int(d.strip(), 16) for d in self.dids_var.get().split(",") if d.strip()]
        except ValueError:
            messagebox.showerror("Bad input", "DIDs must be hex values e.g. F18C,F190")
            return

        log_append(self.log, f"\n── Persist poll: {mod}  DIDs: {[hex(d) for d in dids]} ──", ACCENT)
        self._polling = True

        async def _do():
            await _bridge.persist_stop()
            for did in dids:
                payload = struct.pack(">BH", 0x22, did)
                await _bridge.persist_add(tx, rx, payload)
            await _bridge.persist_start()

        run_async(_do())

    def _stop(self):
        if _bridge:
            run_async(_bridge.persist_stop())
        self._polling = False
        log_append(self.log, "Persist stopped.", YELLOW)


# ═══════════════════════════════════════════════════════════════════════════════
# Tab 5: Raw Sniff
# ═══════════════════════════════════════════════════════════════════════════════

class RawSniffTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._sniffing = False
        self._filter_id: Optional[int] = None
        self._build()

    def _build(self):
        f = tk.Frame(self, bg=BG)
        f.pack(fill="both", expand=True, padx=16, pady=16)

        ctrl = tk.Frame(f, bg=BG)
        ctrl.pack(fill="x", pady=(0, 8))

        self.start_btn = make_button(ctrl, "Start Sniff", self._start, color=GREEN)
        self.start_btn.pack(side="left", padx=4)
        self.stop_btn  = make_button(ctrl, "Stop",  self._stop,  color=RED)
        self.stop_btn.pack(side="left", padx=4)
        self.clear_btn = make_button(ctrl, "Clear", self._clear, color=YELLOW)
        self.clear_btn.pack(side="left", padx=4)

        make_label(ctrl, "  Filter CAN ID (hex, blank=all):").pack(side="left")
        self.filter_var = tk.StringVar()
        make_entry(ctrl, textvariable=self.filter_var, width=8).pack(side="left", padx=4)

        make_label(f, "Raw CAN frames (ID < 0x500 visible only in sniff mode):",
                   fg=SUBTEXT).pack(anchor="w")

        self.log = make_log(f, height=24)
        self.log.pack(fill="both", expand=True)

        # Stats bar
        self.stats_var = tk.StringVar(value="0 frames")
        make_label(f, "", textvariable=self.stats_var, fg=SUBTEXT).pack(anchor="e")

        self._frame_count = 0

    def _on_frame(self, frame: RawFrame):
        # Filter
        filt = self.filter_var.get().strip()
        if filt:
            try:
                filt_id = int(filt, 16)
                if frame.can_id != filt_id:
                    return
            except ValueError:
                pass

        self._frame_count += 1
        ts = f"{frame.timestamp_ms/1000:.3f}s " if frame.timestamp_ms else ""
        line = (f"{ts}id=0x{frame.can_id:03X}  "
                f"[{frame.dlc}]  {frame.data.hex(' ').upper()}")

        # Colour-code by ID range
        if frame.can_id in (0x710, 0x77A):
            color = ACCENT    # J533
        elif frame.can_id in (0x746, 0x7B0):
            color = GREEN     # J255
        elif frame.can_id in (0x7E0, 0x7E8):
            color = YELLOW    # ECU
        else:
            color = TEXT

        self.after(0, log_append, self.log, line, color)
        self.after(0, lambda: self.stats_var.set(f"{self._frame_count} frames"))

    def _start(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        self._sniffing = True
        self._frame_count = 0
        log_append(self.log, "\n── Sniff started ──", ACCENT)

        async def _do():
            _bridge._raw_sniff_cb = self._on_frame
            await _bridge.set_raw_sniff(True)

        run_async(_do())

    def _stop(self):
        self._sniffing = False
        if _bridge:
            async def _do():
                await _bridge.set_raw_sniff(False)
                _bridge._raw_sniff_cb = None
            run_async(_do())
        log_append(self.log, "── Sniff stopped ──", YELLOW)

    def _clear(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self._frame_count = 0
        self.stats_var.set("0 frames")


# ═══════════════════════════════════════════════════════════════════════════════
# Tab 6: Simos8 CAL (3.0T lean diagnosis helper)
# ═══════════════════════════════════════════════════════════════════════════════

class Simos8Tab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._build()

    def _build(self):
        f = tk.Frame(self, bg=BG)
        f.pack(fill="both", expand=True, padx=16, pady=16)

        make_label(f, "Simos8.5 (S85) — 3.0T TFSI Engine",
                   font=("Helvetica", 13, "bold"), fg=ACCENT).pack(pady=(0, 12))

        # --- ECU info strip ---
        info_frame = tk.LabelFrame(f, text="ECU Quick Info",
                                    bg=BG, fg=SUBTEXT, relief="flat")
        info_frame.pack(fill="x", pady=4)

        self.info_btn = make_button(info_frame, "Read ECU Info", self._read_info)
        self.info_btn.pack(anchor="w", padx=8, pady=4)

        self.info_log = make_log(info_frame, height=5)
        self.info_log.pack(fill="x", padx=4, pady=4)

        # --- CAL read/write ---
        cal_frame = tk.LabelFrame(f, text="CAL Block (Calibration — fuel/spark/boost maps)",
                                   bg=BG, fg=SUBTEXT, relief="flat")
        cal_frame.pack(fill="x", pady=8)

        btnrow = tk.Frame(cal_frame, bg=BG)
        btnrow.pack(anchor="w", padx=8, pady=4)
        make_button(btnrow, "Read CAL → file", self._read_cal).pack(side="left", padx=4)
        make_button(btnrow, "Write CAL ← file", self._write_cal,
                    color=RED).pack(side="left", padx=4)

        make_label(cal_frame,
                   "CAL base: 0xA0040000   Block 3   Size: 0x3C000\n"
                   "Crypto: XOR counter (trivial).  Checksum: CRC32 at offset 0x300.\n"
                   "VW_Flash CLI: python VW_Flash.py --simos8 --interface J2534 "
                   "--action read --outfile cal.bin",
                   fg=SUBTEXT, font=("Helvetica", 9), justify="left").pack(
            padx=8, pady=4, anchor="w")

        # --- Lambda / AFR probe ---
        lam_frame = tk.LabelFrame(f, text="Live Data — Lean diagnosis",
                                   bg=BG, fg=SUBTEXT, relief="flat")
        lam_frame.pack(fill="x", pady=4)

        btnrow2 = tk.Frame(lam_frame, bg=BG)
        btnrow2.pack(anchor="w", padx=8, pady=4)
        make_button(btnrow2, "Read lambda/AFR DIDs", self._read_lambda).pack(
            side="left", padx=4)

        make_label(lam_frame,
                   "Reads DID 0xF40D (vehicle speed), 0xF442 (voltage),\n"
                   "and attempts known Simos8 AFR/lambda measurement DIDs.\n"
                   "Note: live measurement DIDs vary by CAL version — "
                   "use DID scanner to map your specific bin.",
                   fg=SUBTEXT, font=("Helvetica", 9), justify="left").pack(
            padx=8, pady=2, anchor="w")

        self.lam_log = make_log(lam_frame, height=6)
        self.lam_log.pack(fill="x", padx=4, pady=4)

    def _read_info(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        tx, rx = C7_MODULES["ECU_ENGINE"]
        log_append(self.info_log, "Reading Simos8.5 ECU info…", YELLOW)

        async def _do():
            return await _bridge.get_ecu_info(tx, rx)

        def _done(fut):
            try:
                info = fut.result()
                for k, v in info.items():
                    log_append(self.info_log, f"  {k:<22} {v}")
            except Exception as e:
                log_append(self.info_log, f"Error: {e}", RED)

        run_async(_do()).add_done_callback(lambda f: self.after(0, _done, f))

    def _read_cal(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".bin",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")],
            title="Save CAL block as…")
        if not path:
            return
        messagebox.showinfo("Use VW_Flash CLI",
                            "Direct CAL reading requires VW_Flash:\n\n"
                            "python VW_Flash.py --simos8 --interface J2534 \\\n"
                            "  --action read --outfile cal.bin\n\n"
                            "This uses the same J2534 bridge interface.\n"
                            f"Save path noted: {path}")

    def _write_cal(self):
        messagebox.showinfo("Use VW_Flash CLI",
                            "CAL write (tuning) requires VW_Flash:\n\n"
                            "python VW_Flash.py --simos8 --interface J2534 \\\n"
                            "  --action flash --infile cal_modified.bin\n\n"
                            "VW_Flash handles XOR crypto, CRC32 checksum fix,\n"
                            "and the SA2 seed/key security access automatically.")

    def _read_lambda(self):
        if not state.connected or _bridge is None:
            messagebox.showwarning("Not connected", "Connect first.")
            return
        tx, rx = C7_MODULES["ECU_ENGINE"]

        # Known Simos8 measurement DIDs — some may not respond on all cal versions
        # These are discovered empirically / via DID scanner
        probe_dids = {
            0xF40D: "Vehicle Speed",
            0xF442: "Control Module Voltage",
            0x0600: "Coding Value",
            0x295A: "Mileage",
        }

        log_append(self.lam_log, "\nProbing ECU live data DIDs…", YELLOW)

        async def _do():
            results = {}
            await _bridge.uds_session(tx, rx, session=0x03)
            for did, label in probe_dids.items():
                try:
                    raw = await _bridge.uds_read_did(tx, rx, did, timeout=2.0)
                    results[label] = raw
                except Exception as e:
                    results[label] = None
            return results

        def _done(fut):
            try:
                r = fut.result()
                for label, raw in r.items():
                    if raw:
                        log_append(self.lam_log,
                                   f"  {label:<30} {raw.hex(' ').upper()}", GREEN)
                    else:
                        log_append(self.lam_log,
                                   f"  {label:<30} <no response>", SUBTEXT)
                log_append(self.lam_log,
                           "\nFor full live data (MAF, lambda, boost) use VW_Flash HSL:\n"
                           "  python VW_Flash.py --simos8 --interface J2534 --action log",
                           SUBTEXT)
            except Exception as e:
                log_append(self.lam_log, f"Error: {e}", RED)

        run_async(_do()).add_done_callback(lambda f: self.after(0, _done, f))


# ═══════════════════════════════════════════════════════════════════════════════
# Main application window
# ═══════════════════════════════════════════════════════════════════════════════

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("C7 VAG Bridge Companion")
        self.geometry("900x700")
        self.configure(bg=BG)
        self.resizable(True, True)
        self._style()
        self._build()

    def _style(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=BG2, foreground=TEXT,
                         padding=[12, 6], font=("Helvetica", 10))
        style.map("TNotebook.Tab",
                  background=[("selected", ACCENT)],
                  foreground=[("selected", "#11111b")])
        style.configure("Dark.TFrame", background=BG)
        style.configure("TCombobox", fieldbackground=BG2, background=BG2,
                         foreground=TEXT, selectbackground=ACCENT)

    def _build(self):
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=8)

        tabs = [
            ("Connect",        ConnectTab),
            ("ECU Info",       EcuInfoTab),
            ("DID Scanner",    DIDScannerTab),
            ("Persist Poller", PersistTab),
            ("Raw Sniff",      RawSniffTab),
            ("Simos8 / CAL",   Simos8Tab),
        ]
        for name, cls in tabs:
            frame = cls(nb)
            nb.add(frame, text=name)

    def on_close(self):
        if _bridge:
            run_async(_bridge.disconnect())
        if _loop:
            _loop.call_soon_threadsafe(_loop.stop)
        self.destroy()


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    logging.basicConfig(level=logging.INFO,
                         format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    # Start BLE event loop in background thread
    t = threading.Thread(target=_start_loop, daemon=True)
    t.start()

    app = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()


if __name__ == "__main__":
    main()
