#!/usr/bin/env python3
"""
EJEMPLO (rama tp3-ejemplo-senal-interna, no se integra al proyecto).

Pide tramas a la placa por UART y muestra en qué frecuencia está el pico del espectro.

Usa la trama binaria de uart_stage.c:
    Sync(A5 5A) Type(u8) N(u16) Fs(u32) Payload(int16 x L), little-endian
    Type 0 = FFT    -> L = N/2 bins en dBFS x 100
    Type 1 = BYPASS -> L = N muestras q15 (la FFT se calcula acá con numpy)

Uso (cerrar antes cualquier otra terminal serie abierta en el mismo puerto):
    python3 ver_frecuencia.py                     # /dev/ttyACM0, una medición por segundo
    python3 ver_frecuencia.py --cmd g             # antes de medir, manda 'g' (fuente siguiente)
    python3 ver_frecuencia.py --port /dev/ttyACM1 --cmd gw
"""

import argparse
import struct
import sys
import time

import numpy as np

SYNC = b"\xA5\x5A"
HEADER_FMT = "<BHI"  # Type, N, Fs
HEADER_SIZE = struct.calcsize(HEADER_FMT)
TYPE_FFT, TYPE_BYPASS = 0, 1
VALID_N = (512, 1024, 2048)


def parse_frames(buf: bytearray, text: bytearray = None):
    """Saca de buf las tramas completas que encuentre. Deja en buf lo que falta llegar.
    Si se pasa text, ahí se agregan los bytes de texto que no son parte de una trama."""
    frames = []
    while True:
        i = buf.find(SYNC)
        if i < 0:
            keep = 1 if buf.endswith(SYNC[:1]) else 0  # puede quedar medio sync al final
            if text is not None:
                text += buf[:len(buf) - keep]
            del buf[:len(buf) - keep]
            return frames
        if text is not None:
            text += buf[:i]
        del buf[:i]
        if len(buf) < 2 + HEADER_SIZE:
            return frames
        type_, n, fs = struct.unpack_from(HEADER_FMT, buf, 2)
        if type_ not in (TYPE_FFT, TYPE_BYPASS) or n not in VALID_N or not 1000 <= fs <= 100000:
            del buf[:2]  # sync falso (por ejemplo, dentro del payload): seguir buscando
            continue
        length = n // 2 if type_ == TYPE_FFT else n
        total = 2 + HEADER_SIZE + 2 * length
        if len(buf) < total:
            return frames
        payload = np.frombuffer(bytes(buf[2 + HEADER_SIZE:total]), dtype="<i2").astype(float)
        del buf[:total]
        frames.append((type_, n, fs, payload))


def spectrum_db(type_, n, payload):
    """Espectro en dBFS, bins 0 .. N/2 - 1."""
    if type_ == TYPE_FFT:
        return payload / 100.0
    x = payload / 32768.0
    x = x - x.mean()
    mag = np.abs(np.fft.rfft(x))[: n // 2] / (n / 2)  # 0 dBFS = seno de amplitud 1
    return 20 * np.log10(np.maximum(mag, 1e-9))


def peak(db, n, fs):
    """Bin del pico (sin contar DC) y frecuencia, con interpolación parabólica entre bins."""
    k = int(np.argmax(db[1:])) + 1
    f_bin = k * fs / n
    delta = 0.0
    if 1 <= k < len(db) - 1:
        a, b, c = db[k - 1], db[k], db[k + 1]
        den = a - 2 * b + c
        if den != 0:
            delta = 0.5 * (a - c) / den
    return k, f_bin, (k + delta) * fs / n, db[k]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmd", default="", help="caracteres a mandar antes de medir (ej. 'g', 'w', 'm')")
    ap.add_argument("--periodo", type=float, default=1.0, help="segundos entre mediciones")
    args = ap.parse_args()

    import serial  # pyserial

    with serial.Serial(args.port, args.baud, timeout=0.1, exclusive=True) as ser:
        for ch in args.cmd:
            ser.write(ch.encode())
            time.sleep(0.05)
        time.sleep(0.3)
        ser.reset_input_buffer()  # descarta los mensajes de texto de los comandos

        buf = bytearray()
        print("Ctrl+C para salir")
        while True:
            ser.write(b"s")  # una trama, con la próxima ventana
            t0 = time.time()
            frames = []
            while not frames and time.time() - t0 < 3.0:
                buf += ser.read(4096)
                frames = parse_frames(buf)
            if not frames:
                print("sin respuesta (¿puerto correcto? ¿hay otra terminal abierta?)")
                continue
            type_, n, fs, payload = frames[-1]
            db = spectrum_db(type_, n, payload)
            k, f_bin, f_est, level = peak(db, n, fs)
            mode = "FFT" if type_ == TYPE_FFT else "BYPASS"
            print(f"{mode:6s} N={n:4d} Fs={fs:5d} Hz  Δf={fs / n:6.2f} Hz  "
                  f"pico: bin {k:4d} = {f_bin:8.2f} Hz  (estimada {f_est:8.2f} Hz)  {level:6.1f} dBFS")
            time.sleep(args.periodo)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
