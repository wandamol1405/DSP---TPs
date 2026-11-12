#!/usr/bin/env python3
"""
Firmware falso: simula el stream UART de MCXN947_Project_TP3 sin placa ni
generador de señales, para poder probar TP3/tools/fft_gui.py de punta a
punta (puerto, hilo lector, parseo de tramas, gráficos en vivo, comandos).

Abre un pseudo-terminal (pty): por el extremo "master" manda tramas
sintéticas del protocolo binario (ver TP3/decisiones.md, "Formato de la trama"),
un tono fijo configurable, alternando FFT/Bypass. La GUI se conecta al extremo
"slave" como si fuera el puerto serie real de la placa.

Reacciona a los mismos comandos que el firmware y los botones de la GUI:
  m -> ciclo BYPASS -> FFT 512 -> FFT 1024 -> FFT 2048 -> BYPASS
  f -> siguiente Fs (8/16/22/44/48 kHz)
  r -> alterna RUN / STOP
  c -> continuo on/off
  s -> una trama más si está en pausa
  t -> imprime contadores y estado
  h -> imprime ayuda

Uso:
    cd TP3/tools && venv/bin/python3 ../tests/fake_board.py [tono_hz]

    Copiá el puerto que imprime (ej. /dev/pts/4) en el combo "Puerto" de
    la GUI (es editable a propósito, justamente para esto) y conectate
    normal con el botón "Conectar".
"""

import os
import pty
import select
import struct
import sys
import time
import tty
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from fft_gui import (  # noqa: E402
    FRAME_TYPE_BYPASS,
    FRAME_TYPE_FFT,
    HEADER_FMT,
    SYNC,
)

FS_CYCLE = [8000, 16000, 22000, 44000, 48000]
MODES_CYCLE = [
    (FRAME_TYPE_BYPASS, 512),
    (FRAME_TYPE_FFT, 512),
    (FRAME_TYPE_FFT, 1024),
    (FRAME_TYPE_FFT, 2048),
]
FRAME_PERIOD_S = 0.1  # 10 tramas/s
COMMAND_CHARS = set("mfrcsth")


def write_all(fd: int, data: bytes):
    """os.write() a un pty puede escribir MENOS bytes de los pedidos si su
    buffer interno está lleno (p.ej. tramas Bypass grandes, hasta ~4 KB
    para N=2048, chocan con el límite típico de 4096 B del buffer del
    pty) -- sin reintentar el resto, esos bytes se pierden en silencio y
    el receptor ve una trama truncada en el medio. Reintenta hasta mandar
    todo (con el extremo lector drenando del otro lado, se destraba)."""
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        view = view[n:]


def encode_frame(type_, n, fs, payload) -> bytes:
    """Arma una trama binaria igual a como la mandaría el firmware real
    según TP3/decisiones.md."""
    header = struct.pack(HEADER_FMT, type_, n, fs)
    payload_bytes = np.asarray(payload, dtype="<i2").tobytes()
    return SYNC + header + payload_bytes


class FakeBoard:
    """Estado del firmware simulado: qué manda y cómo reacciona a comandos."""

    def __init__(self, tone_hz: float, amplitude: float = 0.3):
        self.tone_hz = tone_hz
        self.amplitude = amplitude
        self.mode_idx = 2  # arranca en FFT 1024
        self.fs_idx = 0    # arranca en 8 kHz
        self.running = True
        self.single_shot = False

    @property
    def type(self) -> int:
        return MODES_CYCLE[self.mode_idx][0]

    @property
    def n(self) -> int:
        return MODES_CYCLE[self.mode_idx][1]

    @property
    def fs(self) -> int:
        return FS_CYCLE[self.fs_idx]

    def handle_command(self, ch: str):
        if ch == "m":
            self.mode_idx = (self.mode_idx + 1) % len(MODES_CYCLE)
        elif ch == "f":
            self.fs_idx = (self.fs_idx + 1) % len(FS_CYCLE)
        elif ch == "r":
            self.running = not self.running
        elif ch == "c":
            self.running = not self.running
        elif ch == "s":
            self.single_shot = True
        elif ch == "t":
            print(f"[fake_board] Estado: Fs={self.fs} Hz, N={self.n}, modo={self.type}, running={self.running}")
        elif ch == "h":
            print("[fake_board] Comandos: m (modo), f (Fs), r (run/stop), c (continuo), s (single), t (estado), h (ayuda)")

        type_name = "FFT" if self.type == FRAME_TYPE_FFT else "Bypass"
        print(f"[fake_board] comando '{ch}' -> {type_name} fs={self.fs} n={self.n} running={self.running}")

    def build_frame(self) -> bytes:
        n, fs = self.n, self.fs
        t = np.arange(n) / fs
        signal = self.amplitude * np.sin(2 * np.pi * self.tone_hz * t)
        signal += np.random.normal(0, 0.01, n)  # ruido de piso

        if self.type == FRAME_TYPE_BYPASS:
            q15 = np.clip(signal * 32767, -32768, 32767).astype(np.int16)
            return encode_frame(FRAME_TYPE_BYPASS, n, fs, q15)
        else:
            w = np.ones(n)  # Rectangular según decisiones.md
            spectrum = np.fft.rfft(signal * w)
            mag = np.abs(spectrum) / max(np.sum(w) / 2.0, 1e-12)
            length = n // 2
            db = 20.0 * np.log10(np.maximum(mag[:length], 1e-12))
            payload = np.clip(np.round(db * 100), -32768, 32767).astype(np.int16)
            return encode_frame(FRAME_TYPE_FFT, n, fs, payload)


def main():
    tone_hz = float(sys.argv[1]) if len(sys.argv) > 1 else 1000.0
    master_fd, slave_fd = pty.openpty()
    # Modo raw en el extremo slave: sin esto, la disciplina de línea del
    # pty hace eco de lo que se escribe en master_fd de vuelta hacia
    # master_fd (como si fuera una terminal), y las tramas binarias que
    # mandamos nosotros mismos "vuelven" pareciendo comandos entrantes.
    tty.setraw(slave_fd)
    slave_name = os.ttyname(slave_fd)

    print(f"[fake_board] puerto simulado: {slave_name}")
    print("[fake_board] pegalo en el combo 'Puerto' de la GUI y conectate")
    print(f"[fake_board] tono de prueba: {tone_hz:.1f} Hz")
    print("[fake_board] Ctrl+C para cortar\n")

    board = FakeBoard(tone_hz=tone_hz)
    next_send = time.monotonic()

    try:
        while True:
            timeout = max(0.0, next_send - time.monotonic())
            ready, _, _ = select.select([master_fd], [], [], timeout)

            if master_fd in ready:
                try:
                    data = os.read(master_fd, 256)
                except OSError:
                    break
                for b in data:
                    ch = chr(b)
                    if ch in COMMAND_CHARS:
                        board.handle_command(ch)

            now = time.monotonic()
            if now >= next_send:
                next_send = now + FRAME_PERIOD_S
                if board.running or board.single_shot:
                    board.single_shot = False
                    try:
                        write_all(master_fd, board.build_frame())
                    except OSError:
                        break
    except KeyboardInterrupt:
        pass
    finally:
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    main()
    master_fd, slave_fd = pty.openpty()
    # Modo raw en el extremo slave: sin esto, la disciplina de línea del
    # pty hace eco de lo que se escribe en master_fd de vuelta hacia
    # master_fd (como si fuera una terminal), y las tramas binarias que
    # mandamos nosotros mismos "vuelven" pareciendo comandos entrantes.
    tty.setraw(slave_fd)
    slave_name = os.ttyname(slave_fd)

    print(f"[fake_board] puerto simulado: {slave_name}")
    print("[fake_board] pegalo en el combo 'Puerto' de la GUI y conectate")
    print(f"[fake_board] tono de prueba: {tone_hz:.1f} Hz")
    print("[fake_board] Ctrl+C para cortar\n")

    board = FakeBoard(tone_hz=tone_hz)
    next_send = time.monotonic()

    try:
        while True:
            timeout = max(0.0, next_send - time.monotonic())
            ready, _, _ = select.select([master_fd], [], [], timeout)

            if master_fd in ready:
                try:
                    data = os.read(master_fd, 256)
                except OSError:
                    break
                for b in data:
                    ch = chr(b)
                    if ch in COMMAND_CHARS:
                        board.handle_command(ch)

            now = time.monotonic()
            if now >= next_send:
                next_send = now + FRAME_PERIOD_S
                if board.running or board.single_shot:
                    board.single_shot = False
                    try:
                        write_all(master_fd, board.build_frame())
                    except OSError:
                        break
    except KeyboardInterrupt:
        pass
    finally:
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    main()
