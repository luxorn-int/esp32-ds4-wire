from __future__ import annotations

import json
import queue
import struct
import tkinter as tk
from datetime import datetime, timezone
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, ttk

from bridge_protocol import GAMEPAD_SLOTS, Frame, FrameType, LinkState, decode_status
from report_profile import OutputReport, ReportProfile
from serial_bridge import SerialBridge
from virtual_ds4 import VirtualDS4

APP_TITLE = "ESP32 Gamepad Bridge"
BAUD_RATE = 921600
BG = "#101722"
PANEL = "#182331"
PANEL_ALT = "#1E2B3B"
TEXT = "#E7EDF5"
MUTED = "#93A3B7"
ACCENT = "#58C7B7"
ACCENT_DARK = "#286F6A"
ERROR = "#FF8D88"

STATE_NAMES = {
    LinkState.UNKNOWN: "Неизвестно",
    LinkState.SCANNING: "Поиск геймпада",
    LinkState.CONNECTING: "Подключение",
    LinkState.CONNECTED: "Геймпад подключён",
    LinkState.PAIRING: "Режим сопряжения",
    LinkState.DISCONNECTED: "Геймпад не подключён",
}


class GamepadBridgeApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("1050x760")
        self.root.minsize(900, 650)
        self.root.configure(bg=BG)
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.bridge = SerialBridge(
            on_frame=lambda frame: self.events.put(("frame", frame)),
            on_error=lambda error: self.events.put(("error", error)),
        )
        self.virtual_ds4 = VirtualDS4()
        self.profiles: dict[int, ReportProfile] = {}
        self.link_state = LinkState.UNKNOWN
        self.pairing_enabled = False
        self.bond_count = 0
        self.controllers: dict[int, dict[str, object]] = {}
        self.last_report_id: int | None = None
        self.last_map_index = 0
        self.raw_report_count = 0
        self.capture_file: Path | None = None
        self.selected_slot = 0
        self.colors = {0: (0, 255, 0), 1: (0, 96, 255)}
        self.rgb = self.colors[self.selected_slot]

        self._configure_style()
        self._build_ui()
        self._refresh_ports()
        self._load_default_profile()
        self._update_ds4_status()
        self.root.after(50, self._drain_events)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _configure_style(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure(".", background=BG, foreground=TEXT, font=("Segoe UI", 10))
        style.configure("TFrame", background=BG)
        style.configure("Panel.TFrame", background=PANEL)
        style.configure("Inset.TFrame", background=PANEL_ALT)
        style.configure("TLabel", background=BG, foreground=TEXT)
        style.configure("Panel.TLabel", background=PANEL, foreground=TEXT)
        style.configure("Muted.TLabel", background=PANEL, foreground=MUTED)
        style.configure("Heading.TLabel", background=BG, foreground=TEXT,
                        font=("Segoe UI Semibold", 21))
        style.configure("Section.TLabel", background=PANEL, foreground=TEXT,
                        font=("Segoe UI Semibold", 12))
        style.configure("Status.TLabel", background=PANEL, foreground=ACCENT,
                        font=("Segoe UI Semibold", 11))
        style.configure("TButton", background=PANEL_ALT, foreground=TEXT,
                        borderwidth=0, padding=(12, 8))
        style.map("TButton", background=[("active", "#2A3B4F"), ("disabled", PANEL)])
        style.configure("Primary.TButton", background=ACCENT_DARK, foreground="#FFFFFF",
                        padding=(13, 8))
        style.map("Primary.TButton", background=[("active", "#328B83")])
        style.configure("TEntry", fieldbackground=PANEL_ALT, foreground=TEXT,
                        insertcolor=TEXT, borderwidth=0, padding=7)
        style.configure("TCombobox", fieldbackground=PANEL_ALT, foreground=TEXT,
                        background=PANEL_ALT, arrowcolor=TEXT, padding=6)
        style.map("TCombobox", fieldbackground=[("readonly", PANEL_ALT)],
                  foreground=[("readonly", TEXT)])
        style.configure("Horizontal.TScale", background=PANEL, troughcolor=PANEL_ALT)
        style.configure("TCheckbutton", background=PANEL, foreground=TEXT)
        style.map("TCheckbutton", background=[("active", PANEL)])

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=(24, 20))
        outer.pack(fill="both", expand=True)
        header = ttk.Frame(outer)
        header.pack(fill="x", pady=(0, 14))
        ttk.Label(header, text="ESP32 Gamepad Bridge", style="Heading.TLabel").pack(side="left")
        ttk.Label(header, text="Bluetooth HID → USB → виртуальный DualShock 4",
                  foreground=MUTED).pack(side="left", padx=(16, 0), pady=(8, 0))

        self.port_panel = self._panel(outer)
        self.port_panel.pack(fill="x", pady=(0, 12))
        ttk.Label(self.port_panel, text="Подключение к ESP32",
                  style="Section.TLabel").grid(row=0, column=0, sticky="w", columnspan=4)
        ttk.Label(self.port_panel, text="COM-порт", style="Muted.TLabel").grid(
            row=1, column=0, sticky="w", pady=(12, 5))
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(self.port_panel, textvariable=self.port_var,
                                       state="readonly", width=22)
        self.port_combo.grid(row=2, column=0, sticky="ew", padx=(0, 8))
        ttk.Button(self.port_panel, text="Обновить", command=self._refresh_ports).grid(
            row=2, column=1, padx=(0, 8))
        ttk.Label(self.port_panel, text="Скорость", style="Muted.TLabel").grid(
            row=1, column=2, sticky="w", pady=(12, 5))
        self.baud_var = tk.StringVar(value=str(BAUD_RATE))
        ttk.Entry(self.port_panel, textvariable=self.baud_var, width=12).grid(
            row=2, column=2, sticky="w")
        self.connect_button = ttk.Button(self.port_panel, text="Подключить",
                                         style="Primary.TButton", command=self._toggle_connection)
        self.connect_button.grid(row=2, column=3, sticky="e", padx=(16, 0))
        self.port_panel.columnconfigure(0, weight=1)

        main_row = ttk.Frame(outer)
        main_row.pack(fill="x", pady=(0, 12))
        main_row.columnconfigure(0, weight=1)
        main_row.columnconfigure(1, weight=1)
        self._build_controller_panel(main_row)
        self._build_output_panel(main_row)

        self._build_reports_panel(outer)
        self._set_connection_label(False)

    @staticmethod
    def _panel(parent: ttk.Widget, padding: int = 16) -> ttk.Frame:
        return ttk.Frame(parent, style="Panel.TFrame", padding=padding)

    def _build_controller_panel(self, parent: ttk.Frame) -> None:
        panel = self._panel(parent)
        panel.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        ttk.Label(panel, text="Состояние", style="Section.TLabel").pack(anchor="w")
        self.link_label = ttk.Label(panel, text="Ожидание платы", style="Status.TLabel")
        self.link_label.pack(anchor="w", pady=(11, 4))
        self.controller_label = ttk.Label(panel, text="Контроллер: не обнаружен",
                                          style="Muted.TLabel")
        self.controller_label.pack(anchor="w")
        self.bonds_label = ttk.Label(panel, text="Сохранённых сопряжений: —",
                                     style="Muted.TLabel")
        self.bonds_label.pack(anchor="w", pady=(4, 12))
        self.pair_button = ttk.Button(panel, text="Начать поиск и сопряжение",
                                      command=self._toggle_pairing)
        self.pair_button.pack(anchor="w")
        ttk.Separator(panel).pack(fill="x", pady=13)
        self.profile_labels: dict[int, ttk.Label] = {}
        for slot in range(GAMEPAD_SLOTS):
            row = ttk.Frame(panel, style="Panel.TFrame")
            row.pack(fill="x", pady=(7, 0))
            ttk.Button(
                row, text=f"Профиль геймпада {slot + 1}",
                command=lambda selected=slot: self._load_profile(selected),
            ).pack(side="left")
            label = ttk.Label(row, text="не загружен", style="Muted.TLabel",
                              wraplength=220)
            label.pack(side="left", padx=(9, 0))
            self.profile_labels[slot] = label
        self.ds4_label = ttk.Label(panel, text="", style="Muted.TLabel", wraplength=390)
        self.ds4_label.pack(anchor="w", pady=(10, 0))

    def _build_output_panel(self, parent: ttk.Frame) -> None:
        panel = self._panel(parent)
        panel.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        ttk.Label(panel, text="Команды геймпаду", style="Section.TLabel").pack(anchor="w")
        ttk.Label(panel, text="Подсветка", style="Muted.TLabel").pack(anchor="w", pady=(10, 5))
        color_row = ttk.Frame(panel, style="Panel.TFrame")
        color_row.pack(fill="x")
        self.slot_var = tk.StringVar(value="Геймпад 1")
        self.slot_combo = ttk.Combobox(
            color_row, textvariable=self.slot_var,
            values=[f"Геймпад {slot + 1}" for slot in range(GAMEPAD_SLOTS)],
            state="readonly", width=13,
        )
        self.slot_combo.pack(side="left", padx=(0, 8))
        self.slot_combo.bind("<<ComboboxSelected>>", self._on_target_slot_changed)
        self.color_swatch = tk.Label(color_row, text="      ", bg=self._rgb_hex(),
                                     relief="flat", borderwidth=0)
        self.color_swatch.pack(side="left", padx=(0, 8))
        self.color_label = ttk.Label(color_row, text=self._rgb_hex(), style="Panel.TLabel")
        self.color_label.pack(side="left")
        ttk.Button(color_row, text="Выбрать цвет", command=self._choose_color).pack(
            side="right")
        ttk.Label(panel, text="Яркость", style="Muted.TLabel").pack(anchor="w", pady=(10, 1))
        self.brightness_var = tk.IntVar(value=100)
        ttk.Scale(panel, from_=0, to=100, variable=self.brightness_var,
                  orient="horizontal").pack(fill="x")
        self.send_color_button = ttk.Button(panel, text="Отправить цвет",
                                            command=self._send_lightbar)
        self.send_color_button.pack(anchor="w", pady=(5, 10))
        ttk.Label(panel, text="Вибрация", style="Muted.TLabel").pack(anchor="w")
        self.rumble_var = tk.IntVar(value=0)
        ttk.Scale(panel, from_=0, to=255, variable=self.rumble_var,
                  orient="horizontal").pack(fill="x", pady=(2, 0))
        self.rumble_value_label = ttk.Label(panel, text="0 / 255", style="Muted.TLabel")
        self.rumble_value_label.pack(anchor="w")
        self.rumble_var.trace_add("write", self._on_rumble_change)
        self.send_rumble_button = ttk.Button(panel, text="Включить вибрацию",
                                             command=self._send_rumble)
        self.send_rumble_button.pack(anchor="w", pady=(7, 0))
        ttk.Label(panel, text="Для каждого геймпада нужен профиль его HID-отчётов.",
                  style="Muted.TLabel", wraplength=380).pack(anchor="w", pady=(10, 0))
        self._set_output_enabled(False)

    def _build_reports_panel(self, parent: ttk.Frame) -> None:
        panel = self._panel(parent)
        panel.pack(fill="both", expand=True)
        header = ttk.Frame(panel, style="Panel.TFrame")
        header.pack(fill="x")
        ttk.Label(header, text="HID-отчёты", style="Section.TLabel").pack(side="left")
        self.report_summary = ttk.Label(header, text="Пока нет данных",
                                        style="Muted.TLabel")
        self.report_summary.pack(side="left", padx=(14, 0))
        ttk.Button(header, text="Сохранить/остановить запись",
                   command=self._toggle_capture).pack(side="right")
        self.capture_label = ttk.Label(header, text="Запись выключена",
                                       style="Muted.TLabel")
        self.capture_label.pack(side="right", padx=(0, 10))

        self.report_text = tk.Text(
            panel, height=12, bg="#0C131C", fg="#C6D7E7", insertbackground=TEXT,
            relief="flat", borderwidth=0, font=("Cascadia Mono", 9),
            padx=12, pady=10, wrap="word",
        )
        self.report_text.pack(fill="both", expand=True, pady=(11, 0))
        self.report_text.configure(state="disabled")

    def _refresh_ports(self) -> None:
        try:
            from serial.tools import list_ports

            ports = list(list_ports.comports())
            items = [f"{port.device} — {port.description}" for port in ports]
            self._port_devices = {item: port.device for item, port in zip(items, ports)}
            self.port_combo["values"] = items
            if items and self.port_var.get() not in self._port_devices:
                self.port_var.set(items[0])
            if not items:
                self.port_var.set("")
        except Exception as exc:
            self._port_devices = {}
            self._append_log(f"COM-порты недоступны: {exc}")

    def _toggle_connection(self) -> None:
        if self.bridge.connected:
            self.bridge.close()
            self.connect_button.configure(text="Подключить")
            self._set_connection_label(False)
            self.link_label.configure(text="Отключено")
            return
        selected = self.port_var.get()
        port = getattr(self, "_port_devices", {}).get(selected, selected.split(" — ")[0])
        if not port:
            messagebox.showwarning("Нет COM-порта", "Выберите COM-порт ESP32.")
            return
        try:
            baud = int(self.baud_var.get())
            if baud <= 0:
                raise ValueError
            self.bridge.open(port, baud)
        except (ValueError, RuntimeError) as exc:
            messagebox.showerror("Ошибка подключения", str(exc))
            return
        self.connect_button.configure(text="Отключить")
        self._set_connection_label(True)
        self._append_log(f"Открыт {port} на скорости {baud}. Ожидание ESP32…")

    def _set_connection_label(self, connected: bool) -> None:
        self.connect_button.configure(text="Отключить" if connected else "Подключить")

    def _toggle_pairing(self) -> None:
        if not self.bridge.connected:
            messagebox.showinfo("Нет соединения", "Сначала подключите ESP32 по USB.")
            return
        enable = not self.pairing_enabled
        try:
            self.bridge.send(FrameType.PAIR_MODE, bytes((1 if enable else 0,)))
            self.pairing_enabled = enable
            self.pair_button.configure(
                text="Остановить поиск" if enable else "Начать поиск и сопряжение")
            self._append_log("Включён режим поиска геймпада." if enable
                             else "Режим поиска выключен.")
        except RuntimeError as exc:
            self._append_log(str(exc), error=True)

    def _load_default_profile(self) -> None:
        path = Path(__file__).parent / "profiles" / "ds4_bluetooth.json"
        try:
            profile = ReportProfile.load(path)
            for slot in range(GAMEPAD_SLOTS):
                self._set_profile(profile, slot)
        except ValueError as exc:
            self._append_log(str(exc), error=True)

    def _load_profile(self, slot: int | None = None) -> None:
        slot = self.selected_slot if slot is None else slot
        path = filedialog.askopenfilename(
            title="Выберите JSON-профиль геймпада",
            filetypes=[("Профили JSON", "*.json"), ("Все файлы", "*.*")],
        )
        if not path:
            return
        try:
            self._set_profile(ReportProfile.load(path), slot)
        except ValueError as exc:
            messagebox.showerror("Некорректный профиль", str(exc))

    def _set_profile(self, profile: ReportProfile, slot: int) -> None:
        self.profiles[slot] = profile
        self.profile_labels[slot].configure(text=profile.name)
        self._sync_output_controls()
        self._update_ds4_status()
        self._append_log(f"Для геймпада {slot + 1} загружен профиль «{profile.name}».")

    def _set_output_enabled(self, enabled: bool) -> None:
        state = "normal" if enabled else "disabled"
        self.send_color_button.configure(state=state)
        self.send_rumble_button.configure(state=state)

    def _sync_output_controls(self) -> None:
        profile = self.profiles.get(self.selected_slot)
        can_send = (self.bridge.connected and
                    bool(self.controllers.get(self.selected_slot, {}).get("connected")))
        self.send_color_button.configure(
            state="normal" if can_send and profile is not None
                              and profile.output_config.get("lightbar") is not None else "disabled"
        )
        self.send_rumble_button.configure(
            state="normal" if can_send and profile is not None
                              and profile.output_config.get("rumble") is not None else "disabled"
        )

    def _selected_slot(self) -> int:
        try:
            return max(0, min(GAMEPAD_SLOTS - 1, int(self.slot_var.get().rsplit(" ", 1)[1]) - 1))
        except (ValueError, IndexError):
            return 0

    def _on_target_slot_changed(self, _event: object = None) -> None:
        self.selected_slot = self._selected_slot()
        self.rgb = self.colors[self.selected_slot]
        self.color_swatch.configure(bg=self._rgb_hex())
        self.color_label.configure(text=self._rgb_hex())
        self._sync_output_controls()

    def _choose_color(self) -> None:
        chosen = colorchooser.askcolor(color=self._rgb_hex(), title="Цвет подсветки")
        if chosen[0] is None:
            return
        self.rgb = tuple(round(value) for value in chosen[0])
        self.colors[self.selected_slot] = self.rgb
        self.color_swatch.configure(bg=self._rgb_hex())
        self.color_label.configure(text=self._rgb_hex())

    def _rgb_hex(self) -> str:
        return "#%02X%02X%02X" % self.rgb

    def _send_lightbar(self, slot: int | None = None) -> None:
        slot = self.selected_slot if slot is None else slot
        if not self._require_profile_output("lightbar", slot):
            return
        profile = self.profiles[slot]
        brightness = max(0, min(100, int(self.brightness_var.get()))) / 100
        color = tuple(round(value * brightness) for value in self.colors[slot])
        report = profile.build_lightbar_report(*color)
        if report is not None:
            self._send_output_report(report, slot)

    def _send_rumble(self, slot: int | None = None) -> None:
        slot = self.selected_slot if slot is None else slot
        if not self._require_profile_output("rumble", slot):
            return
        profile = self.profiles[slot]
        value = max(0, min(255, int(self.rumble_var.get())))
        report = profile.build_rumble_report(value, value)
        if report is not None:
            self._send_output_report(report, slot)

    def _require_profile_output(self, name: str, slot: int) -> bool:
        if not self.bridge.connected:
            messagebox.showinfo("Нет соединения", "Сначала подключите ESP32 по USB.")
            return False
        profile = self.profiles.get(slot)
        if profile is None or profile.output_config.get(name) is None:
            messagebox.showinfo(
                "Нужен профиль геймпада",
                "Для этого геймпада ещё не настроен формат HID-команды. "
                "Сохраните несколько сырых отчётов и укажите точную модель.",
            )
            return False
        return True

    def _send_output_report(self, report: OutputReport, slot: int) -> None:
        payload = bytes((slot, report.map_index)) + struct.pack(
            "<HH", report.report_id, len(report.data)
        ) + report.data
        try:
            sequence = self.bridge.send(FrameType.HID_OUTPUT, payload)
            self._append_log(
                f"Команда HID отправлена геймпаду {slot + 1} "
                f"(report {report.report_id}, "
                f"{len(report.data)} байт, seq {sequence})."
            )
        except RuntimeError as exc:
            self._append_log(str(exc), error=True)

    def _on_rumble_change(self, *_args: object) -> None:
        self.rumble_value_label.configure(text=f"{int(self.rumble_var.get())} / 255")

    def _toggle_capture(self) -> None:
        if self.capture_file is not None:
            self.capture_file = None
            self.capture_label.configure(text="Запись выключена")
            self._append_log(f"Записано HID-отчётов: {self.raw_report_count}.")
            return
        selected = filedialog.asksaveasfilename(
            title="Сохранить запись HID",
            defaultextension=".jsonl",
            initialfile="gamepad-reports.jsonl",
            filetypes=[("JSON Lines", "*.jsonl"), ("Все файлы", "*.*")],
        )
        if not selected:
            return
        self.capture_file = Path(selected)
        self.raw_report_count = 0
        self.capture_label.configure(text="Идёт запись")
        self._append_log(f"Запись отчётов: {self.capture_file.name}")

    def _drain_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "frame":
                    self._handle_frame(value)  # type: ignore[arg-type]
                elif kind == "error":
                    self._append_log(f"Ошибка последовательного порта: {value}", error=True)
                    self._set_connection_label(False)
                    self._set_output_enabled(False)
        except queue.Empty:
            pass
        self.root.after(50, self._drain_events)

    def _handle_frame(self, frame: Frame) -> None:
        if frame.frame_type == FrameType.STATUS:
            self._handle_status(frame.payload)
        elif frame.frame_type == FrameType.HID_INPUT:
            self._handle_input_report(frame.payload)
        elif frame.frame_type == FrameType.ACK and len(frame.payload) >= 2:
            command, result = frame.payload[:2]
            if result == 0:
                self._append_log(f"ESP32 подтвердил команду 0x{command:02X}.")
            else:
                self._append_log(f"ESP32 отклонил команду 0x{command:02X} "
                                 f"(код {result}).", error=True)

    def _handle_status(self, payload: bytes) -> None:
        try:
            state, bond_count, slots = decode_status(payload)
        except ValueError as exc:
            self._append_log(f"Некорректный статус ESP32: {exc}", error=True)
            return
        previous = self.controllers
        self.controllers = slots
        self.bond_count = bond_count
        self.link_state = state
        connected_slots = [
            slot for slot in range(GAMEPAD_SLOTS)
            if bool(self.controllers.get(slot, {}).get("connected"))
        ]
        if connected_slots:
            self.link_label.configure(text=f"Подключено геймпадов: {len(connected_slots)}/{GAMEPAD_SLOTS}")
        else:
            self.link_label.configure(text=STATE_NAMES.get(state, f"Состояние {state}"))
        if state == LinkState.PAIRING:
            self.pairing_enabled = True
        elif state == LinkState.SCANNING:
            self.pairing_enabled = False
        names = [
            f"{slot + 1}: {self.controllers[slot].get('name') or 'подключён'}"
            for slot in connected_slots
        ]
        self.controller_label.configure(
            text="Контроллеры: " + ("; ".join(names) if names else "не обнаружены")
        )
        self.bonds_label.configure(text=f"Сохранённых сопряжений: {self.bond_count}")
        self.pair_button.configure(
            text="Остановить поиск" if self.pairing_enabled
            else "Начать поиск и сопряжение"
        )
        self._sync_output_controls()
        for slot in range(GAMEPAD_SLOTS):
            was_connected = bool(previous.get(slot, {}).get("connected"))
            now_connected = bool(self.controllers.get(slot, {}).get("connected"))
            if was_connected and not now_connected:
                self.virtual_ds4.neutral(slot)

    def _handle_input_report(self, payload: bytes) -> None:
        if len(payload) < 6:
            self._append_log("Получен некорректный HID-отчёт.", error=True)
            return
        slot, report_id, map_index, usage = struct.unpack("<BHBH", payload[:6])
        if slot >= GAMEPAD_SLOTS:
            self._append_log(f"Получен HID-отчёт с неизвестного слота {slot}.", error=True)
            return
        report = payload[6:]
        self.last_report_id = report_id
        self.last_map_index = map_index
        self.raw_report_count += 1
        self.report_summary.configure(
            text=f"Пад {slot + 1} · ID {report_id} · map {map_index} · "
                 f"usage 0x{usage:04X} · {len(report)} байт"
        )
        self._append_log(f"Пад {slot + 1}: ID={report_id} map={map_index} usage=0x{usage:04X} "
                         f"len={len(report)}  {report.hex(' ')}")
        if self.capture_file is not None:
            record = {
                "time": datetime.now(timezone.utc).isoformat(),
                "slot": slot,
                "report_id": report_id,
                "map_index": map_index,
                "usage": usage,
                "data_hex": report.hex(),
            }
            try:
                with self.capture_file.open("a", encoding="utf-8") as output:
                    output.write(json.dumps(record, ensure_ascii=False) + "\n")
            except OSError as exc:
                self._append_log(f"Не удалось записать отчёт: {exc}", error=True)

        profile = self.profiles.get(slot)
        if profile is not None:
            try:
                state = profile.map_input(report_id, report)
                if state is not None:
                    updated = self.virtual_ds4.apply(slot, state)
                    self._update_ds4_status(updated=updated)
            except (KeyError, IndexError, ValueError) as exc:
                self._append_log(f"Ошибка маппинга отчёта: {exc}", error=True)

    def _update_ds4_status(self, updated: bool = False) -> None:
        if self.virtual_ds4.available_count == GAMEPAD_SLOTS:
            mapped_slots = [
                slot for slot, profile in self.profiles.items()
                if (profile.input_config.get("axes")
                    or profile.input_config.get("buttons")
                    or profile.input_config.get("dpad"))
            ]
            if mapped_slots:
                label = ", ".join(str(slot + 1) for slot in mapped_slots)
                text = f"Виртуальные DS4 активны ({label})" if updated else f"Профили загружены: {label}"
            else:
                text = "2 виртуальных DS4 готовы; отчёты пока не сопоставлены"
        elif self.virtual_ds4.available:
            text = (f"Создано виртуальных DS4: {self.virtual_ds4.available_count}/"
                    f"{GAMEPAD_SLOTS}; проверьте ViGEmBus.")
        else:
            text = "VDS4 недоступен. На Windows установите ViGEmBus."
        self.ds4_label.configure(text=text)

    def _append_log(self, message: str, error: bool = False) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.report_text.configure(state="normal")

        # --- ОГРАНИЧЕНИЕ ЛОГА (максимум 100 строк) ---
        # Считаем количество строк в виджете
        current_lines = int(self.report_text.index('end-1c').split('.')[0])

        # Если строк больше 100, удаляем самые старые (верхние 20 строк разом),
        # чтобы не очищать по одной строке на каждый пакет.
        if current_lines > 100:
            self.report_text.delete('1.0', '20.0')
        # ----------------------------------------------

        self.report_text.insert("end", f"[{timestamp}] {message}\n")

        if error:
            last_line = self.report_text.index("end-2l")
            self.report_text.tag_add("error", last_line, "end-1l")
            self.report_text.tag_config("error", foreground=ERROR)

        self.report_text.see("end")
        self.report_text.configure(state="disabled")

    def _close(self) -> None:
        self.bridge.close()
        self.virtual_ds4.close()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    GamepadBridgeApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
