"""Optional Windows ViGEm/vgamepad output adapter."""

from __future__ import annotations

from report_profile import ControllerState
from bridge_protocol import GAMEPAD_SLOTS


BUTTON_CONSTANTS = {
    "cross": "DS4_BUTTON_CROSS",
    "circle": "DS4_BUTTON_CIRCLE",
    "square": "DS4_BUTTON_SQUARE",
    "triangle": "DS4_BUTTON_TRIANGLE",
    "l1": "DS4_BUTTON_SHOULDER_LEFT",
    "r1": "DS4_BUTTON_SHOULDER_RIGHT",
    "l3": "DS4_BUTTON_THUMB_LEFT",
    "r3": "DS4_BUTTON_THUMB_RIGHT",
    "share": "DS4_BUTTON_SHARE",
    "options": "DS4_BUTTON_OPTIONS",
}

SPECIAL_BUTTONS = {
    "ps": "DS4_SPECIAL_BUTTON_PS",
    "touchpad": "DS4_SPECIAL_BUTTON_TOUCHPAD",
}

DPAD_DIRECTIONS = {
    frozenset(): "DS4_BUTTON_DPAD_NONE",
    frozenset({"dpad_up"}): "DS4_BUTTON_DPAD_NORTH",
    frozenset({"dpad_up", "dpad_right"}): "DS4_BUTTON_DPAD_NORTHEAST",
    frozenset({"dpad_right"}): "DS4_BUTTON_DPAD_EAST",
    frozenset({"dpad_down", "dpad_right"}): "DS4_BUTTON_DPAD_SOUTHEAST",
    frozenset({"dpad_down"}): "DS4_BUTTON_DPAD_SOUTH",
    frozenset({"dpad_down", "dpad_left"}): "DS4_BUTTON_DPAD_SOUTHWEST",
    frozenset({"dpad_left"}): "DS4_BUTTON_DPAD_WEST",
    frozenset({"dpad_up", "dpad_left"}): "DS4_BUTTON_DPAD_NORTHWEST",
}


class VirtualDS4:
    """Own up to two independent ViGEm DS4 devices, indexed by ESP32 slot."""

    def __init__(self) -> None:
        self._gamepads: list[object | None] = [None] * GAMEPAD_SLOTS
        self.error: str | None = None
        try:
            import vgamepad as vg

            self._vg = vg
            for slot in range(GAMEPAD_SLOTS):
                try:
                    self._gamepads[slot] = vg.VDS4Gamepad()
                    self.apply(slot, ControllerState())
                except Exception as exc:
                    self.error = str(exc)
        except Exception as exc:  # ViGEmBus and vgamepad are Windows-only.
            self._vg = None
            self.error = str(exc)

    @property
    def available(self) -> bool:
        return any(gamepad is not None for gamepad in self._gamepads)

    @property
    def available_count(self) -> int:
        return sum(gamepad is not None for gamepad in self._gamepads)

    def apply(self, slot: int, state: ControllerState) -> bool:
        if not 0 <= slot < len(self._gamepads):
            return False
        pad = self._gamepads[slot]
        if pad is None:
            return False
        vg = self._vg
        for name, enum_name in BUTTON_CONSTANTS.items():
            button = getattr(vg.DS4_BUTTONS, enum_name, None)
            if button is None:
                continue
            if state.buttons.get(name, False):
                pad.press_button(button=button)
            else:
                pad.release_button(button=button)

        for name, enum_name in SPECIAL_BUTTONS.items():
            button = getattr(vg.DS4_SPECIAL_BUTTONS, enum_name, None)
            if button is None:
                continue
            if state.buttons.get(name, False):
                pad.press_special_button(special_button=button)
            else:
                pad.release_special_button(special_button=button)

        pressed_dpad = frozenset(
            name for name in ("dpad_up", "dpad_down", "dpad_left", "dpad_right")
            if state.buttons.get(name, False)
        )
        direction_name = DPAD_DIRECTIONS.get(pressed_dpad, "DS4_BUTTON_DPAD_NONE")
        direction = getattr(vg.DS4_DPAD_DIRECTIONS, direction_name)
        pad.directional_pad(direction=direction)

        pad.left_joystick(
            x_value=state.axes.get("lx", 128),
            y_value=state.axes.get("ly", 128),
        )
        pad.right_joystick(
            x_value=state.axes.get("rx", 128),
            y_value=state.axes.get("ry", 128),
        )
        pad.left_trigger(value=state.axes.get("lt", 0))
        pad.right_trigger(value=state.axes.get("rt", 0))
        pad.update()
        return True

    def neutral(self, slot: int | None = None) -> None:
        slots = range(len(self._gamepads)) if slot is None else (slot,)
        for item in slots:
            self.apply(item, ControllerState())

    def close(self) -> None:
        self.neutral()
        self._gamepads = [None] * len(self._gamepads)
