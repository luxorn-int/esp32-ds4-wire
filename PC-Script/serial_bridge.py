"""Threaded serial transport for the ESP32 bridge."""

from __future__ import annotations

import threading
from collections.abc import Callable

from bridge_protocol import FrameParser, encode_frame


class SerialBridge:
    def __init__(self, on_frame: Callable, on_error: Callable) -> None:
        self.on_frame = on_frame
        self.on_error = on_error
        self._serial = None
        self._thread: threading.Thread | None = None
        self._write_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._running = False
        self._sequence = 0

    @property
    def connected(self) -> bool:
        with self._state_lock:
            return self._running and self._serial is not None

    def open(self, port: str, baudrate: int = 921600) -> None:
        if self.connected:
            self.close()
        try:
            import serial

            connection = serial.Serial(port, baudrate, timeout=0.1, write_timeout=1)
        except Exception as exc:
            raise RuntimeError(f"Не удалось открыть {port}: {exc}") from exc

        with self._state_lock:
            self._serial = connection
            self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True, name="serial-reader")
        self._thread.start()
        try:
            self.send(0x01)
        except RuntimeError:
            self.close()
            raise

    def send(self, frame_type: int, payload: bytes = b"") -> int:
        with self._state_lock:
            connection = self._serial
            if not self._running or connection is None:
                raise RuntimeError("Последовательный порт не подключён")
            sequence = self._sequence
            self._sequence = (self._sequence + 1) & 0xFFFF
        packet = encode_frame(frame_type, sequence, payload)
        try:
            with self._write_lock:
                connection.write(packet)
                connection.flush()
        except Exception as exc:
            raise RuntimeError(f"Ошибка передачи по COM-порту: {exc}") from exc
        return sequence

    def close(self) -> None:
        with self._state_lock:
            self._running = False
            connection = self._serial
            self._serial = None
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass

    def _read_loop(self) -> None:
        parser = FrameParser()
        while True:
            with self._state_lock:
                running = self._running
                connection = self._serial
            if not running or connection is None:
                return
            try:
                chunk = connection.read(512)
            except Exception as exc:
                with self._state_lock:
                    was_running = self._running
                    self._running = False
                    self._serial = None
                try:
                    connection.close()
                except Exception:
                    pass
                if was_running:
                    self.on_error(str(exc))
                return
            if chunk:
                for frame in parser.feed(chunk):
                    self.on_frame(frame)
