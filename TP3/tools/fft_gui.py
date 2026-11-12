#!/usr/bin/env python3
"""
GUI de control y visualización para el TP3 DSP (FRDM-MCXN947) - Espectro FFT.

Se conecta al puerto serie del proyecto MCXN947_Project_TP3 y decodifica las
tramas binarias descriptas en TP3/README.md ("Formato de Trama UART"):

    Sync(2B=A5 5A) Type(1B) Window(1B) N(2B) Length(2B) Fs(4B) Seq(4B)
    PeakBin(2B) Payload(int16 x Length) CRC(2B, CRC-16/CCITT sobre todo
    lo anterior a partir de Type, sin incluir el Sync)

Soporta dos modos de trama:
  - FFT (Type=0): magnitud en dBFS x100, Length = N/2. Se grafica en Hz
    (eje x = k*Fs/N) con el pico marcado (bin lo manda el propio micro).
  - Bypass (Type=1): muestras q15, Length = N. Se grafica en ms, y además
    se calcula localmente un FFT de referencia (misma ventana que reportó
    el micro) para poder comparar contra el modo FFT real de la placa.

Mientras el firmware no transmita en binario (todavía no hay trama real:
ver issues #14-#16 del repo), cualquier línea de texto ASCII que llegue por
UART (banners, prints de depuración) se loguea igual, intercalada con lo
que sí se decodifique como trama -- FrameParser.feed() es una máquina de
estados que reconoce ambas cosas en el mismo flujo de bytes.

Requisitos: pyserial, matplotlib, numpy (Tkinter suele venir con Python; en
Linux puede requerir el paquete del sistema "python3-tk").

Instalación (en Linux moderno, un "pip install" a secas suele fallar con
"externally-managed-environment" -- crear un venv primero, como en TP1/TP2):
    python3 -m venv venv
    venv/bin/pip install -r requirements.txt

Uso:
    venv/bin/python3 fft_gui.py
"""

import binascii
import dataclasses
import queue
import struct
import threading
import time
import tkinter as tk
from collections import deque
from pathlib import Path
from tkinter import messagebox, ttk

import numpy as np
import serial
import serial.tools.list_ports
from matplotlib import style as mpl_style
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

mpl_style.use("dark_background")

# --------------------------------------------------------------------------
# Protocolo de trama (ver TP3/README.md, seccion "Formato de Trama UART")
# --------------------------------------------------------------------------
SYNC = b"\xA5\x5A"
HEADER_FMT = "<BBHHIIH"  # Type,Window,N,Length,Fs,Seq,PeakBin (little-endian)
HEADER_SIZE = struct.calcsize(HEADER_FMT)
CRC_SIZE = 2
CRC_INIT = 0xFFFF  # CRC-16/CCITT-FALSE: binascii.crc_hqx(data, 0xFFFF)

FRAME_TYPE_FFT = 0
FRAME_TYPE_BYPASS = 1
TYPE_NAMES = {FRAME_TYPE_FFT: "FFT", FRAME_TYPE_BYPASS: "Bypass"}

WINDOW_RECT = 0
WINDOW_HANN = 1
WINDOW_NAMES = {WINDOW_RECT: "Rectangular", WINDOW_HANN: "Hann"}

VALID_N = (512, 1024, 2048)

BAUD_RATES = ("115200", "460800", "921600")
BAUD_RATE_DEFAULT = "115200"

LOG_MAX_LINES = 500
PLOT_REFRESH_MS = 50       # ~20 Hz de refresco de gráficos
QUEUE_POLL_MS = 20
RATE_WINDOW_S = 1.0        # ventana para calcular tramas/s
FFT_DB_MIN, FFT_DB_MAX = -100.0, 0.0
BYPASS_YLIM = (-33000, 33000)

# Comandos de un solo caracter (ver consigna de la issue #17 / a implementar
# en firmware junto con el resto del protocolo binario)
COMMANDS = {
    "FFT / Bypass (x)": "x",
    "Cambiar Fs (f)": "f",
    "Cambiar N (n)": "n",
    "Ventana (w)": "w",
    "Modo continuo (c)": "c",
    "Captura única (s)": "s",
    "Medir tiempos (t)": "t",
}

# --- Paleta dark mode (igual que TP1/tools/uart_gui.py) ---
DARK_BG = "#1e1e1e"
DARK_BG_ALT = "#2a2d2e"
DARK_FG = "#e6e6e6"
DARK_ACCENT = "#3a7ca5"

IMAGES_DIR = Path(__file__).resolve().parent.parent / "images"


def crc16_ccitt(data: bytes, init: int = CRC_INIT) -> int:
    """CRC-16/CCITT (poly 0x1021) sobre `data`, arrancando en `init`.

    binascii.crc_hqx ya implementa exactamente este polinomio; con
    init=0xFFFF es la variante "CCITT-FALSE", la más común para framing
    por UART. Si el firmware termina usando otro init/variante, ajustar
    CRC_INIT acá (un solo lugar).
    """
    return binascii.crc_hqx(data, init)


@dataclasses.dataclass
class Frame:
    """Una trama ya decodificada y con el CRC verificado."""

    type: int
    window: int
    n: int
    length: int
    fs: int
    seq: int
    peak_bin: int
    payload: np.ndarray  # int16, tamaño == length

    @property
    def is_fft(self) -> bool:
        return self.type == FRAME_TYPE_FFT

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, f"?{self.type}")

    @property
    def window_name(self) -> str:
        return WINDOW_NAMES.get(self.window, f"?{self.window}")

    def freqs_hz(self) -> np.ndarray:
        """Eje de frecuencia para una trama FFT: k*Fs/N, k=0..length-1."""
        k = np.arange(self.length)
        return k * self.fs / self.n

    def db(self) -> np.ndarray:
        """Magnitud en dBFS (el payload FFT viene como dB x100)."""
        return self.payload.astype(np.float64) / 100.0

    def time_ms(self) -> np.ndarray:
        """Eje temporal para una trama Bypass: k/Fs*1000, k=0..length-1."""
        k = np.arange(self.length)
        return k / self.fs * 1000.0


def local_reference_spectrum(frame: Frame):
    """FFT de referencia calculada en la PC sobre una trama Bypass (q15),
    con la misma ventana que reportó el micro -- sirve para validar que el
    FFT que corre en la placa da lo mismo (issue #17, "Validación").

    Normaliza a dBFS con ganancia coherente (divide por suma de la
    ventana/2) para que un tono a escala completa dé ~0 dB, igual
    convención que se espera del lado del firmware. Si el firmware termina
    usando otra normalización, esto es un punto de referencia a ajustar,
    no una verdad absoluta.
    """
    x = frame.payload.astype(np.float64) / 32768.0
    n = len(x)
    if n < 8:
        return None, None
    w = np.ones(n) if frame.window == WINDOW_RECT else np.hanning(n)
    spectrum = np.fft.rfft(x * w)
    mag = np.abs(spectrum) / max(np.sum(w) / 2.0, 1e-12)
    db = 20.0 * np.log10(np.maximum(mag, 1e-12))
    freqs = np.fft.rfftfreq(n, d=1.0 / frame.fs)
    return freqs, db


class FrameParser:
    """Máquina de estados para decodificar el flujo de bytes de la UART.

    feed(data) es un generador que devuelve, en orden, una tupla por cada
    evento reconocido:
      ('frame', Frame)     -- trama binaria válida (CRC OK)
      ('crc_error', nbytes)-- trama con sync+header plausible pero CRC
                               inválido; se descarta y se sigue buscando
                               sync (no se pierde el resto del stream)
      ('text', str)        -- línea ASCII fuera de una trama (prints de
                               depuración, banners, modo no binario)

    Tolera que los datos lleguen partidos en chunks arbitrarios entre
    llamadas a feed() (fragmentación típica de pyserial.read()).
    """

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf += data
        while True:
            sync_idx = self._buf.find(SYNC)

            if sync_idx == -1:
                # Sin sync en el buffer: todo lo que esté antes de la
                # última '\n' son líneas de texto completas; el resto
                # (sin '\n' todavía, o el principio de un sync partido
                # entre dos lecturas) se deja en el buffer para la
                # próxima llamada.
                yield from self._flush_text_lines()
                return

            if sync_idx > 0:
                yield from self._flush_text_upto(sync_idx)
                continue  # el sync ahora queda al principio del buffer

            # sync_idx == 0: hay un posible inicio de trama
            if len(self._buf) < len(SYNC) + HEADER_SIZE:
                return  # falta el header completo, esperar más datos

            header = self._buf[len(SYNC):len(SYNC) + HEADER_SIZE]
            type_, window, n, length, fs, seq, peak_bin = struct.unpack(HEADER_FMT, header)

            if not self._header_plausible(type_, window, n, length):
                # Coincidencia espuria de A5 5A dentro de datos/texto:
                # descartar solo el sync falso y seguir buscando.
                del self._buf[0:len(SYNC)]
                continue

            total = len(SYNC) + HEADER_SIZE + length * 2 + CRC_SIZE
            if len(self._buf) < total:
                return  # falta el payload+CRC completo, esperar más datos

            crc_covered = bytes(self._buf[len(SYNC):len(SYNC) + HEADER_SIZE + length * 2])
            payload_bytes = bytes(self._buf[len(SYNC) + HEADER_SIZE:len(SYNC) + HEADER_SIZE + length * 2])
            (crc_rx,) = struct.unpack_from("<H", self._buf, len(SYNC) + HEADER_SIZE + length * 2)
            crc_calc = crc16_ccitt(crc_covered)

            del self._buf[0:total]

            if crc_rx != crc_calc:
                yield ("crc_error", total)
                continue

            payload = np.frombuffer(payload_bytes, dtype="<i2")
            yield ("frame", Frame(type_, window, n, length, fs, seq, peak_bin, payload))

    @staticmethod
    def _header_plausible(type_, window, n, length) -> bool:
        if type_ not in TYPE_NAMES or window not in WINDOW_NAMES or n not in VALID_N:
            return False
        expected_length = (n // 2) if type_ == FRAME_TYPE_FFT else n
        return length == expected_length

    def _flush_text_lines(self):
        if b"\n" not in self._buf:
            return
        *lines, remainder = bytes(self._buf).split(b"\n")
        self._buf = bytearray(remainder)
        for raw in lines:
            text = raw.decode("ascii", errors="replace").rstrip("\r")
            if text:
                yield ("text", text)

    def _flush_text_upto(self, idx: int):
        segment = bytes(self._buf[:idx])
        del self._buf[0:idx]
        for raw in segment.split(b"\n"):
            text = raw.decode("ascii", errors="replace").rstrip("\r")
            if text:
                yield ("text", text)


class SerialWorker:
    """Lee el puerto serie en un hilo aparte y expone una cola de chunks
    de bytes crudos (el parseo de tramas se hace en el hilo principal)."""

    def __init__(self, chunk_queue: "queue.Queue[bytes]"):
        self._chunk_queue = chunk_queue
        self._ser: serial.Serial | None = None
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def connect(self, port: str, baud: int):
        self._ser = serial.Serial(port, baudrate=baud, timeout=0.2)
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def disconnect(self):
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        if self._ser is not None:
            try:
                self._ser.close()
            except serial.SerialException:
                pass
        self._ser = None

    def send_char(self, ch: str):
        if self.is_open:
            self._ser.write(ch.encode("ascii", errors="ignore"))

    def _read_loop(self):
        while not self._stop_event.is_set():
            try:
                chunk = self._ser.read(4096)
            except serial.SerialException:
                break
            if chunk:
                self._chunk_queue.put(chunk)


class FftGuiApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TP3 DSP - Espectro FFT (FRDM-MCXN947)")
        self.geometry("1100x820")

        self._chunk_queue: "queue.Queue[bytes]" = queue.Queue()
        self._worker = SerialWorker(self._chunk_queue)
        self._parser = FrameParser()

        self._last_fft_frame: Frame | None = None
        self._last_bypass_frame: Frame | None = None
        self._last_frame: Frame | None = None  # el más reciente de cualquier tipo (export)

        self._fft_dirty = False
        self._bypass_dirty = False
        self._fft_axis_key = None
        self._bypass_axis_key = None

        self._frame_times: deque[float] = deque(maxlen=500)
        self._crc_error_count = 0
        self._lost_frames = 0
        self._expected_seq: int | None = None

        self._apply_dark_theme()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(QUEUE_POLL_MS, self._poll_queue)
        self.after(PLOT_REFRESH_MS, self._refresh_plots)
        self.after(500, self._refresh_info_panel)

    # --------------------------------------------------------------- theme
    def _apply_dark_theme(self):
        self.configure(bg=DARK_BG)
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(".", background=DARK_BG, foreground=DARK_FG,
                         fieldbackground=DARK_BG_ALT, bordercolor=DARK_BG_ALT)
        style.configure("TFrame", background=DARK_BG)
        style.configure("TLabelframe", background=DARK_BG, foreground=DARK_FG)
        style.configure("TLabelframe.Label", background=DARK_BG, foreground=DARK_FG)
        style.configure("TLabel", background=DARK_BG, foreground=DARK_FG)
        style.configure("TButton", background=DARK_BG_ALT, foreground=DARK_FG)
        style.map("TButton", background=[("active", DARK_ACCENT)])
        style.configure("TCheckbutton", background=DARK_BG, foreground=DARK_FG)
        style.map("TCheckbutton", background=[("active", DARK_BG)])
        style.configure("TCombobox", fieldbackground=DARK_BG_ALT, background=DARK_BG_ALT,
                         foreground=DARK_FG, arrowcolor=DARK_FG)
        style.configure("TEntry", fieldbackground=DARK_BG_ALT, foreground=DARK_FG)
        style.configure("TNotebook", background=DARK_BG, bordercolor=DARK_BG)
        style.configure("TNotebook.Tab", background=DARK_BG_ALT, foreground=DARK_FG)
        style.map("TNotebook.Tab", background=[("selected", DARK_ACCENT)])
        self.option_add("*TCombobox*Listbox.background", DARK_BG_ALT)
        self.option_add("*TCombobox*Listbox.foreground", DARK_FG)
        self.option_add("*Text.background", DARK_BG_ALT)
        self.option_add("*Text.foreground", DARK_FG)

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        top = ttk.Frame(self, padding=8)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="Puerto:").pack(side=tk.LEFT)
        # Editable (no "readonly"): permite pegar a mano un puerto que no
        # aparece en el autodetectado de pyserial, como el pty que arma
        # TP3/tests/fake_board.py para probar la GUI sin placa.
        self._port_combo = ttk.Combobox(top, width=20)
        self._port_combo.pack(side=tk.LEFT, padx=4)
        self._refresh_ports()
        ttk.Button(top, text="Refrescar", command=self._refresh_ports).pack(side=tk.LEFT, padx=4)

        ttk.Label(top, text="Baud:").pack(side=tk.LEFT, padx=(12, 0))
        self._baud_combo = ttk.Combobox(top, width=10, values=BAUD_RATES)
        self._baud_combo.set(BAUD_RATE_DEFAULT)
        self._baud_combo.pack(side=tk.LEFT, padx=4)

        self._connect_btn = ttk.Button(top, text="Conectar", command=self._toggle_connection)
        self._connect_btn.pack(side=tk.LEFT, padx=12)

        self._status_var = tk.StringVar(value="Desconectado")
        ttk.Label(top, textvariable=self._status_var).pack(side=tk.LEFT, padx=8)

        ttk.Button(top, text="Exportar captura", command=self._export_capture).pack(side=tk.RIGHT, padx=4)

        # --- Comandos ---
        cmd_frame = ttk.LabelFrame(self, text="Comandos", padding=8)
        cmd_frame.pack(side=tk.TOP, fill=tk.X, padx=8, pady=4)
        for label, ch in COMMANDS.items():
            ttk.Button(cmd_frame, text=label, command=lambda c=ch: self._send(c)).pack(
                side=tk.LEFT, padx=4, pady=4
            )

        # --- Panel de información ---
        self._build_info_panel()

        # --- Notebook: gráficos + log ---
        notebook = ttk.Notebook(self)
        notebook.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=8, pady=4)
        self._notebook = notebook
        self._build_fft_tab(notebook)
        self._build_bypass_tab(notebook)
        self._build_log_tab(notebook)

    def _build_info_panel(self):
        frame = ttk.LabelFrame(self, text="Estado", padding=8)
        frame.pack(side=tk.TOP, fill=tk.X, padx=8, pady=4)

        self._info_vars = {
            "fs": tk.StringVar(value="Fs: --"),
            "n": tk.StringVar(value="N: --"),
            "df": tk.StringVar(value="Δf: --"),
            "tw": tk.StringVar(value="T_ventana: --"),
            "window": tk.StringVar(value="Ventana: --"),
            "peak": tk.StringVar(value="Pico: --"),
            "rate": tk.StringVar(value="Tramas/s: 0"),
            "crc": tk.StringVar(value="CRC inválido: 0"),
            "lost": tk.StringVar(value="Seq perdidos: 0"),
        }
        order = ["fs", "n", "df", "tw", "window", "peak", "rate", "crc", "lost"]
        for i, key in enumerate(order):
            row, col = divmod(i, 3)
            ttk.Label(frame, textvariable=self._info_vars[key], relief=tk.SUNKEN, padding=4).grid(
                row=row, column=col, padx=4, pady=2, sticky="we"
            )
        for col in range(3):
            frame.columnconfigure(col, weight=1)

    def _build_fft_tab(self, notebook):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text="Espectro (FFT)")
        self._fft_fig = Figure(figsize=(5, 3), dpi=100)
        self._fft_ax = self._fft_fig.add_subplot(111)
        self._fft_ax.set_title("Espectro FFT (dBFS)")
        self._fft_ax.set_xlabel("Frecuencia (Hz)")
        self._fft_ax.set_ylabel("Magnitud (dBFS)")
        self._fft_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
        (self._fft_line,) = self._fft_ax.plot([], [], linewidth=0.9, color="#7ec8e3")
        self._fft_peak_marker = self._fft_ax.axvline(0, color=DARK_ACCENT, linestyle="--", linewidth=1.0, visible=False)
        self._fft_peak_text = self._fft_ax.annotate(
            "", xy=(0, 0), xytext=(6, -10), textcoords="offset points",
            color=DARK_ACCENT, fontsize=9, visible=False,
        )
        self._fft_fig.tight_layout()
        self._fft_canvas = FigureCanvasTkAgg(self._fft_fig, master=tab)
        self._fft_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_bypass_tab(self, notebook):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text="Bypass + Validación")
        self._bypass_fig = Figure(figsize=(5, 4.5), dpi=100)
        self._bypass_ax = self._bypass_fig.add_subplot(211)
        self._bypass_ax.set_title("Señal temporal (Bypass)")
        self._bypass_ax.set_xlabel("Tiempo (ms)")
        self._bypass_ax.set_ylabel("Cuentas q15")
        self._bypass_ax.set_ylim(*BYPASS_YLIM)
        (self._bypass_line,) = self._bypass_ax.plot([], [], linewidth=0.8, color="#7ec8e3")

        self._ref_ax = self._bypass_fig.add_subplot(212)
        self._ref_ax.set_title("FFT de referencia (PC, misma ventana)")
        self._ref_ax.set_xlabel("Frecuencia (Hz)")
        self._ref_ax.set_ylabel("Magnitud (dBFS)")
        self._ref_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
        (self._ref_line,) = self._ref_ax.plot([], [], linewidth=0.9, color="#e8a33d")

        self._bypass_fig.tight_layout()
        self._bypass_canvas = FigureCanvasTkAgg(self._bypass_fig, master=tab)
        self._bypass_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_log_tab(self, notebook):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text="Log de consola")
        self._log_text = tk.Text(
            tab, wrap=tk.NONE, state=tk.DISABLED,
            bg=DARK_BG_ALT, fg=DARK_FG, insertbackground=DARK_FG,
            selectbackground=DARK_ACCENT, borderwidth=0, highlightthickness=0,
        )
        self._log_text.pack(fill=tk.BOTH, expand=True)

    # -------------------------------------------------------------- serial
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self._port_combo["values"] = ports
        if ports and not self._port_combo.get():
            self._port_combo.set(ports[0])

    def _toggle_connection(self):
        if self._worker.is_open:
            self._worker.disconnect()
            self._connect_btn.config(text="Conectar")
            self._status_var.set("Desconectado")
            return

        port = self._port_combo.get()
        if not port:
            messagebox.showerror("Error", "Seleccioná un puerto serie.")
            return
        try:
            baud = int(self._baud_combo.get())
        except ValueError:
            messagebox.showerror("Error", "Baud rate inválido.")
            return

        try:
            self._worker.connect(port, baud)
        except serial.SerialException as exc:
            messagebox.showerror("Error al conectar", str(exc))
            return

        self._parser = FrameParser()
        self._frame_times.clear()
        self._crc_error_count = 0
        self._lost_frames = 0
        self._expected_seq = None

        self._connect_btn.config(text="Desconectar")
        self._status_var.set(f"Conectado a {port} @ {baud}")

    def _send(self, ch: str):
        if not self._worker.is_open:
            messagebox.showwarning("Sin conexión", "Conectate a un puerto serie primero.")
            return
        self._worker.send_char(ch)

    def _on_close(self):
        self._worker.disconnect()
        self.destroy()

    # --------------------------------------------------------------- data
    def _poll_queue(self):
        try:
            while True:
                chunk = self._chunk_queue.get_nowait()
                for kind, payload in self._parser.feed(chunk):
                    self._handle_event(kind, payload)
        except queue.Empty:
            pass
        self.after(QUEUE_POLL_MS, self._poll_queue)

    def _handle_event(self, kind: str, payload):
        if kind == "text":
            self._log(payload)
        elif kind == "crc_error":
            self._crc_error_count += 1
        elif kind == "frame":
            self._handle_frame(payload)

    def _handle_frame(self, frame: Frame):
        self._frame_times.append(time.monotonic())
        if self._expected_seq is not None and frame.seq != self._expected_seq:
            if frame.seq > self._expected_seq:
                self._lost_frames += frame.seq - self._expected_seq
        self._expected_seq = frame.seq + 1

        self._last_frame = frame
        if frame.is_fft:
            self._last_fft_frame = frame
            self._fft_dirty = True
        else:
            self._last_bypass_frame = frame
            self._bypass_dirty = True

    def _log(self, line: str):
        self._log_text.config(state=tk.NORMAL)
        self._log_text.insert(tk.END, line + "\n")
        num_lines = int(self._log_text.index("end-1c").split(".")[0])
        if num_lines > LOG_MAX_LINES:
            self._log_text.delete("1.0", f"{num_lines - LOG_MAX_LINES}.0")
        self._log_text.see(tk.END)
        self._log_text.config(state=tk.DISABLED)

    # -------------------------------------------------------------- plots
    def _refresh_plots(self):
        if self._fft_dirty:
            self._fft_dirty = False
            self._redraw_fft()
        if self._bypass_dirty:
            self._bypass_dirty = False
            self._redraw_bypass()
        self.after(PLOT_REFRESH_MS, self._refresh_plots)

    def _redraw_fft(self):
        frame = self._last_fft_frame
        if frame is None:
            return
        freqs, db = frame.freqs_hz(), frame.db()
        self._fft_line.set_data(freqs, db)

        key = (frame.fs, frame.n)
        if key != self._fft_axis_key:
            self._fft_axis_key = key
            self._fft_ax.set_xlim(0, frame.fs / 2.0)
            self._fft_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)

        if 0 <= frame.peak_bin < frame.length:
            peak_freq = freqs[frame.peak_bin]
            peak_db = db[frame.peak_bin]
            self._fft_peak_marker.set_xdata([peak_freq, peak_freq])
            self._fft_peak_marker.set_visible(True)
            self._fft_peak_text.xy = (peak_freq, peak_db)
            self._fft_peak_text.set_text(f"{peak_freq:.1f} Hz\n{peak_db:.1f} dBFS")
            self._fft_peak_text.set_visible(True)
        else:
            self._fft_peak_marker.set_visible(False)
            self._fft_peak_text.set_visible(False)

        self._fft_canvas.draw_idle()

    def _redraw_bypass(self):
        frame = self._last_bypass_frame
        if frame is None:
            return
        t_ms = frame.time_ms()
        self._bypass_line.set_data(t_ms, frame.payload)

        freqs, db = local_reference_spectrum(frame)

        key = (frame.fs, frame.n)
        if key != self._bypass_axis_key:
            self._bypass_axis_key = key
            self._bypass_ax.set_xlim(0, t_ms[-1] if len(t_ms) > 1 else 1)
            self._bypass_ax.set_ylim(*BYPASS_YLIM)
            self._ref_ax.set_xlim(0, frame.fs / 2.0)
            self._ref_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)

        if freqs is not None:
            self._ref_line.set_data(freqs, db)

        self._bypass_canvas.draw_idle()

    def _refresh_info_panel(self):
        frame = self._last_frame
        if frame is not None:
            df = frame.fs / frame.n
            tw = frame.n / frame.fs
            self._info_vars["fs"].set(f"Fs: {frame.fs} Hz")
            self._info_vars["n"].set(f"N: {frame.n}")
            self._info_vars["df"].set(f"Δf: {df:.2f} Hz")
            self._info_vars["tw"].set(f"T_ventana: {tw * 1000:.2f} ms")
            self._info_vars["window"].set(f"Ventana: {frame.window_name}")

            if frame.is_fft and 0 <= frame.peak_bin < frame.length:
                peak_freq = frame.peak_bin * frame.fs / frame.n
                peak_db = frame.db()[frame.peak_bin]
                self._info_vars["peak"].set(f"Pico: {peak_freq:.1f} Hz / {peak_db:.1f} dBFS")
            else:
                self._info_vars["peak"].set("Pico: --")

        now = time.monotonic()
        while self._frame_times and now - self._frame_times[0] > RATE_WINDOW_S:
            self._frame_times.popleft()
        self._info_vars["rate"].set(f"Tramas/s: {len(self._frame_times)}")
        self._info_vars["crc"].set(f"CRC inválido: {self._crc_error_count}")
        self._info_vars["lost"].set(f"Seq perdidos: {self._lost_frames}")

        self.after(500, self._refresh_info_panel)

    # ------------------------------------------------------------- export
    def _export_capture(self):
        frame = self._last_frame
        if frame is None:
            messagebox.showwarning("Sin datos", "Todavía no se recibió ninguna trama.")
            return

        IMAGES_DIR.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        window_tag = frame.window_name.lower()
        base = f"capture_{frame.type_name.lower()}_fs{frame.fs}_n{frame.n}_{window_tag}_{stamp}"
        csv_path = IMAGES_DIR / f"{base}.csv"
        png_path = IMAGES_DIR / f"{base}.png"

        if frame.is_fft:
            freqs, db = frame.freqs_hz(), frame.db()
            header = "frecuencia_hz,db"
            rows = np.column_stack([freqs, db])
            fig = self._fft_fig
        else:
            header = "tiempo_ms,q15"
            rows = np.column_stack([frame.time_ms(), frame.payload.astype(np.float64)])
            fig = self._bypass_fig

        np.savetxt(csv_path, rows, delimiter=",", header=header, comments="", fmt="%.6f")
        fig.savefig(png_path, dpi=150)

        self._status_var.set(f"Exportado: {csv_path.name} / {png_path.name}")
        self._log(f"[EXPORT] {csv_path}")
        self._log(f"[EXPORT] {png_path}")


if __name__ == "__main__":
    app = FftGuiApp()
    app.mainloop()
