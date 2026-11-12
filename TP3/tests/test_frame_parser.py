#!/usr/bin/env python3
"""
Tests de FrameParser (TP3/tools/fft_gui.py) contra tramas sintéticas.

Valida que el parser decodifique bien el formato descripto en
TP3/decisiones.md ("Formato de la trama"):
    Sync(2B=A5 5A) Type(1B) N(2B) Fs(4B) Payload(int16 x L)
donde L = N/2 para FFT y L = N para BYPASS.

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
    FRAME_TYPE_BYPASS,
    FRAME_TYPE_FFT,
    HEADER_FMT,
    SYNC,
    FrameParser,
)


def encode_frame(type_, n, fs, payload):
    """Arma una trama binaria igual a como la manda el firmware real."""
    header = struct.pack(HEADER_FMT, type_, n, fs)
    payload_bytes = np.asarray(payload, dtype="<i2").tobytes()
    expected_length = (n // 2) if type_ == FRAME_TYPE_FFT else n
    assert len(payload_bytes) == expected_length * 2
    return SYNC + header + payload_bytes


def make_fft_frame_bytes(n=512, fs=8000, peak_bin=10):
    length = n // 2
    payload = np.full(length, -4000, dtype=np.int16)  # -40.00 dB "piso"
    payload[peak_bin] = 0  # 0.00 dB en el pico
    return encode_frame(FRAME_TYPE_FFT, n, fs, payload)


def make_bypass_frame_bytes(n=512, fs=8000):
    length = n
    payload = (1000 * np.sin(2 * np.pi * np.arange(length) / 32)).astype(np.int16)
    return encode_frame(FRAME_TYPE_BYPASS, n, fs, payload)


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
    assert frame.peak_bin == 10
    assert frame.window_name == "Rectangular"
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
    assert frame.window_name == "Rectangular"
    assert len(frame.payload) == 512


def test_two_consecutive_frames_in_one_feed():
    raw = make_fft_frame_bytes(n=512) + make_fft_frame_bytes(n=1024)
    parser = FrameParser()
    events = collect(parser, raw)

    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 2
    assert frames[0].n == 512
    assert frames[1].n == 1024


def test_frame_split_across_many_feeds_byte_by_byte():
    raw = make_fft_frame_bytes()
    parser = FrameParser()
    events = []
    for i in range(len(raw)):
        events.extend(parser.feed(raw[i:i + 1]))

    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 1
    assert frames[0].n == 512


def test_invalid_header_is_discarded_and_recovers():
    # Sync seguido de parámetros no válidos (ej. tipo 99, N=123, Fs=999)
    invalid_header = struct.pack(HEADER_FMT, 99, 123, 999)
    bad_stream = SYNC + invalid_header + make_fft_frame_bytes(n=1024)

    parser = FrameParser()
    events = collect(parser, bad_stream)
    frames = [f for k, f in events if k == "frame"]

    assert len(frames) == 1
    assert frames[0].n == 1024


def test_resync_after_garbage_with_fake_sync():
    garbage = b"ruido" + SYNC + b"\x00\x00no es un header valido"
    stream = garbage + make_fft_frame_bytes(n=2048)

    parser = FrameParser()
    events = collect(parser, stream)
    frames = [f for k, f in events if k == "frame"]

    assert len(frames) == 1
    assert frames[0].n == 2048


def test_text_lines_interleaved_with_binary_frame():
    stream = (
        b"TP3 DSP - FFT (FRDM-MCXN947)\r\n"
        b"[INFO] Frecuencia de muestreo: 8000 Hz\r\n"
        + make_fft_frame_bytes(n=512)
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
    assert frames[0].n == 512


def test_partial_frame_waits_for_more_data():
    raw = make_fft_frame_bytes()
    parser = FrameParser()

    events = collect(parser, raw[:6])  # sync + parte del header
    assert events == []

    events = collect(parser, raw[6:])
    frames = [f for k, f in events if k == "frame"]
    assert len(frames) == 1


def test_peak_calculation_on_pc():
    # N=512, Fs=8000 -> df = 15.625 Hz. Pico en bin 64 -> 1000 Hz.
    raw = make_fft_frame_bytes(n=512, fs=8000, peak_bin=64)
    parser = FrameParser()
    events = collect(parser, raw)
    assert len(events) == 1
    _, frame = events[0]
    assert frame.peak_bin == 64
    assert abs(frame.peak_freq_hz - 1000.0) < 1e-9
    assert abs(frame.peak_db - 0.0) < 1e-9


def test_peak_calculation_ignores_dc():
    # Simula un offset de continua grande en bin 0 (+5.00 dB = 500)
    # y el pico real de la señal en bin 20 (-10.00 dB = -1000).
    length = 256
    payload = np.full(length, -5000, dtype=np.int16)
    payload[0] = 500     # DC grande
    payload[20] = -1000  # Pico de señal AC
    raw = encode_frame(FRAME_TYPE_FFT, 512, 8000, payload)

    parser = FrameParser()
    events = collect(parser, raw)
    assert len(events) == 1
    _, frame = events[0]
    assert frame.peak_bin == 20  # Ignora bin 0 y detecta bin 20
    assert abs(frame.peak_db - (-10.0)) < 1e-9


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
