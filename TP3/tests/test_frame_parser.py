#!/usr/bin/env python3
"""
Tests de FrameParser (TP3/tools/fft_gui.py) contra tramas sintéticas.

No hay firmware real emitiendo el protocolo binario todavía (issues #14-#16
del repo): mientras tanto, estos tests son la única forma de validar que el
parser decodifica bien el formato descripto en TP3/README.md, incluida la
recuperación ante CRC inválido y resincronización ante basura en el canal.

Uso:
    cd TP3/tools && python3 -m pytest ../tests/test_frame_parser.py -v
    (o, sin pytest instalado: python3 ../tests/test_frame_parser.py)
"""

import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from fft_gui import (  # noqa: E402
    CRC_SIZE,
    FRAME_TYPE_BYPASS,
    FRAME_TYPE_FFT,
    HEADER_FMT,
    SYNC,
    WINDOW_HANN,
    WINDOW_RECT,
    FrameParser,
    crc16_ccitt,
)


def encode_frame(type_, window, n, length, fs, seq, peak_bin, payload):
    """Arma una trama binaria igual a como la mandaría el firmware."""
    header = struct.pack(HEADER_FMT, type_, window, n, length, fs, seq, peak_bin)
    payload_bytes = np.asarray(payload, dtype="<i2").tobytes()
    assert len(payload_bytes) == length * 2
    crc = crc16_ccitt(header + payload_bytes)
    return SYNC + header + payload_bytes + struct.pack("<H", crc)


def make_fft_frame_bytes(n=512, fs=8000, seq=1, peak_bin=10, window=WINDOW_HANN):
    length = n // 2
    payload = np.full(length, -4000, dtype=np.int16)  # -40.00 dB "piso"
    payload[peak_bin] = 0  # 0.00 dB en el pico
    return encode_frame(FRAME_TYPE_FFT, window, n, length, fs, seq, peak_bin, payload)


def make_bypass_frame_bytes(n=512, fs=8000, seq=1, window=WINDOW_RECT):
    length = n
    payload = (1000 * np.sin(2 * np.pi * np.arange(length) / 32)).astype(np.int16)
    return encode_frame(FRAME_TYPE_BYPASS, window, n, length, fs, seq, 0, payload)


def collect(parser, data):
    return list(parser.feed(data))


def test_single_fft_frame_roundtrip():
    raw = make_fft_frame_bytes()
    parser = FrameParser()
    events = collect(parser, raw)

    assert len(events) == 1
    kind, frame = events[0]
    assert kind == "frame"
    assert frame.is_fft
    assert frame.n == 512
    assert frame.length == 256
    assert frame.fs == 8000
    assert frame.seq == 1
    assert frame.peak_bin == 10
    assert frame.window == WINDOW_HANN
    db = frame.db()
    assert abs(db[10] - 0.0) < 1e-9
    assert abs(db[0] - (-40.0)) < 1e-9


def test_single_bypass_frame_roundtrip():
    raw = make_bypass_frame_bytes()
    parser = FrameParser()
    events = collect(parser, raw)

    assert len(events) == 1
    kind, frame = events[0]
    assert kind == "frame"
    assert not frame.is_fft
    assert frame.length == frame.n == 512
    assert frame.window == WINDOW_RECT
    assert len(frame.payload) == 512


def test_two_consecutive_frames_in_one_feed():
    raw = make_fft_frame_bytes(seq=1) + make_fft_frame_bytes(seq=2)
    parser = FrameParser()
    events = collect(parser, raw)

    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 2
    assert frames[0].seq == 1
    assert frames[1].seq == 2


def test_frame_split_across_many_feeds_byte_by_byte():
    raw = make_fft_frame_bytes()
    parser = FrameParser()
    events = []
    for i in range(len(raw)):
        events.extend(parser.feed(raw[i:i + 1]))

    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 1
    assert frames[0].seq == 1


def test_crc_error_is_detected_and_recovers():
    raw = bytearray(make_fft_frame_bytes(seq=1))
    # Corrompe un byte del payload (no toca el sync ni el header) para que
    # el CRC no coincida, pero la trama siga siendo "plausible" (N/Length OK).
    raw[len(SYNC) + struct.calcsize(HEADER_FMT) + 4] ^= 0xFF
    corrupted = bytes(raw) + make_fft_frame_bytes(seq=2)

    parser = FrameParser()
    events = collect(parser, corrupted)
    kinds = [k for k, _ in events]

    assert "crc_error" in kinds
    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 1
    assert frames[0].seq == 2  # se recuperó y decodificó la siguiente trama


def test_resync_after_garbage_with_fake_sync():
    garbage = b"ruido" + SYNC + b"\x00\x00no es un header valido"
    stream = garbage + make_fft_frame_bytes(seq=7)

    parser = FrameParser()
    events = collect(parser, stream)
    frames = [f for k, f in events if k == "frame"]

    assert len(frames) == 1
    assert frames[0].seq == 7


def test_text_lines_interleaved_with_binary_frame():
    stream = (
        b"TP3 DSP - FFT (FRDM-MCXN947)\r\n"
        b"[INFO] Frecuencia de muestreo: 8000 Hz\r\n"
        + make_fft_frame_bytes(seq=3)
        + b"[INFO] listo\r\n"
    )

    parser = FrameParser()
    events = collect(parser, stream)

    texts = [p for k, p in events if k == "text"]
    frames = [f for k, f in events if k == "frame"]

    assert "TP3 DSP - FFT (FRDM-MCXN947)" in texts
    assert "[INFO] Frecuencia de muestreo: 8000 Hz" in texts
    assert "[INFO] listo" in texts
    assert len(frames) == 1
    assert frames[0].seq == 3


def test_partial_frame_waits_for_more_data():
    raw = make_fft_frame_bytes()
    parser = FrameParser()

    events = collect(parser, raw[:10])  # sync + parte del header, nada más
    assert events == []

    events = collect(parser, raw[10:])
    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 1


def _run_without_pytest():
    import traceback

    tests = [obj for name, obj in list(globals().items()) if name.startswith("test_") and callable(obj)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except Exception:
            failed += 1
            print(f"FAIL {t.__name__}")
            traceback.print_exc()
    print(f"\n{len(tests) - failed}/{len(tests)} tests OK")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    _run_without_pytest()
