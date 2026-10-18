"""Shared ESP32 <-> PC framed serial protocol."""

from __future__ import annotations

from dataclasses import dataclass

MAGIC = b"\xA5\x5A\xA5\x5A"
VERSION = 2
MAX_PAYLOAD = 1024
HEADER_SIZE = 10
FRAME_OVERHEAD = 12
GAMEPAD_SLOTS = 2


class FrameType:
    HELLO = 0x01
    STATUS = 0x02
    ACK = 0x03
    HID_INPUT = 0x10
    PAIR_MODE = 0x20
    HID_OUTPUT = 0x21


def decode_status(payload: bytes) -> tuple[int, int, dict[int, dict[str, object]]]:
    """Decode v2 status: state, bond count, slot count, then slot records."""
    if len(payload) < 3:
        raise ValueError("Укороченный статус ESP32")
    state, bond_count, slot_count = payload[:3]
    offset = 3
    slots: dict[int, dict[str, object]] = {}
    for _ in range(slot_count):
        if len(payload) - offset < 9:
            raise ValueError("Обрезана запись контроллера в статусе ESP32")
        slot, connected = payload[offset], payload[offset + 1]
        address = payload[offset + 2:offset + 8]
        name_length = payload[offset + 8]
        offset += 9
        if len(payload) - offset < name_length:
            raise ValueError("Обрезано имя контроллера в статусе ESP32")
        name = payload[offset:offset + name_length].decode("utf-8", errors="replace")
        offset += name_length
        if slot >= GAMEPAD_SLOTS:
            continue
        slots[slot] = {
            "connected": bool(connected),
            "address": address,
            "name": name,
        }
    if offset != len(payload):
        raise ValueError("В статусе ESP32 есть лишние байты")
    return state, bond_count, slots


class LinkState:
    UNKNOWN = 0
    SCANNING = 1
    CONNECTING = 2
    CONNECTED = 3
    PAIRING = 4
    DISCONNECTED = 5


@dataclass(frozen=True)
class Frame:
    frame_type: int
    sequence: int
    payload: bytes


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_frame(frame_type: int, sequence: int, payload: bytes = b"") -> bytes:
    if not 0 <= frame_type <= 0xFF:
        raise ValueError("Тип кадра должен помещаться в один байт")
    if not 0 <= sequence <= 0xFFFF:
        raise ValueError("Номер кадра должен помещаться в два байта")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"Слишком длинный пакет: максимум {MAX_PAYLOAD} байт")

    header = bytes((VERSION, frame_type)) + sequence.to_bytes(2, "little")
    header += len(payload).to_bytes(2, "little")
    body = header + payload
    return MAGIC + body + crc16_ccitt_false(body).to_bytes(2, "little")


class FrameParser:
    """Incremental parser that resynchronizes after noise, truncation or CRC errors."""

    def __init__(self) -> None:
        self._buffer = bytearray()
        self.crc_errors = 0
        self.discarded_bytes = 0

    def feed(self, data: bytes) -> list[Frame]:
        self._buffer.extend(data)
        frames: list[Frame] = []

        while True:
            marker_at = self._buffer.find(MAGIC)
            if marker_at < 0:
                # Keep a possible partial magic marker split between reads.
                keep = min(len(self._buffer), len(MAGIC) - 1)
                discarded = len(self._buffer) - keep
                if discarded:
                    del self._buffer[:discarded]
                    self.discarded_bytes += discarded
                break
            if marker_at:
                del self._buffer[:marker_at]
                self.discarded_bytes += marker_at
            if len(self._buffer) < HEADER_SIZE:
                break

            version = self._buffer[4]
            frame_type = self._buffer[5]
            sequence = int.from_bytes(self._buffer[6:8], "little")
            payload_length = int.from_bytes(self._buffer[8:10], "little")
            if version != VERSION or payload_length > MAX_PAYLOAD:
                del self._buffer[0]
                self.discarded_bytes += 1
                continue

            frame_length = FRAME_OVERHEAD + payload_length
            if len(self._buffer) < frame_length:
                break

            crc_start = 4
            crc_end = HEADER_SIZE + payload_length
            expected_crc = int.from_bytes(self._buffer[crc_end : crc_end + 2], "little")
            actual_crc = crc16_ccitt_false(bytes(self._buffer[crc_start:crc_end]))
            if expected_crc != actual_crc:
                del self._buffer[0]
                self.crc_errors += 1
                self.discarded_bytes += 1
                continue

            payload = bytes(self._buffer[HEADER_SIZE:crc_end])
            frames.append(Frame(frame_type, sequence, payload))
            del self._buffer[:frame_length]

        return frames
