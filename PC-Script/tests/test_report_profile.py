import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bridge_protocol import FrameParser, FrameType, crc16_ccitt_false, encode_frame


class BridgeProtocolTests(unittest.TestCase):
    def test_crc_reference_vector(self):
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)

    def test_round_trip_and_fragmented_input(self):
        encoded = encode_frame(FrameType.HID_INPUT, 0x1234, b"\x00\xAA\xFF")
        parser = FrameParser()
        self.assertEqual(parser.feed(encoded[:7]), [])
        self.assertEqual(parser.feed(encoded[7:12]), [])
        frames = parser.feed(encoded[12:])
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].frame_type, FrameType.HID_INPUT)
        self.assertEqual(frames[0].sequence, 0x1234)
        self.assertEqual(frames[0].payload, b"\x00\xAA\xFF")

    def test_resynchronizes_after_noise_and_bad_crc(self):
        good = encode_frame(FrameType.STATUS, 7, b"ok")
        bad = bytearray(encode_frame(FrameType.STATUS, 6, b"bad"))
        bad[-1] ^= 0x80
        parser = FrameParser()
        frames = parser.feed(b"noise" + bytes(bad) + good)
        self.assertEqual([frame.sequence for frame in frames], [7])
        self.assertEqual(parser.crc_errors, 1)

    def test_rejects_oversized_payload(self):
        parser = FrameParser()
        malformed = b"\xA5\x5A\xA5\x5A\x01\x10\x00\x00\xFF\xFF"
        self.assertEqual(parser.feed(malformed), [])


if __name__ == "__main__":
    unittest.main()
