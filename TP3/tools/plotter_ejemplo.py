#!/usr/bin/env python3
"""
EJEMPLO (rama tp3-ejemplo-senal-interna, no se integra al proyecto).

Plotter del ejemplo de señal interna: una sola ventana que se conecta a la placa, manda los
comandos y grafica. No hace falta abrir una terminal serie aparte.

  - Arriba: la ventana de entrada en el tiempo (en BYPASS) o el espectro en Hz (en FFT).
  - El título dice en qué frecuencia está el pico.

Teclas (con la ventana del gráfico seleccionada; se mandan tal cual a la placa):
  g  fuente: ADC -> DAC LOOPBACK -> SINTETICA      w  tono: 250/1000/1004/3000/5000 Hz
  m  modo: BYPASS -> FFT 512 -> 1024 -> 2048 (SW3)  f  Fs: 8k/16k/22k/44k/48k (SW2)

Uso (con el programa corriendo en la placa y sin otra terminal abierta en el puerto):
    python3 plotter_ejemplo.py                  # /dev/ttyACM0
    python3 plotter_ejemplo.py --port /dev/ttyACM1
"""

import argparse
import re
import time

import matplotlib.pyplot as plt
import numpy as np
import serial

from ver_frecuencia import TYPE_FFT, parse_frames, peak, spectrum_db

KEYS = "gwmf"
NUMERIC_LINE_RE = re.compile(rb"^-?\d+,-?\d+(,-?\d+)?$")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    # Sacarle a matplotlib sus atajos de teclado (g = grilla, f = pantalla completa, etc.)
    for k in list(plt.rcParams):
        if k.startswith("keymap."):
            plt.rcParams[k] = []

    # exclusive: si otro programa (una terminal serie) tiene el puerto abierto, falla acá en
    # vez de que los dos se repartan los bytes y ninguno reciba lo que espera
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0, exclusive=True)
    except serial.SerialException as e:
        raise SystemExit(f"No se pudo abrir {args.port}: {e}\n"
                         "¿Hay una terminal serie (picocom, Serial Monitor de VS Code, MCUXpresso) "
                         "u otro plotter abierto en ese puerto? Cerralo y probá de nuevo.")
    ser.reset_input_buffer()

    fig, ax = plt.subplots(figsize=(11, 5.5))
    (line,) = ax.plot([], [], lw=1)
    (dot,) = ax.plot([], [], "o", color="tab:red")
    status = fig.text(0.01, 0.01, "Teclas: g fuente · w tono · m modo · f Fs", fontsize=9, family="monospace")
    ax.grid(True, alpha=0.3)
    fig.canvas.manager.set_window_title("Plotter ejemplo TP3")

    def on_key(event):
        if event.key and event.key.lower() in KEYS:
            ser.write(event.key.lower().encode())

    fig.canvas.mpl_connect("key_press_event", on_key)

    buf, text = bytearray(), bytearray()
    last_msg = "Esperando la primera ventana..."
    last_request = 0.0
    numeric_lines, last_v = 0, 0.0
    t_start, got_anything, warned = time.time(), False, False
    shown_status = None
    ser.write(b"h")  # para ver en consola que la placa responde

    plt.ion()
    plt.show()
    while plt.fignum_exists(fig.number):
        chunk = ser.read(8192)
        if chunk:
            got_anything = True
        buf += chunk
        frames = parse_frames(buf, text)
        if not got_anything and not warned and time.time() - t_start > 5.0:
            warned = True
            last_msg = "La placa no responde: ¿el programa está corriendo? (en Debug: Resume / F8)"
            print(last_msg)

        # Texto de la placa: guardar la última línea [SIG]/[DSP]/[INFO] para mostrarla
        while b"\n" in text:
            raw, _, rest = text.partition(b"\n")
            text[:] = rest
            raw = raw.strip()
            if NUMERIC_LINE_RE.match(raw):
                numeric_lines += 1  # streaming de texto ('p' o 'v') prendido: lo apagamos
                continue
            if raw:
                msg = raw.decode(errors="replace")
                print(msg)
                if msg.startswith("["):
                    last_msg = msg
        if numeric_lines > 50 and time.time() - last_v > 2.0:
            ser.write(b"v")  # 'v' apaga 'v', y si estaba 'p' lo cambia por 'v' (otro 'v' lo apaga)
            numeric_lines, last_v = 0, time.time()

        if frames:
            type_, n, fs, payload = frames[-1]
            db = spectrum_db(type_, n, payload)
            k, f_bin, f_est, level = peak(db, n, fs)
            if type_ == TYPE_FFT:
                x = np.arange(len(db)) * fs / n
                line.set_data(x, db)
                dot.set_data([f_bin], [level])
                ax.set_xlim(0, fs / 2)
                ax.set_ylim(-110, 5)
                ax.set_xlabel("Frecuencia [Hz]")
                ax.set_ylabel("dBFS")
                mode = "FFT"
            else:
                x = np.arange(n) / fs * 1000.0
                line.set_data(x, payload / 32768.0)
                dot.set_data([], [])
                ax.set_xlim(0, x[-1])
                ax.set_ylim(-1.05, 1.05)
                ax.set_xlabel("Tiempo [ms]")
                ax.set_ylabel("Amplitud (q15 / 32768)")
                mode = "BYPASS"
            ax.set_title(f"{mode}  N={n}  Fs={fs} Hz  Δf={fs / n:.2f} Hz   →   pico en {f_bin:.1f} Hz "
                         f"(bin {k}, estimada {f_est:.1f} Hz, {level:.1f} dBFS)")
            last_request = 0.0  # pedir la siguiente ya

        # Solo se toca el texto si cambió: cada cambio obliga a redibujar toda la figura
        new_status = f"Teclas: g fuente · w tono · m modo · f Fs        Último mensaje: {last_msg}"
        if new_status != shown_status:
            status.set_text(new_status)
            shown_status = new_status

        # Pedir una ventana ('s') cuando llegó la anterior, o cada 3 s si no llega nada
        if time.time() - last_request > (0.3 if last_request == 0.0 else 3.0):
            ser.write(b"s")
            last_request = time.time()

        plt.pause(0.05)

    ser.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
