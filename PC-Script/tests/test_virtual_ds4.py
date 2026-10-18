import sys
import types
import unittest
from unittest.mock import patch

from report_profile import ControllerState
from virtual_ds4 import VirtualDS4


class FakeGamepad:
    def __init__(self):
        self.updates = 0
        self.left_x = None

    def press_button(self, **_kwargs): pass
    def release_button(self, **_kwargs): pass
    def press_special_button(self, **_kwargs): pass
    def release_special_button(self, **_kwargs): pass
    def directional_pad(self, **_kwargs): pass
    def left_joystick(self, **kwargs): self.left_x = kwargs["x_value"]
    def right_joystick(self, **_kwargs): pass
    def left_trigger(self, **_kwargs): pass
    def right_trigger(self, **_kwargs): pass
    def update(self): self.updates += 1


def fake_vgamepad_module():
    button_names = (
        "DS4_BUTTON_CROSS", "DS4_BUTTON_CIRCLE", "DS4_BUTTON_SQUARE",
        "DS4_BUTTON_TRIANGLE", "DS4_BUTTON_SHOULDER_LEFT",
        "DS4_BUTTON_SHOULDER_RIGHT", "DS4_BUTTON_THUMB_LEFT",
        "DS4_BUTTON_THUMB_RIGHT", "DS4_BUTTON_SHARE", "DS4_BUTTON_OPTIONS",
    )
    special_names = ("DS4_SPECIAL_BUTTON_PS", "DS4_SPECIAL_BUTTON_TOUCHPAD")
    dpad_names = (
        "DS4_BUTTON_DPAD_NONE", "DS4_BUTTON_DPAD_NORTH",
        "DS4_BUTTON_DPAD_NORTHEAST", "DS4_BUTTON_DPAD_EAST",
        "DS4_BUTTON_DPAD_SOUTHEAST", "DS4_BUTTON_DPAD_SOUTH",
        "DS4_BUTTON_DPAD_SOUTHWEST", "DS4_BUTTON_DPAD_WEST",
        "DS4_BUTTON_DPAD_NORTHWEST",
    )
    return types.SimpleNamespace(
        DS4_BUTTONS=types.SimpleNamespace(**{name: name for name in button_names}),
        DS4_SPECIAL_BUTTONS=types.SimpleNamespace(**{name: name for name in special_names}),
        DS4_DPAD_DIRECTIONS=types.SimpleNamespace(**{name: name for name in dpad_names}),
        VDS4Gamepad=FakeGamepad,
    )


class VirtualDS4Tests(unittest.TestCase):
    def test_two_slots_update_independent_virtual_devices(self):
        with patch.dict(sys.modules, {"vgamepad": fake_vgamepad_module()}):
            devices = VirtualDS4()
            self.assertEqual(devices.available_count, 2)
            self.assertTrue(devices.apply(0, ControllerState(axes={"lx": 12})))
            self.assertTrue(devices.apply(1, ControllerState(axes={"lx": 240})))
            self.assertEqual(devices._gamepads[0].left_x, 12)
            self.assertEqual(devices._gamepads[1].left_x, 240)
            self.assertFalse(devices.apply(2, ControllerState()))
            devices.close()
            self.assertEqual(devices.available_count, 0)


if __name__ == "__main__":
    unittest.main()
