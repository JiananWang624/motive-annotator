#!/usr/bin/env python3
"""
Motive Annotation Monitor v3.1

Thin annotation layer for OptiTrack Motive playback:
- Motive remains the playback / frame-stepping UI.
- NatNet supplies the current Motive frame.
- Global hotkeys annotate only stable frames.
- Compact always-on-top monitor shows current and saved Frame IDs.
- One CSV per Take; existing labels reload automatically.
- Explicit Correction Mode allows deliberate replacement without silent overwrite.

Use Motive Frame ID as the primary key when joining to Motive marker exports.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import queue
import re
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import tkinter as tk
from pynput import keyboard

try:
    from NatNetClient import NatNetClient
except ImportError as exc:
    raise SystemExit(
        "\nCannot import NatNetClient.\n\n"
        "Put motive_annotator.py in the official OptiTrack NatNet Python sample "
        "folder containing NatNetClient.py, then run it again.\n"
    ) from exc


CSV_FIELDS = [
    "take",
    "bite_id",
    "event",
    "motive_frame",
    "natnet_timestamp_s",
    "annotated_at",
]


class FrameState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.frame: Optional[int] = None
        self.timestamp: Optional[float] = None
        self.last_frame_change = 0.0
        self.last_packet = 0.0

    def update(self, frame, timestamp) -> None:
        if frame is None:
            return
        try:
            frame = int(frame)
        except (TypeError, ValueError):
            return

        now = time.monotonic()
        with self.lock:
            if self.frame != frame:
                self.frame = frame
                self.last_frame_change = now
            self.last_packet = now
            try:
                self.timestamp = float(timestamp) if timestamp is not None else None
            except (TypeError, ValueError):
                self.timestamp = None

    def snapshot(self) -> Tuple[Optional[int], Optional[float], float, float]:
        with self.lock:
            return self.frame, self.timestamp, self.last_frame_change, self.last_packet


FRAME_STATE = FrameState()


def receive_new_frame(*args, **kwargs):
    """Support common old/new OptiTrack NatNet Python callback forms."""
    frame = None
    timestamp = None

    if len(args) == 1 and isinstance(args[0], dict):
        data = args[0]
        frame = data.get("frame_number", data.get("frameNumber"))
        timestamp = data.get("timestamp")
    elif len(args) >= 1:
        frame = args[0]
        if len(args) >= 9:
            timestamp = args[8]

    if frame is None:
        frame = kwargs.get("frame_number", kwargs.get("frameNumber"))
    if timestamp is None:
        timestamp = kwargs.get("timestamp")

    FRAME_STATE.update(frame, timestamp)


def safe_take_name(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", name.strip())
    return cleaned or "unnamed_take"


def load_config(path: Path) -> dict:
    if not path.exists():
        raise ValueError(f"Config file not found: {path}")

    with path.open("r", encoding="utf-8") as f:
        cfg = json.load(f)

    events = cfg.get("events")
    if not isinstance(events, list) or not events:
        raise ValueError("Config must contain a non-empty 'events' list.")

    seen_names = set()
    seen_hotkeys = set()
    for event in events:
        for key in ("name", "label", "hotkey"):
            if not event.get(key):
                raise ValueError(f"Each event must define '{key}'.")
        if event["name"] in seen_names:
            raise ValueError(f"Duplicate event name: {event['name']}")
        if event["hotkey"] in seen_hotkeys:
            raise ValueError(f"Duplicate hotkey: {event['hotkey']}")
        seen_names.add(event["name"])
        seen_hotkeys.add(event["hotkey"])

    actions = cfg.get("actions", {})
    required_actions = (
        "correction_mode",
        "undo",
        "next_bite",
        "previous_bite",
        "quit",
    )
    for action in required_actions:
        hotkey = actions.get(action)
        if not hotkey:
            raise ValueError(f"Missing action hotkey: {action}")
        if hotkey in seen_hotkeys:
            raise ValueError(f"Hotkey collision: {hotkey}")
        seen_hotkeys.add(hotkey)

    cfg["stable_ms"] = int(cfg.get("stable_ms", 300))
    cfg["correction_timeout_s"] = float(cfg.get("correction_timeout_s", 10))
    if cfg["stable_ms"] < 0:
        raise ValueError("stable_ms must be >= 0.")
    if cfg["correction_timeout_s"] <= 0:
        raise ValueError("correction_timeout_s must be > 0.")

    return cfg


def set_client_config(client, client_ip: str, server_ip: str, multicast: bool):
    if hasattr(client, "set_client_address"):
        client.set_client_address(client_ip)
    elif hasattr(client, "local_ip_address"):
        client.local_ip_address = client_ip
    elif hasattr(client, "localIPAddress"):
        client.localIPAddress = client_ip

    if hasattr(client, "set_server_address"):
        client.set_server_address(server_ip)
    elif hasattr(client, "server_ip_address"):
        client.server_ip_address = server_ip
    elif hasattr(client, "serverIPAddress"):
        client.serverIPAddress = server_ip

    if hasattr(client, "set_use_multicast"):
        client.set_use_multicast(multicast)
    elif hasattr(client, "use_multicast"):
        client.use_multicast = multicast


def attach_frame_callback(client) -> None:
    if hasattr(client, "new_frame_listener"):
        client.new_frame_listener = receive_new_frame
    elif hasattr(client, "newFrameListener"):
        client.newFrameListener = receive_new_frame
    else:
        raise RuntimeError(
            "NatNetClient has no new_frame_listener/newFrameListener callback."
        )


class AnnotationStore:
    """Final-state, per-Take CSV with atomic writes and session undo."""

    def __init__(self, csv_path: Path, take: str) -> None:
        self.csv_path = csv_path
        self.take = take
        self.rows: Dict[Tuple[int, str], dict] = {}
        self.session_undo: List[Tuple[str, Tuple[int, str], Optional[dict]]] = []
        self.load()

    def load(self) -> None:
        if not self.csv_path.exists():
            return

        with self.csv_path.open("r", newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            fields = reader.fieldnames or []
            missing = [field for field in CSV_FIELDS if field not in fields]
            if missing:
                raise ValueError(
                    f"{self.csv_path.name} is missing columns: {', '.join(missing)}"
                )

            for row in reader:
                if row["take"] != self.take:
                    raise ValueError(
                        f"{self.csv_path.name} contains Take '{row['take']}', "
                        f"but current Take is '{self.take}'."
                    )
                self.rows[(int(row["bite_id"]), row["event"])] = row

    def _atomic_write(self) -> None:
        self.csv_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.csv_path.with_suffix(self.csv_path.suffix + ".tmp")

        ordered = sorted(
            self.rows.values(),
            key=lambda r: (int(r["bite_id"]), int(r["motive_frame"]), r["event"]),
        )

        try:
            with tmp.open("w", newline="", encoding="utf-8") as f:
                writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
                writer.writeheader()
                writer.writerows(ordered)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self.csv_path)
        except PermissionError as exc:
            try:
                tmp.unlink(missing_ok=True)
            except Exception:
                pass
            raise PermissionError(
                f"Cannot update {self.csv_path.name}. "
                "Close it in Excel/another program and try again."
            ) from exc

    def get(self, bite_id: int, event: str) -> Optional[dict]:
        return self.rows.get((bite_id, event))

    @staticmethod
    def _make_row(take, bite_id, event, frame, natnet_timestamp) -> dict:
        return {
            "take": take,
            "bite_id": str(bite_id),
            "event": event,
            "motive_frame": str(frame),
            "natnet_timestamp_s": (
                "" if natnet_timestamp is None else f"{natnet_timestamp:.9f}"
            ),
            "annotated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        }

    def add(
        self,
        bite_id: int,
        event: str,
        frame: int,
        natnet_timestamp: Optional[float],
    ) -> dict:
        key = (bite_id, event)
        if key in self.rows:
            raise KeyError("duplicate")

        row = self._make_row(
            self.take, bite_id, event, frame, natnet_timestamp
        )
        self.rows[key] = row
        try:
            self._atomic_write()
        except Exception:
            self.rows.pop(key, None)
            raise

        self.session_undo.append(("add", key, None))
        return row

    def replace(
        self,
        bite_id: int,
        event: str,
        frame: int,
        natnet_timestamp: Optional[float],
    ) -> Tuple[dict, dict]:
        key = (bite_id, event)
        previous = self.rows.get(key)
        if previous is None:
            raise KeyError("missing")

        previous_copy = dict(previous)
        new_row = self._make_row(
            self.take, bite_id, event, frame, natnet_timestamp
        )
        self.rows[key] = new_row

        try:
            self._atomic_write()
        except Exception:
            self.rows[key] = previous_copy
            raise

        self.session_undo.append(("replace", key, previous_copy))
        return previous_copy, new_row

    def undo(self) -> Optional[Tuple[str, dict]]:
        if not self.session_undo:
            return None

        action, key, previous = self.session_undo.pop()

        if action == "add":
            removed = self.rows.pop(key, None)
            try:
                self._atomic_write()
            except Exception:
                if removed is not None:
                    self.rows[key] = removed
                self.session_undo.append((action, key, previous))
                raise
            return "add", removed

        if action == "replace":
            current = self.rows.get(key)
            self.rows[key] = previous
            try:
                self._atomic_write()
            except Exception:
                if current is not None:
                    self.rows[key] = current
                self.session_undo.append((action, key, previous))
                raise
            return "replace", previous

        return None

    def rows_for_bite(self, bite_id: int) -> Dict[str, dict]:
        return {
            event: row
            for (bite, event), row in self.rows.items()
            if bite == bite_id
        }

    def recent_rows(self, limit: int = 5) -> List[dict]:
        rows = list(self.rows.values())
        rows.sort(key=lambda r: r["annotated_at"])
        return rows[-limit:]


class App:
    def __init__(
        self,
        root: tk.Tk,
        take: str,
        store: AnnotationStore,
        config: dict,
        command_queue: queue.Queue,
    ) -> None:
        self.root = root
        self.take = take
        self.store = store
        self.config = config
        self.command_queue = command_queue

        self.event_defs = config["events"]
        self.event_names = [e["name"] for e in self.event_defs]
        self.event_labels = {e["name"]: e["label"] for e in self.event_defs}
        self.stable_seconds = config["stable_ms"] / 1000.0
        self.correction_timeout = config["correction_timeout_s"]

        self.bite_id = 1
        if self.store.rows:
            self.bite_id = max(bite for bite, _ in self.store.rows.keys())

        self.correction_until = 0.0
        self.last_message = "Ready. Pause/step Motive to an exact frame."
        self.message_kind = "normal"

        self._build_ui()
        self.refresh()

    def _build_ui(self) -> None:
        self.root.title("Motive Annotator")
        self.root.attributes("-topmost", True)
        self.root.resizable(False, False)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)

        outer = tk.Frame(self.root, padx=12, pady=10)
        outer.pack(fill="both", expand=True)

        header = tk.Frame(outer)
        header.pack(fill="x")

        self.take_var = tk.StringVar(value=self.take)
        self.bite_var = tk.StringVar(value=f"Bite {self.bite_id}")
        tk.Label(
            header, textvariable=self.take_var, font=("Segoe UI", 11, "bold")
        ).pack(side="left")
        tk.Label(
            header, textvariable=self.bite_var, font=("Segoe UI", 11, "bold")
        ).pack(side="right")

        status = tk.Frame(outer, pady=8)
        status.pack(fill="x")

        self.frame_var = tk.StringVar(value="—")
        self.state_var = tk.StringVar(value="NO DATA")
        self.mode_var = tk.StringVar(value="NORMAL")

        tk.Label(status, text="Frame", font=("Segoe UI", 9)).grid(
            row=0, column=0, sticky="w"
        )
        tk.Label(
            status, textvariable=self.frame_var, font=("Consolas", 20, "bold")
        ).grid(row=1, column=0, sticky="w")

        right = tk.Frame(status)
        right.grid(row=0, column=1, rowspan=2, padx=(30, 0), sticky="e")
        self.state_label = tk.Label(
            right, textvariable=self.state_var, font=("Segoe UI", 10, "bold")
        )
        self.state_label.pack(anchor="e")
        self.mode_label = tk.Label(
            right, textvariable=self.mode_var, font=("Segoe UI", 9, "bold")
        )
        self.mode_label.pack(anchor="e")
        status.columnconfigure(1, weight=1)

        tk.Frame(outer, height=1, bd=1, relief="sunken").pack(
            fill="x", pady=(0, 8)
        )

        self.event_frame_vars: Dict[str, tk.StringVar] = {}
        for event in self.event_defs:
            row = tk.Frame(outer)
            row.pack(fill="x", pady=1)

            tk.Label(
                row,
                text=event["label"],
                width=14,
                anchor="w",
                font=("Segoe UI", 9),
            ).pack(side="left")

            var = tk.StringVar(value="-----")
            self.event_frame_vars[event["name"]] = var
            tk.Label(
                row,
                textvariable=var,
                width=10,
                anchor="e",
                font=("Consolas", 10),
            ).pack(side="left")

            tk.Label(
                row,
                text=self._pretty_hotkey(event["hotkey"]),
                anchor="e",
                font=("Segoe UI", 8),
            ).pack(side="right")

        tk.Frame(outer, height=1, bd=1, relief="sunken").pack(fill="x", pady=8)

        self.sequence_var = tk.StringVar(value="Sequence: —")
        self.sequence_label = tk.Label(
            outer,
            textvariable=self.sequence_var,
            anchor="w",
            font=("Segoe UI", 9),
        )
        self.sequence_label.pack(fill="x")

        self.message_var = tk.StringVar(value=self.last_message)
        self.message_label = tk.Label(
            outer,
            textvariable=self.message_var,
            anchor="w",
            justify="left",
            wraplength=410,
            font=("Segoe UI", 9),
        )
        self.message_label.pack(fill="x", pady=(5, 7))

        tk.Label(
            outer,
            text="Recent",
            anchor="w",
            font=("Segoe UI", 8, "bold"),
        ).pack(fill="x")

        self.recent_var = tk.StringVar(value="—")
        tk.Label(
            outer,
            textvariable=self.recent_var,
            anchor="w",
            justify="left",
            font=("Consolas", 8),
        ).pack(fill="x")

        actions = self.config["actions"]
        footer_text = (
            f"Edit {self._pretty_hotkey(actions['correction_mode'])}   "
            f"Undo {self._pretty_hotkey(actions['undo'])}\n"
            f"Next {self._pretty_hotkey(actions['next_bite'])}   "
            f"Prev {self._pretty_hotkey(actions['previous_bite'])}"
        )
        tk.Label(
            outer,
            text=footer_text,
            justify="left",
            anchor="w",
            font=("Segoe UI", 8),
            pady=8,
        ).pack(fill="x")

    @staticmethod
    def _pretty_hotkey(hotkey: str) -> str:
        return (
            hotkey.replace("<ctrl>", "Ctrl")
            .replace("<alt>", "Alt")
            .replace("<shift>", "Shift")
            .upper()
        )

    def correction_active(self) -> bool:
        return time.monotonic() < self.correction_until

    def current_frame_status(self):
        frame, timestamp, last_change, last_packet = FRAME_STATE.snapshot()
        if frame is None:
            return frame, timestamp, "NO DATA", False

        stable_age = time.monotonic() - last_change if last_change else 0.0
        stable = stable_age >= self.stable_seconds
        status = f"STABLE {stable_age:.1f}s" if stable else "MOVING"
        return frame, timestamp, status, stable

    def _sequence_check(self) -> Tuple[str, bool]:
        bite_rows = self.store.rows_for_bite(self.bite_id)
        seen = []
        for event_name in self.event_names:
            row = bite_rows.get(event_name)
            if row:
                seen.append((event_name, int(row["motive_frame"])))

        if len(seen) < 2:
            return "Sequence: —", True

        for (name_a, frame_a), (name_b, frame_b) in zip(seen, seen[1:]):
            if frame_a >= frame_b:
                return (
                    f"WARNING: {self.event_labels[name_a]} {frame_a} "
                    f">= {self.event_labels[name_b]} {frame_b}",
                    False,
                )
        return "Sequence: OK", True

    def set_message(self, text: str, kind: str = "normal") -> None:
        self.last_message = text
        self.message_kind = kind

    def enable_correction_mode(self) -> None:
        self.correction_until = time.monotonic() + self.correction_timeout
        self.set_message(
            f"CORRECTION MODE: press the event hotkey to replace its frame "
            f"within {self.correction_timeout:g}s.",
            "warning",
        )

    def handle_event(self, event_name: str) -> None:
        frame, timestamp, _, stable = self.current_frame_status()

        if frame is None:
            self.set_message("NOT SAVED: no NatNet frame received.", "error")
            return

        if not stable:
            self.set_message(
                f"NOT SAVED: frame is still moving ({frame}). Pause/step Motive first.",
                "error",
            )
            return

        existing = self.store.get(self.bite_id, event_name)

        if self.correction_active():
            if existing is None:
                self.set_message(
                    f"CORRECTION NOT APPLIED: "
                    f"{self.event_labels[event_name]} has no existing frame in Bite "
                    f"{self.bite_id}.",
                    "error",
                )
                self.correction_until = 0.0
                return

            old_frame = existing["motive_frame"]
            try:
                _, new_row = self.store.replace(
                    self.bite_id, event_name, frame, timestamp
                )
            except PermissionError as exc:
                self.set_message(str(exc), "error")
                return
            except Exception as exc:
                self.set_message(f"Correction failed: {exc}", "error")
                return

            self.correction_until = 0.0
            self.set_message(
                f"CORRECTED: B{self.bite_id} {self.event_labels[event_name]} "
                f"{old_frame} -> {frame}",
                "success",
            )
            print(
                f"[CORRECTED] take={self.take} bite={self.bite_id} "
                f"event={event_name} {old_frame}->{frame}"
            )
            return

        if existing is not None:
            self.set_message(
                f"NOT SAVED: {self.event_labels[event_name]} already exists at "
                f"frame {existing['motive_frame']}. Enter Correction Mode to replace it.",
                "error",
            )
            return

        try:
            self.store.add(self.bite_id, event_name, frame, timestamp)
        except PermissionError as exc:
            self.set_message(str(exc), "error")
            return
        except Exception as exc:
            self.set_message(f"Save failed: {exc}", "error")
            return

        self.set_message(
            f"SAVED: B{self.bite_id} {self.event_labels[event_name]} @ frame {frame}",
            "success",
        )
        print(
            f"[SAVED] take={self.take} bite={self.bite_id} "
            f"event={event_name} frame={frame}"
        )

    def undo(self) -> None:
        try:
            result = self.store.undo()
        except PermissionError as exc:
            self.set_message(str(exc), "error")
            return
        except Exception as exc:
            self.set_message(f"Undo failed: {exc}", "error")
            return

        if not result:
            self.set_message("Nothing to undo from this program run.", "normal")
            return

        action, row = result
        self.bite_id = int(row["bite_id"])

        if action == "add":
            self.set_message(
                f"UNDONE: removed B{row['bite_id']} "
                f"{self.event_labels.get(row['event'], row['event'])} "
                f"@ frame {row['motive_frame']}",
                "normal",
            )
        else:
            self.set_message(
                f"UNDONE: restored B{row['bite_id']} "
                f"{self.event_labels.get(row['event'], row['event'])} "
                f"to frame {row['motive_frame']}",
                "normal",
            )

    def next_bite(self) -> None:
        self.correction_until = 0.0
        bite_rows = self.store.rows_for_bite(self.bite_id)
        next_id = self.bite_id + 1

        if bite_rows and len(bite_rows) < len(self.event_names):
            missing = [
                self.event_labels[name]
                for name in self.event_names
                if name not in bite_rows
            ]
            self.set_message(
                f"Bite {self.bite_id} incomplete ({', '.join(missing)} missing). "
                f"Moved to Bite {next_id}.",
                "warning",
            )
        else:
            self.set_message(f"Moved to Bite {next_id}.", "normal")

        self.bite_id = next_id

    def previous_bite(self) -> None:
        self.correction_until = 0.0
        if self.bite_id <= 1:
            self.set_message("Already at Bite 1.", "normal")
            return

        self.bite_id -= 1
        self.set_message(f"Moved to Bite {self.bite_id}.", "normal")

    def process_commands(self) -> None:
        try:
            while True:
                command, payload = self.command_queue.get_nowait()
                if command == "event":
                    self.handle_event(payload)
                elif command == "correction_mode":
                    self.enable_correction_mode()
                elif command == "undo":
                    self.undo()
                elif command == "next_bite":
                    self.next_bite()
                elif command == "previous_bite":
                    self.previous_bite()
                elif command == "quit":
                    self.quit()
        except queue.Empty:
            pass

    def refresh(self) -> None:
        self.process_commands()

        frame, _, status, _ = self.current_frame_status()
        self.frame_var.set("—" if frame is None else str(frame))
        self.state_var.set(status)
        self.bite_var.set(f"Bite {self.bite_id}")

        active_correction = self.correction_active()
        self.mode_var.set("CORRECTION" if active_correction else "NORMAL")

        bite_rows = self.store.rows_for_bite(self.bite_id)
        for event in self.event_defs:
            row = bite_rows.get(event["name"])
            self.event_frame_vars[event["name"]].set(
                "-----" if row is None else f"{row['motive_frame']}  ✓"
            )

        seq_text, seq_good = self._sequence_check()
        self.sequence_var.set(seq_text)
        self.message_var.set(self.last_message)

        recent = self.store.recent_rows(5)
        self.recent_var.set(
            "—"
            if not recent
            else "\n".join(
                f"B{r['bite_id']} "
                f"{self.event_labels.get(r['event'], r['event'])[:11]:11s} "
                f"{r['motive_frame']}"
                for r in recent
            )
        )

        if status.startswith("STABLE"):
            self.state_label.config(fg="dark green")
        elif status == "MOVING":
            self.state_label.config(fg="dark orange")
        else:
            self.state_label.config(fg="dark red")

        self.mode_label.config(fg="dark red" if active_correction else "black")
        self.sequence_label.config(fg="black" if seq_good else "dark red")

        if self.message_kind == "success":
            self.message_label.config(fg="dark green")
        elif self.message_kind in ("error", "warning"):
            self.message_label.config(fg="dark red")
        else:
            self.message_label.config(fg="black")

        self.root.after(80, self.refresh)

    def quit(self) -> None:
        self.root.quit()


def build_hotkey_map(config: dict, command_queue: queue.Queue):
    mapping = {}

    for event in config["events"]:
        name = event["name"]

        def make_event_callback(event_name):
            return lambda: command_queue.put(("event", event_name))

        mapping[event["hotkey"]] = make_event_callback(name)

    actions = config["actions"]
    mapping[actions["correction_mode"]] = (
        lambda: command_queue.put(("correction_mode", None))
    )
    mapping[actions["undo"]] = lambda: command_queue.put(("undo", None))
    mapping[actions["next_bite"]] = lambda: command_queue.put(("next_bite", None))
    mapping[actions["previous_bite"]] = (
        lambda: command_queue.put(("previous_bite", None))
    )
    mapping[actions["quit"]] = lambda: command_queue.put(("quit", None))

    return mapping


def start_natnet(client_ip: str, server_ip: str, multicast: bool):
    client = NatNetClient()
    set_client_config(client, client_ip, server_ip, multicast)
    attach_frame_callback(client)

    result = client.run()
    if result is False:
        raise RuntimeError(
            "NatNet client failed to start. Check Motive Streaming settings, "
            "IP addresses, Windows Firewall, and multicast/unicast mode."
        )
    return client


def shutdown_natnet(client) -> None:
    for method_name in ("shutdown", "close"):
        method = getattr(client, method_name, None)
        if callable(method):
            try:
                method()
            except Exception:
                pass
            break


def main():
    parser = argparse.ArgumentParser(
        description="Always-on-top frame annotation monitor for Motive playback."
    )
    parser.add_argument("--take", required=True, help="Exact Take/trial name.")
    parser.add_argument(
        "--output-dir",
        default="annotations",
        help="Directory for per-Take CSV files (default: annotations).",
    )
    parser.add_argument(
        "--config",
        default="annotation_config.json",
        help="JSON event/hotkey configuration.",
    )
    parser.add_argument(
        "--client-ip",
        default="127.0.0.1",
        help="NatNet client/local IP for same-PC use.",
    )
    parser.add_argument(
        "--server-ip",
        default="127.0.0.1",
        help="Motive server IP for same-PC use.",
    )
    parser.add_argument(
        "--unicast",
        action="store_true",
        help="Use unicast instead of multicast.",
    )
    args = parser.parse_args()

    take = args.take.strip()
    if not take:
        raise SystemExit("--take cannot be empty.")

    try:
        config = load_config(Path(args.config).resolve())
    except Exception as exc:
        raise SystemExit(f"Config error: {exc}") from exc

    csv_path = (
        Path(args.output_dir).resolve()
        / f"{safe_take_name(take)}.annotations.csv"
    )

    try:
        store = AnnotationStore(csv_path, take)
    except Exception as exc:
        raise SystemExit(f"Annotation file error: {exc}") from exc

    try:
        client = start_natnet(
            args.client_ip,
            args.server_ip,
            multicast=not args.unicast,
        )
    except Exception as exc:
        raise SystemExit(str(exc)) from exc

    command_queue = queue.Queue()
    hotkey_listener = keyboard.GlobalHotKeys(
        build_hotkey_map(config, command_queue)
    )
    hotkey_listener.start()

    print("=" * 68)
    print("MOTIVE ANNOTATION MONITOR v3.1")
    print(f"Take:   {take}")
    print(f"CSV:    {csv_path}")
    print(f"Stable: {config['stable_ms']} ms")
    print("=" * 68)

    root = tk.Tk()
    App(root, take, store, config, command_queue)

    try:
        root.mainloop()
    finally:
        try:
            hotkey_listener.stop()
        except Exception:
            pass
        shutdown_natnet(client)
        try:
            root.destroy()
        except Exception:
            pass


if __name__ == "__main__":
    main()
