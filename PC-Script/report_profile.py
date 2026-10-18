"""Configurable HID input/output adapters; device-specific offsets are not guessed."""

from __future__ import annotations

import json
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

AXIS_NAMES = {"lx", "ly", "rx", "ry", "lt", "rt"}
BUTTON_NAMES = {
    "cross", "circle", "square", "triangle",
    "l1", "r1", "l3", "r3", "share", "options", "ps", "touchpad",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
}


@dataclass
class ControllerState:
    axes: dict[str, int] = field(default_factory=dict)
    buttons: dict[str, bool] = field(default_factory=dict)
    extended_report: bytes | None = None


@dataclass(frozen=True)
class OutputReport:
    map_index: int
    report_id: int
    data: bytes


@dataclass
class ReportProfile:
    name: str
    input_config: dict[str, Any]
    output_config: dict[str, Any]
    source_path: Path | None = None

    @classmethod
    def load(cls, path: str | Path) -> "ReportProfile":
        profile_path = Path(path)
        try:
            raw = json.loads(profile_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ValueError(f"Не удалось прочитать профиль: {exc}") from exc
        if not isinstance(raw, dict):
            raise ValueError("Профиль должен быть JSON-объектом")
        input_config = raw.get("input", {})
        output_config = raw.get("output", {})
        if not isinstance(input_config, dict) or not isinstance(output_config, dict):
            raise ValueError("Разделы input и output должны быть объектами")
        profile = cls(
            name=str(raw.get("name") or profile_path.stem),
            input_config=input_config,
            output_config=output_config,
            source_path=profile_path,
        )
        profile._validate()
        return profile

    def _validate(self) -> None:
        report_id = self.input_config.get("report_id")
        if report_id is not None and not _is_byte_or_word(report_id, 0xFFFF):
            raise ValueError("input.report_id должен быть от 0 до 65535 или null")

        axes = self.input_config.get("axes", {})
        buttons = self.input_config.get("buttons", [])
        if not isinstance(axes, dict) or not isinstance(buttons, list):
            raise ValueError("input.axes должен быть объектом, input.buttons — списком")
        for name, spec in axes.items():
            if name not in AXIS_NAMES or not isinstance(spec, dict):
                raise ValueError(f"Неизвестная или некорректная ось: {name}")
            _validate_offset(spec.get("offset"), f"input.axes.{name}.offset")
            low = spec.get("min", 0)
            high = spec.get("max", 255)
            if not isinstance(low, int) or not isinstance(high, int) or low >= high:
                raise ValueError(f"Для оси {name} нужны min < max")
        for index, button in enumerate(buttons):
            if not isinstance(button, dict) or button.get("name") not in BUTTON_NAMES:
                raise ValueError(f"Некорректная кнопка input.buttons[{index}]")
            _validate_offset(button.get("offset"), f"input.buttons[{index}].offset")
            if not _is_byte_or_word(button.get("mask"), 0xFF) or button["mask"] == 0:
                raise ValueError(f"Для кнопки {index} нужен ненулевой mask 1..255")

        dpad = self.input_config.get("dpad")
        if dpad is not None:
            if not isinstance(dpad, dict):
                raise ValueError("input.dpad должен быть объектом или null")
            _validate_offset(dpad.get("offset"), "input.dpad.offset")
            if not _is_byte_or_word(dpad.get("mask"), 0xFF) or dpad["mask"] == 0:
                raise ValueError("input.dpad.mask должен быть в диапазоне 1..255")
            if not isinstance(dpad.get("values"), dict):
                raise ValueError("input.dpad.values должен быть объектом")
            for value, names in dpad["values"].items():
                if not value.isdigit() or not isinstance(names, list):
                    raise ValueError("Значения D-pad задаются как числа со списком направлений")
                if any(name not in BUTTON_NAMES for name in names):
                    raise ValueError(f"Неизвестное направление D-pad: {names}")

        extended = self.input_config.get("ds4_extended")
        if extended is not None:
            if not isinstance(extended, dict):
                raise ValueError("input.ds4_extended должен быть объектом или null")
            for key in ("common_offset", "touch_count_offset", "touch_reports_offset"):
                _validate_offset(extended.get(key), f"input.ds4_extended.{key}")
            for key in ("touch_report_size", "source_touch_count_max", "virtual_touch_count_max"):
                value = extended.get(key)
                if not isinstance(value, int) or value <= 0:
                    raise ValueError(f"input.ds4_extended.{key} должен быть целым числом больше 0")
            if extended["virtual_touch_count_max"] > 3:
                raise ValueError("input.ds4_extended.virtual_touch_count_max не должен быть больше 3")

        for output_name in ("lightbar", "rumble"):
            report = self.output_config.get(output_name)
            if report is not None:
                _validate_output_report(report, output_name)

    def map_input(self, report_id: int, data: bytes) -> ControllerState | None:
        expected_id = self.input_config.get("report_id")
        if expected_id is not None and report_id != expected_id:
            return None
        axes: dict[str, int] = {}
        buttons: dict[str, bool] = {}

        for name, spec in self.input_config.get("axes", {}).items():
            offset = spec["offset"]
            if offset >= len(data):
                continue
            raw = data[offset]
            minimum = spec.get("min", 0)
            maximum = spec.get("max", 255)
            value = round((raw - minimum) * 255 / (maximum - minimum))
            value = max(0, min(255, value))
            if spec.get("invert", False):
                value = 255 - value
            axes[name] = int(value)

        for item in self.input_config.get("buttons", []):
            offset = item["offset"]
            if offset < len(data):
                set_bit = (data[offset] & item["mask"]) != 0
                buttons[item["name"]] = set_bit == item.get("active_high", True)

        dpad = self.input_config.get("dpad")
        if dpad and dpad["offset"] < len(data):
            value = data[dpad["offset"]] & dpad["mask"]
            for name in ("dpad_up", "dpad_down", "dpad_left", "dpad_right"):
                buttons[name] = False
            names = dpad["values"].get(str(value), [])
            for name in names:
                buttons[name] = True

        extended_report = self._map_ds4_extended(data)
        return ControllerState(
            axes=axes, buttons=buttons, extended_report=extended_report
        )

    def _map_ds4_extended(self, data: bytes) -> bytes | None:
        config = self.input_config.get("ds4_extended")
        if config is None:
            return None
        common_offset = config["common_offset"]
        touch_count_offset = config["touch_count_offset"]
        touch_reports_offset = config["touch_reports_offset"]
        touch_size = config["touch_report_size"]
        if (common_offset + 32 > len(data)
                or touch_count_offset >= len(data)
                or touch_reports_offset > len(data)):
            return None

        available_records = (len(data) - touch_reports_offset) // touch_size
        source_count = min(
            data[touch_count_offset],
            config["source_touch_count_max"],
            available_records,
        )
        output_count = min(source_count, config["virtual_touch_count_max"])
        first_record = source_count - output_count
        start = touch_reports_offset + first_record * touch_size
        end = start + output_count * touch_size

        # ViGEm's DS4_REPORT_EX input body is the 32-byte common report,
        # followed by a count and up to three 9-byte touch snapshots.
        report = bytearray(63)
        report[:32] = data[common_offset:common_offset + 32]
        report[32] = output_count
        report[33:33 + output_count * touch_size] = data[start:end]
        return bytes(report)

    def build_lightbar_report(self, red: int, green: int, blue: int) -> OutputReport | None:
        return self._build_output("lightbar", {"r": red, "g": green, "b": blue})

    def build_rumble_report(self, weak: int, strong: int) -> OutputReport | None:
        return self._build_output("rumble", {"weak": weak, "strong": strong})

    def _build_output(self, name: str, values: dict[str, int]) -> OutputReport | None:
        config = self.output_config.get(name)
        if config is None:
            return None
        data = bytearray.fromhex(config["template_hex"])
        for key, offset in config.get("offsets", {}).items():
            data[offset] = max(0, min(255, int(values[key])))
        checksum = config.get("crc32")
        if checksum is not None:
            checksum_offset = checksum["offset"]
            crc_input = bytes((checksum["seed"],))
            if checksum.get("include_report_id", False):
                crc_input += config["report_id"].to_bytes(1, "little")
            crc_input += data[:checksum_offset]
            data[checksum_offset:checksum_offset + 4] = zlib.crc32(crc_input).to_bytes(
                4, "little"
            )
        return OutputReport(
            map_index=config.get("map_index", 0),
            report_id=config["report_id"],
            data=bytes(data),
        )


def _validate_output_report(config: Any, name: str) -> None:
    if not isinstance(config, dict):
        raise ValueError(f"output.{name} должен быть объектом или null")
    if not _is_byte_or_word(config.get("report_id"), 0xFFFF):
        raise ValueError(f"output.{name}.report_id должен быть от 0 до 65535")
    if not _is_byte_or_word(config.get("map_index", 0), 0xFF):
        raise ValueError(f"output.{name}.map_index должен быть от 0 до 255")
    try:
        data = bytes.fromhex(config.get("template_hex", ""))
    except (TypeError, ValueError) as exc:
        raise ValueError(f"output.{name}.template_hex должен быть HEX-строкой") from exc
    if not data:
        raise ValueError(f"output.{name}.template_hex не должен быть пустым")
    offsets = config.get("offsets", {})
    if not isinstance(offsets, dict):
        raise ValueError(f"output.{name}.offsets должен быть объектом")
    expected_keys = {"r", "g", "b"} if name == "lightbar" else {"weak", "strong"}
    for key in expected_keys:
        if key not in offsets:
            raise ValueError(f"В output.{name}.offsets не задано поле {key}")
        _validate_offset(offsets[key], f"output.{name}.offsets.{key}")
        if offsets[key] >= len(data):
            raise ValueError(f"Смещение {key} выходит за длину output.{name}.template_hex")
    checksum = config.get("crc32")
    if checksum is not None:
        if not isinstance(checksum, dict):
            raise ValueError(f"output.{name}.crc32 должен быть объектом или null")
        _validate_offset(checksum.get("offset"), f"output.{name}.crc32.offset")
        if checksum["offset"] + 4 > len(data):
            raise ValueError(f"output.{name}.crc32.offset выходит за длину template_hex")
        if not _is_byte_or_word(checksum.get("seed"), 0xFF):
            raise ValueError(f"output.{name}.crc32.seed должен быть от 0 до 255")
        if not isinstance(checksum.get("include_report_id", False), bool):
            raise ValueError(f"output.{name}.crc32.include_report_id должен быть true или false")


def _validate_offset(value: Any, label: str) -> None:
    if not isinstance(value, int) or value < 0:
        raise ValueError(f"{label} должен быть целым числом не меньше 0")


def _is_byte_or_word(value: Any, maximum: int) -> bool:
    return isinstance(value, int) and 0 <= value <= maximum
