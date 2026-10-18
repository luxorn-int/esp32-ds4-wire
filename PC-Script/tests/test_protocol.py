import struct
import unittest

from bridge_protocol import (
    FrameParser,
    FrameType,
    LinkState,
    crc16_ccitt_false,
    decode_status,
    encode_frame,
)


class ProtocolTests(unittest.TestCase):
    def test_crc_ccitt_false_reference_vector(self):
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)

    def test_parser_handles_fragmented_and_concatenated_frames(self):
        first = encode_frame(FrameType.HELLO, 1)
        second = encode_frame(FrameType.PAIR_MODE, 2, b"\x01")
        parser = FrameParser()
        self.assertEqual(parser.feed(first[:5]), [])
        self.assertEqual(parser.feed(first[5:] + second), [
            parser_frame(FrameType.HELLO, 1, b""),
            parser_frame(FrameType.PAIR_MODE, 2, b"\x01"),
        ])

    def test_v2_status_contains_two_independent_slots(self):
        payload = bytes((LinkState.CONNECTED, 2, 2))
        payload += bytes((0, 1)) + bytes.fromhex("010203040506") + bytes((3,)) + b"Pad"
        payload += bytes((1, 0)) + bytes(6) + bytes((0,))
        state, bonds, slots = decode_status(payload)
        self.assertEqual((state, bonds), (LinkState.CONNECTED, 2))
        self.assertEqual(slots[0]["name"], "Pad")
        self.assertTrue(slots[0]["connected"])
        self.assertFalse(slots[1]["connected"])

    def test_input_payload_layout_has_slot_before_report_metadata(self):
        payload = struct.pack("<BHBH", 1, 5, 0, 1) + b"\x10\x20"
        slot, report_id, map_index, usage = struct.unpack("<BHBH", payload[:6])
        self.assertEqual((slot, report_id, map_index, usage), (1, 5, 0, 1))
        self.assertEqual(payload[6:], b"\x10\x20")


def parser_frame(frame_type, sequence, payload):
    # Keep expected values explicit without importing an implementation detail.
    from bridge_protocol import Frame

    return Frame(frame_type, sequence, payload)


if __name__ == "__main__":
    unittest.main()
