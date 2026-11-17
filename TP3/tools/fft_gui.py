#!/usr/bin/env python3
"""
GUI de control y visualización para el TP3 DSP (FRDM-MCXN947) - Espectro FFT.

Se conecta al puerto serie del proyecto MCXN947_Project_TP3 y decodifica las
tramas binarias descriptas en TP3/decisiones.md ("Formato de la trama"):

    Sync(2B=A5 5A) Type(1B) N(2B) Fs(4B) Payload(int16 x L)

Soporta dos modos de trama:
  - FFT (Type=0): magnitud en dBFS x100, L = N/2 bins (de 0 a N/2 - 1).
    Se grafica en Hz (eje x = k*Fs/N) con el pico calculado por la GUI.
  - Bypass (Type=1): muestras q15, L = N. Se grafica en ms, y además
    se calcula localmente un FFT de referencia (ventana rectangular) para
    poder comparar contra el modo FFT real de la placa.

Cualquier línea de texto ASCII que llegue por UART (banners, respuestas a
comandos) se loguea en la consola, intercalada con lo que sí se decodifique como
trama -- FrameParser.feed() es una máquina de estados que reconoce ambas cosas
en el mismo flujo de bytes.

Requisitos: pyserial, matplotlib, numpy (Tkinter suele venir con Python; en
Linux puede requerir el paquete del sistema "python3-tk").

Instalación (en Linux moderno, crear un venv primero):
    python3 -m venv venv
    venv/bin/pip install -r requirements.txt

Uso:
    venv/bin/python3 fft_gui.py
"""

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
# Protocolo de trama (ver TP3/decisiones.md, seccion "Formato de la trama")
# --------------------------------------------------------------------------
SYNC = b"\xA5\x5A"
HEADER_FMT = "<BHI"  # Type(1B), N(2B), Fs(4B) (little-endian)
HEADER_SIZE = struct.calcsize(HEADER_FMT)

FRAME_TYPE_FFT = 0
FRAME_TYPE_BYPASS = 1
TYPE_NAMES = {FRAME_TYPE_FFT: "FFT", FRAME_TYPE_BYPASS: "Bypass"}

VALID_N = (512, 1024, 2048)
VALID_FS = (8000, 16000, 22000, 44000, 48000)

WINDOW_RECT = 0
WINDOW_NAMES = {WINDOW_RECT: "Rectangular"}

BAUD_RATES = ("115200", "460800", "921600")
BAUD_RATE_DEFAULT = "115200"

LOG_MAX_LINES = 500
PLOT_REFRESH_MS = 50       # ~20 Hz de refresco de gráficos
QUEUE_POLL_MS = 20
RATE_WINDOW_S = 1.0        # ventana para calcular tramas/s
FFT_DB_MIN, FFT_DB_MAX = -100.0, 0.0
BYPASS_YLIM = (-33000, 33000)

# Comandos de un solo caracter según el firmware (m, f, r, c, s, t)
COMMANDS = {
    "Modo / N (m)": "m",
    "Cambiar Fs (f)": "f",
    "RUN / STOP (r)": "r",
    "Modo continuo (c)": "c",
    "Captura única (s)": "s",
    "Estado / Contadores (t)": "t",
}

# --- Paleta dark mode refinada ---
DARK_BG = "#18191c"           # Fondo principal
DARK_PANEL = "#22242a"        # Fondo de paneles y tarjetas
DARK_PANEL_BORDER = "#31353f" # Borde sutil
DARK_FG = "#f3f4f6"           # Texto primario
DARK_MUTED = "#9ca3af"        # Texto secundario
DARK_ACCENT = "#38bdf8"       # Cian moderno (accent)
DARK_ACCENT_HOVER = "#0284c7"
COLOR_CONNECTED = "#22c55e"   # Verde esmeralda
COLOR_DISCONNECTED = "#ef4444"# Rojo
COLOR_MAX_HOLD = "#f43f5e"    # Rosa/magenta para traza Max-Hold
COLOR_REF_FFT = "#f59e0b"     # Ámbar/naranja para FFT de referencia

IMAGES_DIR = Path(__file__).resolve().parent.parent / "images"


@dataclasses.dataclass
class Frame:
    """Una trama ya decodificada según TP3/decisiones.md."""

    type: int
    n: int
    fs: int
    payload: np.ndarray  # int16, tamaño == (n // 2) si FFT else n
    _peak_bin: int | None = dataclasses.field(default=None, init=False, repr=False)

    @property
    def is_fft(self) -> bool:
        return self.type == FRAME_TYPE_FFT

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, f"?{self.type}")

    @property
    def length(self) -> int:
        return len(self.payload)

    @property
    def window(self) -> int:
        return WINDOW_RECT

    @property
    def window_name(self) -> str:
        return WINDOW_NAMES.get(self.window, "Rectangular")

    @property
    def peak_bin(self) -> int:
        """Calcula el bin del pico en la PC (omite continua / bin 0)."""
        if self._peak_bin is None:
            if self.is_fft and len(self.payload) > 1:
                self._peak_bin = 1 + int(np.argmax(self.payload[1:]))
            else:
                self._peak_bin = 0
        return self._peak_bin

    @property
    def peak_freq_hz(self) -> float:
        """Frecuencia del pico en Hz calculada en la PC."""
        return self.peak_bin * self.fs / self.n

    @property
    def peak_db(self) -> float:
        """Magnitud del pico en dBFS calculada en la PC."""
        if 0 <= self.peak_bin < self.length:
            return float(self.payload[self.peak_bin]) / 100.0
        return -100.0

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
    """FFT de referencia calculada en la PC sobre una trama Bypass (q15).
    Usa ventana rectangular según TP3/decisiones.md.
    """
    x = frame.payload.astype(np.float64) / 32768.0
    n = len(x)
    if n < 8:
        return None, None
    w = np.ones(n)
    spectrum = np.fft.rfft(x * w)
    mag = np.abs(spectrum) / max(np.sum(w) / 2.0, 1e-12)
    db = 20.0 * np.log10(np.maximum(mag, 1e-12))
    freqs = np.fft.rfftfreq(n, d=1.0 / frame.fs)
    return freqs, db


class FrameParser:
    """Máquina de estados para decodificar el flujo de bytes de la UART.

    feed(data) es un generador que devuelve, en orden, una tupla por cada
    evento reconocido:
      ('frame', Frame)     -- trama binaria válida
      ('text', str)        -- línea ASCII fuera de una trama (prints de
                               depuración, respuestas a comandos, avisos)

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
            type_, n, fs = struct.unpack(HEADER_FMT, header)

            if not self._header_plausible(type_, n, fs):
                # Coincidencia espuria de A5 5A dentro de datos/texto:
                # descartar solo el sync falso y seguir buscando.
                del self._buf[0:len(SYNC)]
                continue

            length = (n // 2) if type_ == FRAME_TYPE_FFT else n
            total = len(SYNC) + HEADER_SIZE + length * 2
            if len(self._buf) < total:
                return  # falta el payload completo, esperar más datos

            payload_bytes = bytes(self._buf[len(SYNC) + HEADER_SIZE:total])
            del self._buf[0:total]

            payload = np.frombuffer(payload_bytes, dtype="<i2")
            yield ("frame", Frame(type=type_, n=n, fs=fs, payload=payload))

    @staticmethod
    def _header_plausible(type_: int, n: int, fs: int) -> bool:
        return (type_ in TYPE_NAMES) and (n in VALID_N) and (fs in VALID_FS)

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
        self.send_text(ch)

    def send_text(self, text: str):
        if self.is_open:
            self._ser.write(text.encode("ascii", errors="ignore"))

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
        self.geometry("1160x860")
        self.minsize(960, 700)

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
        self._total_frames = 0

        # Controles interactivos y de análisis
        self._paused = False
        self._max_hold_enabled = tk.BooleanVar(value=False)
        self._max_hold_db: np.ndarray | None = None
        self._cursor_info_var = tk.StringVar(value="")
        self._autoscroll_var = tk.BooleanVar(value=True)

        self._apply_dark_theme()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<Key>", self._on_key_press)
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
                         fieldbackground=DARK_PANEL, bordercolor=DARK_PANEL_BORDER)
        style.configure("TFrame", background=DARK_BG)
        style.configure("TLabelframe", background=DARK_BG, foreground=DARK_FG, bordercolor=DARK_PANEL_BORDER)
        style.configure("TLabelframe.Label", background=DARK_BG, foreground=DARK_ACCENT, font=("TkDefaultFont", 9, "bold"))
        style.configure("TLabel", background=DARK_BG, foreground=DARK_FG)
        style.configure("TButton", background=DARK_PANEL, foreground=DARK_FG, bordercolor=DARK_PANEL_BORDER, padding=5)
        style.map("TButton", background=[("active", DARK_ACCENT), ("pressed", DARK_ACCENT_HOVER)],
                             foreground=[("active", "#ffffff"), ("pressed", "#ffffff")])
        style.configure("TCheckbutton", background=DARK_BG, foreground=DARK_FG)
        style.map("TCheckbutton", background=[("active", DARK_BG)])
        style.configure("TCombobox", fieldbackground=DARK_PANEL, background=DARK_PANEL,
                         foreground=DARK_FG, arrowcolor=DARK_ACCENT, bordercolor=DARK_PANEL_BORDER)
        style.configure("TEntry", fieldbackground=DARK_PANEL, foreground=DARK_FG)
        style.configure("TNotebook", background=DARK_BG, bordercolor=DARK_PANEL_BORDER)
        style.configure("TNotebook.Tab", background=DARK_PANEL, foreground=DARK_MUTED, padding=[12, 4])
        style.map("TNotebook.Tab", background=[("selected", DARK_ACCENT)], foreground=[("selected", "#ffffff")])
        self.option_add("*TCombobox*Listbox.background", DARK_PANEL)
        self.option_add("*TCombobox*Listbox.foreground", DARK_FG)
        self.option_add("*TCombobox*Listbox.selectBackground", DARK_ACCENT)
        self.option_add("*Text.background", DARK_PANEL)
        self.option_add("*Text.foreground", DARK_FG)

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        # --- Barra superior: Conexión y Estado ---
        top = ttk.Frame(self, padding=(10, 8, 10, 4))
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="Puerto:", font=("TkDefaultFont", 9, "bold")).pack(side=tk.LEFT)
        self._port_combo = ttk.Combobox(top, width=18)
        self._port_combo.pack(side=tk.LEFT, padx=(4, 6))
        self._refresh_ports()
        ttk.Button(top, text="🔄 Refrescar", command=self._refresh_ports).pack(side=tk.LEFT, padx=(0, 10))

        ttk.Label(top, text="Baud:", font=("TkDefaultFont", 9, "bold")).pack(side=tk.LEFT)
        self._baud_combo = ttk.Combobox(top, width=9, values=BAUD_RATES)
        self._baud_combo.set(BAUD_RATE_DEFAULT)
        self._baud_combo.pack(side=tk.LEFT, padx=(4, 10))

        self._connect_btn = ttk.Button(top, text="⚡ Conectar", command=self._toggle_connection)
        self._connect_btn.pack(side=tk.LEFT, padx=(0, 12))

        # Indicador LED
        self._led_canvas = tk.Canvas(top, width=16, height=16, bg=DARK_BG, highlightthickness=0)
        self._led_canvas.pack(side=tk.LEFT, padx=(0, 6))
        self._led = self._led_canvas.create_oval(2, 2, 14, 14, fill=COLOR_DISCONNECTED, outline="#111111")

        self._status_var = tk.StringVar(value="Desconectado")
        self._status_label = ttk.Label(top, textvariable=self._status_var, font=("TkDefaultFont", 9, "bold"))
        self._status_label.pack(side=tk.LEFT)

        ttk.Button(top, text="💾 Exportar captura", command=self._export_capture).pack(side=tk.RIGHT, padx=4)

        # --- Barra de Comandos y Herramientas ---
        ctrl_bar = ttk.Frame(self, padding=(10, 2, 10, 4))
        ctrl_bar.pack(side=tk.TOP, fill=tk.X)

        cmd_frame = ttk.LabelFrame(ctrl_bar, text="Comandos del Firmware (teclas rápidas)", padding=6)
        cmd_frame.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))
        for label, ch in COMMANDS.items():
            btn = ttk.Button(cmd_frame, text=label, command=lambda c=ch: self._send(c))
            btn.pack(side=tk.LEFT, padx=3, pady=2)

        graph_ctrl = ttk.LabelFrame(ctrl_bar, text="Control del Gráfico", padding=6)
        graph_ctrl.pack(side=tk.RIGHT, padx=(6, 0))

        self._pause_btn = ttk.Button(graph_ctrl, text="⏸ Congelar (Espacio)", command=self._toggle_pause)
        self._pause_btn.pack(side=tk.LEFT, padx=3)

        ttk.Checkbutton(graph_ctrl, text="Max-Hold", variable=self._max_hold_enabled, command=self._on_max_hold_toggle).pack(side=tk.LEFT, padx=5)
        ttk.Button(graph_ctrl, text="Reset Max", command=self._reset_max_hold).pack(side=tk.LEFT, padx=3)

        # --- Barra del Generador de Señal Interna (UART) ---
        sig_frame = ttk.LabelFrame(self, text="Generador de Señal Interna (UART)", padding=(8, 4))
        sig_frame.pack(side=tk.TOP, fill=tk.X, padx=10, pady=(0, 4))

        ttk.Button(sig_frame, text="Fuente (g)", command=lambda: self._send("g")).pack(side=tk.LEFT, padx=2)
        ttk.Button(sig_frame, text="Onda (k)", command=lambda: self._send("k")).pack(side=tk.LEFT, padx=2)
        ttk.Button(sig_frame, text="Preset (w)", command=lambda: self._send("w")).pack(side=tk.LEFT, padx=(2, 8))

        ttk.Button(sig_frame, text="-10 Hz ([)", command=lambda: self._send("[")).pack(side=tk.LEFT, padx=2)
        ttk.Button(sig_frame, text="-1 Hz (-)", command=lambda: self._send("-")).pack(side=tk.LEFT, padx=2)
        ttk.Button(sig_frame, text="+1 Hz (+)", command=lambda: self._send("+")).pack(side=tk.LEFT, padx=2)
        ttk.Button(sig_frame, text="+10 Hz (])", command=lambda: self._send("]")).pack(side=tk.LEFT, padx=(2, 8))

        ttk.Label(sig_frame, text="Hz:").pack(side=tk.LEFT, padx=(0, 3))
        self._sig_freq_var = tk.StringVar(value="1000")
        self._sig_freq_entry = ttk.Entry(sig_frame, textvariable=self._sig_freq_var, width=7)
        self._sig_freq_entry.pack(side=tk.LEFT, padx=(0, 4))
        self._sig_freq_entry.bind("<Return>", lambda _e: self._send_custom_freq())
        self._sig_freq_entry.bind("<Escape>", lambda _e: self.focus_set())
        ttk.Button(sig_frame, text="Fijar Hz", command=self._send_custom_freq).pack(side=tk.LEFT, padx=2)

        self._siggen_status_var = tk.StringVar(value="Fuente: ADC | Onda: SENO | 1000 Hz")
        ttk.Label(
            sig_frame,
            textvariable=self._siggen_status_var,
            foreground=DARK_ACCENT,
            font=("TkDefaultFont", 9, "bold"),
        ).pack(side=tk.RIGHT, padx=(10, 4))

        # --- Panel de Estado (Tarjetas KPI) ---
        self._build_info_panel()

        # --- Notebook: gráficos + log ---
        notebook = ttk.Notebook(self)
        notebook.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=10, pady=(2, 6))
        self._notebook = notebook
        self._build_fft_tab(notebook)
        self._build_bypass_tab(notebook)
        self._build_log_tab(notebook)

    def _build_info_panel(self):
        container = ttk.LabelFrame(self, text="Estado y Parámetros", padding=(8, 4, 8, 8))
        container.pack(side=tk.TOP, fill=tk.X, padx=10, pady=(2, 4))

        self._info_vars = {}
        cards_def = [
            ("fs", "FRECUENCIA (FS)", "--"),
            ("n", "TAMAÑO VENTANA (N)", "--"),
            ("df", "RESOLUCIÓN (ΔF)", "--"),
            ("tw", "DURACIÓN VENTANA (T)", "--"),
            ("peak", "PICO ESPECTRAL (AC)", "--"),
            ("rate", "TASA DE RECEPCIÓN", "0 tramas/s"),
        ]

        for i, (key, title, default_val) in enumerate(cards_def):
            row, col = divmod(i, 3)
            card = tk.Frame(container, bg=DARK_PANEL, bd=1, relief=tk.SOLID,
                            highlightbackground=DARK_PANEL_BORDER, highlightthickness=1)
            card.grid(row=row, column=col, padx=4, pady=3, sticky="nsew")

            lbl_t = tk.Label(card, text=title, font=("TkDefaultFont", 7, "bold"),
                             fg=DARK_MUTED, bg=DARK_PANEL)
            lbl_t.pack(anchor="w", padx=8, pady=(4, 1))

            var_v = tk.StringVar(value=default_val)
            self._info_vars[key] = var_v

            fg_col = DARK_ACCENT if key == "peak" else DARK_FG
            lbl_v = tk.Label(card, textvariable=var_v, font=("TkDefaultFont", 11, "bold"),
                             fg=fg_col, bg=DARK_PANEL)
            lbl_v.pack(anchor="w", padx=8, pady=(0, 5))

        for col in range(3):
            container.columnconfigure(col, weight=1)

    def _build_fft_tab(self, notebook):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text="📊 Espectro (FFT)")

        self._fft_fig = Figure(figsize=(6, 3.8), dpi=100)
        self._fft_fig.patch.set_facecolor(DARK_BG)
        self._fft_ax = self._fft_fig.add_subplot(111)
        self._fft_ax.set_facecolor(DARK_PANEL)
        self._fft_ax.set_title("Espectro FFT (dBFS)", color=DARK_FG, fontsize=11, fontweight="bold", pad=8)
        self._fft_ax.set_xlabel("Frecuencia (Hz)", color=DARK_MUTED, fontsize=9)
        self._fft_ax.set_ylabel("Magnitud (dBFS)", color=DARK_MUTED, fontsize=9)
        self._fft_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
        self._fft_ax.tick_params(colors=DARK_MUTED, labelsize=8)
        for spine in self._fft_ax.spines.values():
            spine.set_color(DARK_PANEL_BORDER)
        self._fft_ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.3, color="#666666")

        # Curvas
        (self._max_hold_line,) = self._fft_ax.plot([], [], linewidth=1.0, linestyle="--", color=COLOR_MAX_HOLD, alpha=0.75, label="Max-Hold", visible=False)
        (self._fft_line,) = self._fft_ax.plot([], [], linewidth=1.1, color=DARK_ACCENT, label="Espectro FFT")
        self._fft_peak_marker = self._fft_ax.axvline(0, color=DARK_ACCENT, linestyle=":", linewidth=1.0, alpha=0.8, visible=False)
        (self._fft_peak_dot,) = self._fft_ax.plot([], [], marker="o", color=COLOR_REF_FFT, markersize=5, visible=False)
        self._fft_peak_text = self._fft_ax.annotate(
            "", xy=(0, 0), xytext=(8, -12), textcoords="offset points",
            color=DARK_FG, fontsize=9, fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.35", fc=DARK_BG, ec=DARK_ACCENT, lw=1.0, alpha=0.9),
            visible=False,
        )
        self._fft_fig.tight_layout()
        self._fft_canvas = FigureCanvasTkAgg(self._fft_fig, master=tab)
        self._fft_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self._fft_canvas.get_tk_widget().bind("<Button-1>", lambda _e: self.focus_set())

        # Hover de cursor
        bottom_bar = ttk.Frame(tab, padding=(8, 2))
        bottom_bar.pack(fill=tk.X, side=tk.BOTTOM)
        ttk.Label(bottom_bar, textvariable=self._cursor_info_var, font=("TkFixedFont", 8), foreground=DARK_MUTED).pack(side=tk.LEFT)
        self._fft_fig.canvas.mpl_connect("motion_notify_event", self._on_fft_hover)

    def _build_bypass_tab(self, notebook):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text="📈 Bypass + Validación")

        self._bypass_fig = Figure(figsize=(6, 4.8), dpi=100)
        self._bypass_fig.patch.set_facecolor(DARK_BG)

        self._bypass_ax = self._bypass_fig.add_subplot(211)
        self._bypass_ax.set_facecolor(DARK_PANEL)
        self._bypass_ax.set_title("Señal temporal (Bypass q15)", color=DARK_FG, fontsize=10, fontweight="bold", pad=6)
        self._bypass_ax.set_xlabel("Tiempo (ms)", color=DARK_MUTED, fontsize=8)
        self._bypass_ax.set_ylabel("Cuentas q15", color=DARK_MUTED, fontsize=8)
        self._bypass_ax.set_ylim(*BYPASS_YLIM)
        self._bypass_ax.tick_params(colors=DARK_MUTED, labelsize=8)
        for spine in self._bypass_ax.spines.values():
            spine.set_color(DARK_PANEL_BORDER)
        self._bypass_ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.3, color="#666666")
        (self._bypass_line,) = self._bypass_ax.plot([], [], linewidth=0.9, color=DARK_ACCENT)

        self._ref_ax = self._bypass_fig.add_subplot(212)
        self._ref_ax.set_facecolor(DARK_PANEL)
        self._ref_ax.set_title("FFT de referencia (PC, rectangular)", color=DARK_FG, fontsize=10, fontweight="bold", pad=6)
        self._ref_ax.set_xlabel("Frecuencia (Hz)", color=DARK_MUTED, fontsize=8)
        self._ref_ax.set_ylabel("Magnitud (dBFS)", color=DARK_MUTED, fontsize=8)
        self._ref_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
        self._ref_ax.tick_params(colors=DARK_MUTED, labelsize=8)
        for spine in self._ref_ax.spines.values():
            spine.set_color(DARK_PANEL_BORDER)
        self._ref_ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.3, color="#666666")
        (self._ref_line,) = self._ref_ax.plot([], [], linewidth=1.0, color=COLOR_REF_FFT)

        self._bypass_fig.tight_layout()
        self._bypass_canvas = FigureCanvasTkAgg(self._bypass_fig, master=tab)
        self._bypass_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self._bypass_canvas.get_tk_widget().bind("<Button-1>", lambda _e: self.focus_set())

    def _build_log_tab(self, notebook):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text="📝 Log de consola")

        # Toolbar superior del log
        log_bar = ttk.Frame(tab, padding=(6, 4))
        log_bar.pack(side=tk.TOP, fill=tk.X)

        ttk.Button(log_bar, text="🗑 Limpiar consola", command=self._clear_log).pack(side=tk.LEFT, padx=4)
        ttk.Checkbutton(log_bar, text="Autoscroll", variable=self._autoscroll_var).pack(side=tk.LEFT, padx=8)

        self._log_count_var = tk.StringVar(value="Líneas: 0")
        ttk.Label(log_bar, textvariable=self._log_count_var, foreground=DARK_MUTED).pack(side=tk.RIGHT, padx=6)

        # Text widget con scrollbar
        text_frame = ttk.Frame(tab)
        text_frame.pack(fill=tk.BOTH, expand=True)

        scrollbar = ttk.Scrollbar(text_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        self._log_text = tk.Text(
            text_frame, wrap=tk.NONE, state=tk.DISABLED,
            bg=DARK_PANEL, fg=DARK_FG, insertbackground=DARK_FG,
            selectbackground=DARK_ACCENT, borderwidth=0, highlightthickness=0,
            yscrollcommand=scrollbar.set, font=("TkFixedFont", 9),
        )
        self._log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=self._log_text.yview)

        # Tags con colores
        self._log_text.tag_configure("estado", foreground="#38bdf8", font=("TkFixedFont", 9, "bold"))
        self._log_text.tag_configure("sig", foreground="#a78bfa", font=("TkFixedFont", 9, "bold"))
        self._log_text.tag_configure("info", foreground="#4ade80")
        self._log_text.tag_configure("warn", foreground="#fbbf24")
        self._log_text.tag_configure("error", foreground="#f87171", font=("TkFixedFont", 9, "bold"))
        self._log_text.tag_configure("export", foreground="#c084fc")
        self._log_text.tag_configure("normal", foreground=DARK_FG)

    # -------------------------------------------------------------- serial
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self._port_combo["values"] = ports
        if ports and not self._port_combo.get():
            self._port_combo.set(ports[0])

    def _toggle_connection(self):
        if self._worker.is_open:
            self._worker.disconnect()
            self._connect_btn.config(text="⚡ Conectar")
            self._led_canvas.itemconfig(self._led, fill=COLOR_DISCONNECTED)
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
        self._total_frames = 0
        self._max_hold_db = None

        self._connect_btn.config(text="🔌 Desconectar")
        self._led_canvas.itemconfig(self._led, fill=COLOR_CONNECTED)
        self._status_var.set(f"Conectado a {port} @ {baud}")

    def _send(self, ch: str):
        if not self._worker.is_open:
            messagebox.showwarning("Sin conexión", "Conectate a un puerto serie primero.")
            return
        self._worker.send_char(ch)

    def _send_custom_freq(self):
        raw = self._sig_freq_var.get().strip()
        try:
            freq = int(raw)
        except ValueError:
            messagebox.showwarning("Frecuencia inválida", "Ingresá un número entero en Hz (1 a 50000).")
            return
        if not (1 <= freq <= 50000):
            messagebox.showwarning("Frecuencia fuera de rango", "La frecuencia debe estar entre 1 y 50000 Hz.")
            return
        if not self._worker.is_open:
            messagebox.showwarning("Sin conexión", "Conectate a un puerto serie primero.")
            return
        self._worker.send_text(f"{freq}\n")
        self.focus_set()

    def _on_close(self):
        self._worker.disconnect()
        self.destroy()

    def _toggle_pause(self):
        self._paused = not self._paused
        if self._paused:
            self._pause_btn.config(text="▶ Reanudar (Espacio)")
            self._status_var.set(self._status_var.get() + " [PAUSADO]")
        else:
            self._pause_btn.config(text="⏸ Congelar (Espacio)")
            cur = self._status_var.get().replace(" [PAUSADO]", "")
            self._status_var.set(cur)

    def _on_max_hold_toggle(self):
        if not self._max_hold_enabled.get():
            self._max_hold_db = None
            self._max_hold_line.set_visible(False)
            self._fft_canvas.draw_idle()

    def _reset_max_hold(self):
        self._max_hold_db = None
        self._max_hold_line.set_visible(False)
        self._fft_canvas.draw_idle()

    def _on_key_press(self, event):
        if isinstance(event.widget, (ttk.Combobox, ttk.Entry, tk.Entry, tk.Text)):
            return
        ch = event.char.lower()
        hotkeys = set(COMMANDS.values()) | {"g", "k", "w", "+", "-", "[", "]"}
        if ch in hotkeys:
            self._send(ch)
        elif event.keysym == "space":
            self._toggle_pause()

    def _on_fft_hover(self, event):
        if event.inaxes == self._fft_ax and event.xdata is not None and event.ydata is not None:
            self._cursor_info_var.set(f"Cursor: {event.xdata:.1f} Hz  |  {event.ydata:.1f} dBFS")
        else:
            self._cursor_info_var.set("")

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
        elif kind == "frame":
            self._handle_frame(payload)

    def _handle_frame(self, frame: Frame):
        self._total_frames += 1
        self._frame_times.append(time.monotonic())
        self._last_frame = frame
        if frame.is_fft:
            self._last_fft_frame = frame
            self._fft_dirty = True
        else:
            self._last_bypass_frame = frame
            self._bypass_dirty = True

    def _log(self, line: str):
        tag = "normal"
        if "[SIG]" in line:
            tag = "sig"
            sig_summary = line.split("[SIG]", 1)[1].strip()
            self._siggen_status_var.set(sig_summary)
            if self.focus_get() is not self._sig_freq_entry and "Frecuencia:" in sig_summary:
                freq_part = sig_summary.split("Frecuencia:", 1)[1].strip().split()[0]
                if freq_part.isdigit():
                    self._sig_freq_var.set(freq_part)
        elif "[ESTADO]" in line:
            tag = "estado"
        elif "[ERROR]" in line:
            tag = "error"
        elif "[INFO]" in line:
            tag = "info"
        elif "[DSP]" in line or "[WARN]" in line:
            tag = "warn"
        elif "[EXPORT]" in line:
            tag = "export"

        self._log_text.config(state=tk.NORMAL)
        self._log_text.insert(tk.END, line + "\n", tag)
        num_lines = int(self._log_text.index("end-1c").split(".")[0])
        if num_lines > LOG_MAX_LINES:
            self._log_text.delete("1.0", f"{num_lines - LOG_MAX_LINES}.0")
            num_lines = LOG_MAX_LINES
        self._log_count_var.set(f"Líneas: {num_lines}")
        if self._autoscroll_var.get():
            self._log_text.see(tk.END)
        self._log_text.config(state=tk.DISABLED)

    def _clear_log(self):
        self._log_text.config(state=tk.NORMAL)
        self._log_text.delete("1.0", tk.END)
        self._log_count_var.set("Líneas: 0")
        self._log_text.config(state=tk.DISABLED)

    # -------------------------------------------------------------- plots
    def _refresh_plots(self):
        if not self._paused:
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
            self._max_hold_db = None
            self._fft_ax.set_xlim(0, frame.fs / 2.0)
            self._fft_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
            self._fft_ax.set_title(
                f"Espectro FFT (N={frame.n}, Fs={frame.fs} Hz, Δf={frame.fs/frame.n:.2f} Hz)",
                color=DARK_FG, fontsize=11, fontweight="bold", pad=8
            )

        # Max-Hold
        if self._max_hold_enabled.get():
            if self._max_hold_db is None or len(self._max_hold_db) != len(db):
                self._max_hold_db = np.copy(db)
            else:
                self._max_hold_db = np.maximum(self._max_hold_db, db)
            self._max_hold_line.set_data(freqs, self._max_hold_db)
            self._max_hold_line.set_visible(True)
        else:
            self._max_hold_line.set_visible(False)

        # Marcador del pico
        if 0 <= frame.peak_bin < frame.length:
            peak_freq = frame.peak_freq_hz
            peak_db = frame.peak_db
            self._fft_peak_marker.set_xdata([peak_freq, peak_freq])
            self._fft_peak_marker.set_visible(True)
            self._fft_peak_dot.set_data([peak_freq], [peak_db])
            self._fft_peak_dot.set_visible(True)
            self._fft_peak_text.xy = (peak_freq, peak_db)
            self._fft_peak_text.set_text(f"Pico: {peak_freq:.1f} Hz\n{peak_db:.1f} dBFS")
            self._fft_peak_text.set_visible(True)
        else:
            self._fft_peak_marker.set_visible(False)
            self._fft_peak_dot.set_visible(False)
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
            self._info_vars["fs"].set(f"{frame.fs} Hz")
            self._info_vars["n"].set(f"{frame.n} muestras")
            self._info_vars["df"].set(f"{df:.2f} Hz")
            self._info_vars["tw"].set(f"{tw * 1000:.2f} ms")

            if frame.is_fft and 0 <= frame.peak_bin < frame.length:
                self._info_vars["peak"].set(f"{frame.peak_freq_hz:.1f} Hz  ({frame.peak_db:.1f} dBFS)")
            else:
                self._info_vars["peak"].set("--")

        now = time.monotonic()
        while self._frame_times and now - self._frame_times[0] > RATE_WINDOW_S:
            self._frame_times.popleft()
        self._info_vars["rate"].set(f"{len(self._frame_times)} tramas/s")

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
        fig.savefig(png_path, dpi=150, facecolor=fig.get_facecolor(), edgecolor="none")

        self._status_var.set(f"Exportado: {csv_path.name}")
        self._log(f"[EXPORT] CSV: {csv_path.name}")
        self._log(f"[EXPORT] PNG: {png_path.name}")


if __name__ == "__main__":
    app = FftGuiApp()
    app.mainloop()
