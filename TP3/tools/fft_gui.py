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
import tkinter.font as tkfont
from collections import deque
from pathlib import Path
from tkinter import messagebox, ttk

import matplotlib as mpl
import numpy as np
import serial
import serial.tools.list_ports
from matplotlib import font_manager
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from matplotlib.lines import Line2D
from matplotlib.patches import Polygon
from matplotlib.ticker import EngFormatter

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
BAUD_RATE_DEFAULT = "921600"  # igual a BOARD_DEBUG_UART_BAUDRATE (board.h)

LOG_MAX_LINES = 500
PLOT_REFRESH_MS = 50       # ~20 Hz de refresco de gráficos
QUEUE_POLL_MS = 20
RATE_WINDOW_S = 1.0        # ventana para calcular tramas/s
FFT_DB_MIN, FFT_DB_MAX = -100.0, 0.0
BYPASS_YLIM = (-33000, 33000)

AVERAGING_OPTIONS = ("Sin promedio", "2", "4", "8", "16")
FREQ_SCALES = ("Lineal", "Logarítmica")
ZOOM_STEP = 0.8            # factor de zoom por paso de rueda

# Comandos de un solo caracter según el firmware (m, f, r, c, s, t)
COMMANDS = {
    "Modo / N": "m",
    "Cambiar Fs": "f",
    "RUN / STOP": "r",
    "Modo continuo": "c",
    "Captura única": "s",
    "Estado / contadores": "t",
}

# --- Paleta: azul noche, acento azul/cian y detalles en amarillo ---
BG = "#0b1220"              # Fondo de la ventana
SURFACE = "#111a2e"         # Tarjetas y paneles
SURFACE_ALT = "#17233d"     # Campos, botones secundarios
SURFACE_HOVER = "#1e2d4d"
BORDER = "#22314f"          # Bordes sutiles
GRID = "#1c2944"            # Grilla de los gráficos
FG = "#e6edf7"              # Texto primario
MUTED = "#8a9bb8"           # Texto secundario
ACCENT = "#38bdf8"          # Azul cian (trazas, títulos de sección)
ACCENT_STRONG = "#2563eb"   # Azul intenso (botón primario)
ACCENT_STRONG_HOVER = "#1d4ed8"
YELLOW = "#facc15"          # Amarillo (pico, max-hold, exportar)
YELLOW_HOVER = "#eab308"
ON_YELLOW = "#1a1400"       # Texto sobre amarillo
COLOR_CONNECTED = "#22c55e"
COLOR_DISCONNECTED = "#ef4444"
COLOR_MAX_HOLD = YELLOW
COLOR_REF_FFT = YELLOW      # FFT calculada en la PC
COLOR_BOARD_FFT = ACCENT    # FFT de la placa


def _pick_tk_font(root, candidates, fallback):
    available = set(tkfont.families(root))
    return next((f for f in candidates if f in available), fallback)


def _pick_mpl_font(candidates):
    available = {f.name for f in font_manager.fontManager.ttflist}
    return next((f for f in candidates if f in available), "DejaVu Sans")


UI_FONTS = ("Inter", "Segoe UI", "Cantarell", "Ubuntu", "Noto Sans", "Helvetica")
MONO_FONTS = ("JetBrains Mono", "Cascadia Mono", "Noto Sans Mono", "DejaVu Sans Mono",
              "Consolas", "Menlo", "Courier")

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

    @property
    def is_running(self) -> bool:
        """False si el hilo lector terminó (p. ej. se desenchufó la placa)."""
        return self._thread is not None and self._thread.is_alive()

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
        self.title("TP3 DSP - Analizador de espectro FFT (FRDM-MCXN947)")
        self.geometry("1280x880")
        self.minsize(1040, 720)

        self._font = _pick_tk_font(self, UI_FONTS, "TkDefaultFont")
        self._mono = _pick_tk_font(self, MONO_FONTS, "TkFixedFont")

        self._chunk_queue: "queue.Queue[bytes]" = queue.Queue()
        self._worker = SerialWorker(self._chunk_queue)
        self._parser = FrameParser()
        self._connected = False

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
        self._avg_var = tk.StringVar(value=AVERAGING_OPTIONS[0])
        self._avg_power: np.ndarray | None = None
        self._freq_scale_var = tk.StringVar(value=FREQ_SCALES[0])
        self._bypass_autoscale = tk.BooleanVar(value=False)
        self._fft_display: tuple[np.ndarray, np.ndarray] | None = None
        self._cursor_info_var = tk.StringVar(value="Pasá el mouse sobre el espectro para medir")
        self._frame_info_var = tk.StringVar(value="Sin tramas")
        self._autoscroll_var = tk.BooleanVar(value=True)

        self._apply_theme()
        self._apply_mpl_theme()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<Key>", self._on_key_press)
        self.after(QUEUE_POLL_MS, self._poll_queue)
        self.after(PLOT_REFRESH_MS, self._refresh_plots)
        self.after(500, self._refresh_info_panel)

    # --------------------------------------------------------------- theme
    def _f(self, size=10, weight="normal"):
        return (self._font, size, weight)

    def _button_style(self, name, bg, fg, hover, pressed, bold=False, padding=(14, 7)):
        style = ttk.Style(self)
        style.configure(name, background=bg, foreground=fg, bordercolor=bg,
                        lightcolor=bg, darkcolor=bg, focuscolor=bg, relief="flat",
                        borderwidth=1, padding=padding,
                        font=self._f(10, "bold" if bold else "normal"))
        style.map(name,
                  background=[("disabled", SURFACE), ("pressed", pressed), ("active", hover)],
                  lightcolor=[("pressed", pressed), ("active", hover)],
                  darkcolor=[("pressed", pressed), ("active", hover)],
                  bordercolor=[("pressed", pressed), ("active", hover)],
                  foreground=[("disabled", MUTED)])

    def _apply_theme(self):
        self.configure(bg=BG)
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        style.configure(".", background=BG, foreground=FG, fieldbackground=SURFACE_ALT,
                        bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER,
                        troughcolor=SURFACE, focuscolor=ACCENT, font=self._f(10),
                        selectbackground=ACCENT_STRONG, selectforeground="#ffffff",
                        insertcolor=FG)
        style.configure("TFrame", background=BG)
        style.configure("Card.TFrame", background=SURFACE)
        style.configure("TLabel", background=BG, foreground=FG)
        style.configure("Card.TLabel", background=SURFACE, foreground=FG)
        style.configure("Muted.TLabel", background=BG, foreground=MUTED, font=self._f(9))
        style.configure("Muted.Card.TLabel", background=SURFACE, foreground=MUTED, font=self._f(9))

        # Botones
        self._button_style("TButton", SURFACE_ALT, FG, SURFACE_HOVER, BORDER)
        self._button_style("Primary.TButton", ACCENT_STRONG, "#ffffff",
                           ACCENT_STRONG_HOVER, "#1e40af", bold=True)
        self._button_style("Danger.TButton", "#3b1621", "#fca5a5", "#4c1d2a", "#5b2131", bold=True)
        self._button_style("Accent.TButton", YELLOW, ON_YELLOW, YELLOW_HOVER, "#ca8a04", bold=True)
        self._button_style("Icon.TButton", SURFACE_ALT, ACCENT, SURFACE_HOVER, BORDER,
                           bold=True, padding=(9, 7))
        self._button_style("Small.TButton", SURFACE_ALT, FG, SURFACE_HOVER, BORDER, padding=(10, 4))
        style.configure("Cmd.TButton", anchor="w")
        self._button_style("Cmd.TButton", SURFACE_ALT, FG, SURFACE_HOVER, BORDER, padding=(12, 7))
        style.configure("Cmd.TButton", anchor="w")

        # Checkbuttons
        for name, bg in (("TCheckbutton", BG), ("Card.TCheckbutton", SURFACE)):
            style.configure(name, background=bg, foreground=FG, indicatorbackground=SURFACE_ALT,
                            indicatorforeground=ON_YELLOW, indicatormargin=(0, 0, 8, 0),
                            focuscolor=bg, padding=(0, 4))
            style.map(name, background=[("active", bg)],
                      indicatorbackground=[("selected", YELLOW), ("active", SURFACE_HOVER)])

        # Combobox
        style.configure("TCombobox", fieldbackground=SURFACE_ALT, background=SURFACE_ALT,
                        foreground=FG, arrowcolor=ACCENT, bordercolor=BORDER,
                        lightcolor=SURFACE_ALT, darkcolor=SURFACE_ALT, padding=(8, 5))
        style.map("TCombobox",
                  fieldbackground=[("readonly", SURFACE_ALT)],
                  foreground=[("readonly", FG)],
                  selectbackground=[("readonly", SURFACE_ALT), ("!focus", SURFACE_ALT)],
                  selectforeground=[("readonly", FG), ("!focus", FG)],
                  bordercolor=[("focus", ACCENT)],
                  background=[("active", SURFACE_HOVER)])
        self.option_add("*TCombobox*Listbox.background", SURFACE_ALT)
        self.option_add("*TCombobox*Listbox.foreground", FG)
        self.option_add("*TCombobox*Listbox.selectBackground", ACCENT_STRONG)
        self.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")
        self.option_add("*TCombobox*Listbox.font", self._f(10))
        self.option_add("*TCombobox*Listbox.borderWidth", 0)

        # Notebook con pestañas planas; la seleccionada lleva línea amarilla
        style.configure("TNotebook", background=BG, borderwidth=0, tabmargins=(0, 0, 0, 0))
        style.configure("TNotebook.Tab", background=BG, foreground=MUTED, padding=(18, 9),
                        borderwidth=0, lightcolor=BG, darkcolor=BG, bordercolor=BG,
                        focuscolor=BG, font=self._f(10, "bold"))
        style.map("TNotebook.Tab",
                  background=[("selected", SURFACE), ("active", SURFACE_ALT)],
                  foreground=[("selected", FG), ("active", FG)],
                  lightcolor=[("selected", YELLOW)],
                  bordercolor=[("selected", SURFACE)])

        style.configure("Vertical.TScrollbar", background=SURFACE_ALT, troughcolor=SURFACE,
                        bordercolor=SURFACE, lightcolor=SURFACE_ALT, darkcolor=SURFACE_ALT,
                        arrowcolor=MUTED, gripcount=0)
        style.map("Vertical.TScrollbar", background=[("active", SURFACE_HOVER)])

    def _apply_mpl_theme(self):
        mpl.rcParams.update({
            "font.family": _pick_mpl_font(UI_FONTS),
            "font.size": 9,
            "figure.facecolor": SURFACE,
            "axes.facecolor": SURFACE,
            "axes.edgecolor": BORDER,
            "axes.labelcolor": MUTED,
            "axes.titlecolor": FG,
            "axes.titlesize": 11,
            "axes.titleweight": "bold",
            "axes.titlelocation": "left",
            "axes.titlepad": 10,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": True,
            "grid.color": GRID,
            "grid.linewidth": 0.8,
            "xtick.color": MUTED,
            "ytick.color": MUTED,
            "legend.frameon": False,
            "legend.labelcolor": FG,
            "legend.fontsize": 8.5,
            "savefig.facecolor": SURFACE,
        })

    # ------------------------------------------------------------------ UI
    def _card(self, parent, title: str | None = None, padding=12):
        """Tarjeta: fondo SURFACE con borde fino. Devuelve el frame interior."""
        outer = tk.Frame(parent, bg=SURFACE, highlightthickness=1,
                         highlightbackground=BORDER, highlightcolor=BORDER)
        inner = tk.Frame(outer, bg=SURFACE)
        inner.pack(fill=tk.BOTH, expand=True, padx=padding, pady=padding)
        if title:
            tk.Label(inner, text=title.upper(), bg=SURFACE, fg=ACCENT,
                     font=self._f(8, "bold"), anchor="w").pack(fill=tk.X, pady=(0, 8))
        outer.inner = inner
        return outer

    def _key_badge(self, parent, text):
        return tk.Label(parent, text=text, bg=BORDER, fg=YELLOW, font=(self._mono, 8, "bold"),
                        padx=6, pady=1)

    def _build_ui(self):
        self._build_header()
        self._build_toolbar()
        self._build_statusbar()

        body = ttk.Frame(self, padding=(16, 0, 16, 12))
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        sidebar = ttk.Frame(body, width=250)
        sidebar.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 14))
        self._build_sidebar(sidebar)

        main = ttk.Frame(body)
        main.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self._build_info_panel(main)

        notebook = ttk.Notebook(main)
        notebook.pack(side=tk.TOP, fill=tk.BOTH, expand=True, pady=(12, 0))
        self._notebook = notebook
        self._build_fft_tab(notebook)
        self._build_bypass_tab(notebook)
        self._build_log_tab(notebook)

    def _build_header(self):
        hdr = tk.Frame(self, bg=SURFACE)
        hdr.pack(side=tk.TOP, fill=tk.X)
        tk.Frame(hdr, bg=YELLOW, width=5).pack(side=tk.LEFT, fill=tk.Y)

        titles = tk.Frame(hdr, bg=SURFACE)
        titles.pack(side=tk.LEFT, padx=16, pady=10)
        tk.Label(titles, text="Analizador de espectro FFT", bg=SURFACE, fg=FG,
                 font=self._f(16, "bold")).pack(anchor="w")
        tk.Label(titles, text="TP3 · Procesamiento Digital de Señales · FRDM-MCXN947",
                 bg=SURFACE, fg=MUTED, font=self._f(9)).pack(anchor="w")

        # Píldora de estado de conexión
        pill = tk.Frame(hdr, bg=SURFACE_ALT, highlightthickness=1, highlightbackground=BORDER)
        pill.pack(side=tk.RIGHT, padx=16, pady=14)
        self._led_canvas = tk.Canvas(pill, width=14, height=14, bg=SURFACE_ALT, highlightthickness=0)
        self._led_canvas.pack(side=tk.LEFT, padx=(12, 6), pady=8)
        self._led = self._led_canvas.create_oval(2, 2, 12, 12, fill=COLOR_DISCONNECTED, outline="")
        self._status_var = tk.StringVar(value="Desconectado")
        tk.Label(pill, textvariable=self._status_var, bg=SURFACE_ALT, fg=FG,
                 font=self._f(9, "bold")).pack(side=tk.LEFT, padx=(0, 14))

        self._paused_badge = tk.Label(hdr, text="  ❚❚  CONGELADO  ", bg=YELLOW, fg=ON_YELLOW,
                                      font=self._f(9, "bold"), pady=4)

        tk.Frame(self, bg=BORDER, height=1).pack(side=tk.TOP, fill=tk.X)

    def _build_toolbar(self):
        bar = ttk.Frame(self, padding=(16, 12, 16, 12))
        bar.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(bar, text="Puerto", style="Muted.TLabel").pack(side=tk.LEFT, padx=(0, 6))
        self._port_combo = ttk.Combobox(bar, width=20)
        self._port_combo.pack(side=tk.LEFT)
        self._refresh_ports()
        ttk.Button(bar, text="↻", style="Icon.TButton", width=2,
                   command=self._refresh_ports).pack(side=tk.LEFT, padx=(6, 16))

        ttk.Label(bar, text="Baudios", style="Muted.TLabel").pack(side=tk.LEFT, padx=(0, 6))
        self._baud_combo = ttk.Combobox(bar, width=9, values=BAUD_RATES)
        self._baud_combo.set(BAUD_RATE_DEFAULT)
        self._baud_combo.pack(side=tk.LEFT, padx=(0, 16))

        self._connect_btn = ttk.Button(bar, text="Conectar", style="Primary.TButton",
                                       command=self._toggle_connection)
        self._connect_btn.pack(side=tk.LEFT)

        ttk.Button(bar, text="Exportar captura", style="Accent.TButton",
                   command=self._export_capture).pack(side=tk.RIGHT)

    def _build_sidebar(self, sidebar):
        # --- Comandos del firmware ---
        cmd = self._card(sidebar, "Comandos de la placa")
        cmd.pack(side=tk.TOP, fill=tk.X)
        for label, ch in COMMANDS.items():
            row = tk.Frame(cmd.inner, bg=SURFACE)
            row.pack(fill=tk.X, pady=2)
            ttk.Button(row, text=label, style="Cmd.TButton",
                       command=lambda c=ch: self._send(c)).pack(side=tk.LEFT, fill=tk.X, expand=True)
            self._key_badge(row, ch.upper()).pack(side=tk.RIGHT, padx=(8, 0))

        # --- Visualización ---
        vis = self._card(sidebar, "Visualización")
        vis.pack(side=tk.TOP, fill=tk.X, pady=(12, 0))
        v = vis.inner

        row = tk.Frame(v, bg=SURFACE)
        row.pack(fill=tk.X, pady=(0, 6))
        self._pause_btn = ttk.Button(row, text="❚❚  Congelar", style="Cmd.TButton",
                                     command=self._toggle_pause)
        self._pause_btn.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._key_badge(row, "ESP").pack(side=tk.RIGHT, padx=(8, 0))

        row = tk.Frame(v, bg=SURFACE)
        row.pack(fill=tk.X, pady=2)
        ttk.Checkbutton(row, text="Max-Hold", style="Card.TCheckbutton",
                        variable=self._max_hold_enabled,
                        command=self._on_max_hold_toggle).pack(side=tk.LEFT)
        ttk.Button(row, text="Reiniciar", style="Small.TButton",
                   command=self._reset_max_hold).pack(side=tk.RIGHT)

        ttk.Label(v, text="Promediado (exponencial)", style="Muted.Card.TLabel").pack(anchor="w", pady=(10, 3))
        avg = ttk.Combobox(v, state="readonly", values=AVERAGING_OPTIONS, textvariable=self._avg_var)
        avg.pack(fill=tk.X)
        avg.bind("<<ComboboxSelected>>", lambda _e: self._reset_average())

        ttk.Label(v, text="Eje de frecuencia", style="Muted.Card.TLabel").pack(anchor="w", pady=(10, 3))
        scale = ttk.Combobox(v, state="readonly", values=FREQ_SCALES, textvariable=self._freq_scale_var)
        scale.pack(fill=tk.X)
        scale.bind("<<ComboboxSelected>>", lambda _e: self._apply_freq_scale())

        ttk.Checkbutton(v, text="Auto-escala señal (Bypass)", style="Card.TCheckbutton",
                        variable=self._bypass_autoscale,
                        command=self._on_bypass_autoscale).pack(anchor="w", pady=(10, 0))

        tk.Label(v, text="Rueda del mouse: zoom en frecuencia\nDoble clic: vista completa",
                 bg=SURFACE, fg=MUTED, font=self._f(8), justify=tk.LEFT).pack(anchor="w", pady=(10, 0))

    def _build_info_panel(self, parent):
        grid = ttk.Frame(parent)
        grid.pack(side=tk.TOP, fill=tk.X)

        self._info_vars: dict[str, tk.StringVar] = {}
        self._info_sub_vars: dict[str, tk.StringVar] = {}
        cards_def = [
            ("fs", "Frecuencia de muestreo", ACCENT),
            ("n", "Tamaño de ventana", ACCENT),
            ("df", "Resolución Δf", ACCENT),
            ("tw", "Duración de ventana", ACCENT),
            ("peak", "Pico espectral", YELLOW),
            ("floor", "Piso de ruido", ACCENT),
            ("rate", "Tasa de recepción", ACCENT),
            ("total", "Tramas recibidas", ACCENT),
        ]
        cols = 4
        for i, (key, title, stripe) in enumerate(cards_def):
            row, col = divmod(i, cols)
            card = tk.Frame(grid, bg=SURFACE, highlightthickness=1, highlightbackground=BORDER)
            card.grid(row=row, column=col, sticky="nsew",
                      padx=(0 if col == 0 else 5, 0 if col == cols - 1 else 5),
                      pady=(0 if row == 0 else 5, 0 if row else 5))
            tk.Frame(card, bg=stripe, width=3).pack(side=tk.LEFT, fill=tk.Y)
            body = tk.Frame(card, bg=SURFACE)
            body.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=12, pady=8)

            tk.Label(body, text=title.upper(), font=self._f(7, "bold"), fg=MUTED,
                     bg=SURFACE, anchor="w").pack(fill=tk.X)
            var_v = tk.StringVar(value="—")
            self._info_vars[key] = var_v
            tk.Label(body, textvariable=var_v, font=self._f(14, "bold"),
                     fg=YELLOW if key == "peak" else FG, bg=SURFACE, anchor="w").pack(fill=tk.X)
            var_s = tk.StringVar(value=" ")
            self._info_sub_vars[key] = var_s
            tk.Label(body, textvariable=var_s, font=self._f(8), fg=MUTED,
                     bg=SURFACE, anchor="w").pack(fill=tk.X)

        for col in range(cols):
            grid.columnconfigure(col, weight=1, uniform="kpi")

    def _build_statusbar(self):
        bar = tk.Frame(self, bg=SURFACE)
        bar.pack(side=tk.BOTTOM, fill=tk.X)
        tk.Frame(bar, bg=BORDER, height=1).pack(side=tk.TOP, fill=tk.X)
        tk.Label(bar, textvariable=self._cursor_info_var, bg=SURFACE, fg=ACCENT,
                 font=(self._mono, 9)).pack(side=tk.LEFT, padx=16, pady=5)
        tk.Label(bar, textvariable=self._frame_info_var, bg=SURFACE, fg=MUTED,
                 font=(self._mono, 9)).pack(side=tk.RIGHT, padx=16, pady=5)

    @staticmethod
    def _style_axes(ax, title, xlabel, ylabel):
        ax.set_title(title)
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.tick_params(length=0, pad=6)
        ax.grid(True, which="major")
        ax.set_axisbelow(True)

    @staticmethod
    def _embed_figure(fig, master):
        canvas = FigureCanvasTkAgg(fig, master=master)
        widget = canvas.get_tk_widget()
        widget.configure(bg=SURFACE, highlightthickness=0)
        widget.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        def _relayout(_event):
            try:
                fig.tight_layout(pad=1.2)
            except ValueError:
                pass
        canvas.mpl_connect("resize_event", _relayout)
        return canvas

    def _build_fft_tab(self, notebook):
        tab = ttk.Frame(notebook, style="Card.TFrame")
        notebook.add(tab, text="Espectro FFT")

        self._fft_fig = Figure(figsize=(6, 3.8), dpi=100)
        ax = self._fft_ax = self._fft_fig.add_subplot(111)
        self._style_axes(ax, "Espectro de magnitud", "Frecuencia", "Magnitud (dBFS)")
        ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
        ax.set_xlim(0, VALID_FS[-1] / 2)
        ax.xaxis.set_major_formatter(EngFormatter(unit="Hz"))

        # Relleno bajo la curva (Polygon se actualiza con set_xy, barato)
        self._fft_fill = Polygon(np.zeros((1, 2)), closed=True, facecolor=ACCENT,
                                 alpha=0.12, edgecolor="none", visible=False)
        ax.add_patch(self._fft_fill)
        (self._max_hold_line,) = ax.plot([], [], linewidth=1.0, color=COLOR_MAX_HOLD,
                                         alpha=0.8, label="Max-Hold", visible=False)
        (self._fft_line,) = ax.plot([], [], linewidth=1.3, color=ACCENT, label="Espectro")
        self._fft_peak_marker = ax.axvline(0, color=YELLOW, linestyle=(0, (2, 3)),
                                           linewidth=1.0, alpha=0.7, visible=False)
        (self._fft_peak_dot,) = ax.plot([], [], marker="o", color=YELLOW, markersize=7,
                                        markeredgecolor=BG, markeredgewidth=1.5, visible=False)
        self._fft_peak_text = ax.annotate(
            "", xy=(0, 0), xytext=(10, -14), textcoords="offset points",
            color=FG, fontsize=9, fontweight="bold", va="top",
            bbox=dict(boxstyle="round,pad=0.45", fc=BG, ec=YELLOW, lw=1.0, alpha=0.92),
            visible=False,
        )
        # Cursor de medición (snap al bin más cercano)
        self._cursor_vline = ax.axvline(0, color=MUTED, linewidth=0.8, alpha=0.6, visible=False)
        (self._cursor_dot,) = ax.plot([], [], marker="o", markersize=5, color=FG,
                                      markeredgecolor=ACCENT, visible=False)
        self._update_fft_legend()

        self._fft_canvas = self._embed_figure(self._fft_fig, tab)
        self._fft_canvas.mpl_connect("motion_notify_event", self._on_fft_hover)
        self._fft_canvas.mpl_connect("axes_leave_event", lambda _e: self._hide_cursor())
        self._fft_canvas.mpl_connect("scroll_event", self._on_scroll_zoom)
        self._fft_canvas.mpl_connect("button_press_event", self._on_plot_click)

    def _build_bypass_tab(self, notebook):
        tab = ttk.Frame(notebook, style="Card.TFrame")
        notebook.add(tab, text="Bypass + validación")

        self._bypass_fig = Figure(figsize=(6, 4.8), dpi=100)

        ax = self._bypass_ax = self._bypass_fig.add_subplot(211)
        self._style_axes(ax, "Señal temporal (Bypass q15)", "Tiempo (ms)", "Cuentas q15")
        ax.set_ylim(*BYPASS_YLIM)
        ax.axhline(0, color=BORDER, linewidth=1.0)
        (self._bypass_line,) = ax.plot([], [], linewidth=1.0, color=ACCENT)
        self._bypass_stats = ax.text(
            0.995, 0.96, "", transform=ax.transAxes, ha="right", va="top",
            color=FG, fontsize=8.5, family=_pick_mpl_font(MONO_FONTS),
            bbox=dict(boxstyle="round,pad=0.4", fc=BG, ec=BORDER, alpha=0.9),
        )

        ax = self._ref_ax = self._bypass_fig.add_subplot(212)
        self._style_axes(ax, "Validación: FFT en la PC vs. FFT de la placa", "Frecuencia",
                         "Magnitud (dBFS)")
        ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
        ax.set_xlim(0, VALID_FS[-1] / 2)
        ax.xaxis.set_major_formatter(EngFormatter(unit="Hz"))
        (self._board_line,) = ax.plot([], [], linewidth=1.0, color=COLOR_BOARD_FFT, alpha=0.55,
                                      label="FFT placa (última trama)", visible=False)
        (self._ref_line,) = ax.plot([], [], linewidth=1.2, color=COLOR_REF_FFT,
                                    label="FFT PC (rectangular)")
        ax.legend(handles=[
            Line2D([], [], color=COLOR_REF_FFT, linewidth=1.6, label="FFT PC (rectangular)"),
            Line2D([], [], color=COLOR_BOARD_FFT, linewidth=1.6, alpha=0.7,
                   label="FFT placa (última trama, misma Fs)"),
        ], loc="upper right")

        self._bypass_placeholder = self._bypass_fig.text(
            0.5, 0.5, "Sin tramas Bypass todavía\nUsá  Modo / N  (tecla M) para pasar a modo Bypass",
            ha="center", va="center", color=MUTED, fontsize=11, linespacing=1.8,
            bbox=dict(boxstyle="round,pad=0.9", fc=BG, ec=BORDER))

        self._bypass_canvas = self._embed_figure(self._bypass_fig, tab)
        self._bypass_canvas.mpl_connect("scroll_event", self._on_scroll_zoom)
        self._bypass_canvas.mpl_connect("button_press_event", self._on_plot_click)

    def _build_log_tab(self, notebook):
        tab = ttk.Frame(notebook, style="Card.TFrame")
        notebook.add(tab, text="Consola")

        log_bar = tk.Frame(tab, bg=SURFACE)
        log_bar.pack(side=tk.TOP, fill=tk.X, padx=12, pady=(10, 6))
        ttk.Button(log_bar, text="Limpiar", style="Small.TButton",
                   command=self._clear_log).pack(side=tk.LEFT)
        ttk.Checkbutton(log_bar, text="Autoscroll", style="Card.TCheckbutton",
                        variable=self._autoscroll_var).pack(side=tk.LEFT, padx=12)
        self._log_count_var = tk.StringVar(value="0 líneas")
        ttk.Label(log_bar, textvariable=self._log_count_var,
                  style="Muted.Card.TLabel").pack(side=tk.RIGHT)

        text_frame = tk.Frame(tab, bg=BG, highlightthickness=1, highlightbackground=BORDER)
        text_frame.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 12))
        scrollbar = ttk.Scrollbar(text_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self._log_text = tk.Text(
            text_frame, wrap=tk.NONE, state=tk.DISABLED,
            bg=BG, fg=FG, insertbackground=FG, padx=10, pady=8,
            selectbackground=ACCENT_STRONG, borderwidth=0, highlightthickness=0,
            yscrollcommand=scrollbar.set, font=(self._mono, 9),
        )
        self._log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=self._log_text.yview)

        self._log_text.tag_configure("estado", foreground=ACCENT, font=(self._mono, 9, "bold"))
        self._log_text.tag_configure("info", foreground="#4ade80")
        self._log_text.tag_configure("warn", foreground=YELLOW)
        self._log_text.tag_configure("error", foreground="#f87171", font=(self._mono, 9, "bold"))
        self._log_text.tag_configure("export", foreground="#fde68a")
        self._log_text.tag_configure("normal", foreground=FG)

    # -------------------------------------------------------------- serial
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self._port_combo["values"] = ports
        if ports and not self._port_combo.get():
            self._port_combo.set(ports[0])

    def _set_connected_ui(self, connected: bool, text: str):
        self._connected = connected
        self._connect_btn.config(text="Desconectar" if connected else "Conectar",
                                 style="Danger.TButton" if connected else "Primary.TButton")
        self._led_canvas.itemconfig(self._led, fill=COLOR_CONNECTED if connected else COLOR_DISCONNECTED)
        self._status_var.set(text)

    def _toggle_connection(self):
        if self._worker.is_open:
            self._worker.disconnect()
            self._set_connected_ui(False, "Desconectado")
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
        self._avg_power = None
        self._set_connected_ui(True, f"{port} · {baud} bps")

    def _send(self, ch: str):
        if not self._worker.is_open:
            messagebox.showwarning("Sin conexión", "Conectate a un puerto serie primero.")
            return
        try:
            self._worker.send_char(ch)
        except serial.SerialException as exc:
            self._log(f"[ERROR] No se pudo enviar '{ch}': {exc}")

    def _on_close(self):
        self._worker.disconnect()
        self.destroy()

    def _toggle_pause(self):
        self._paused = not self._paused
        if self._paused:
            self._pause_btn.config(text="▶  Reanudar")
            self._paused_badge.pack(side=tk.RIGHT, pady=14)
        else:
            self._pause_btn.config(text="❚❚  Congelar")
            self._paused_badge.pack_forget()
            self._fft_dirty = self._last_fft_frame is not None
            self._bypass_dirty = self._last_bypass_frame is not None

    def _update_fft_legend(self):
        # Proxies: un artista invisible al crear la leyenda deja su muestra vacía
        handles = [Line2D([], [], color=ACCENT, linewidth=1.6, label="Espectro")]
        if self._max_hold_enabled.get():
            handles.append(Line2D([], [], color=COLOR_MAX_HOLD, linewidth=1.6, label="Max-Hold"))
        self._fft_ax.legend(handles=handles, loc="upper right")

    def _on_max_hold_toggle(self):
        if not self._max_hold_enabled.get():
            self._max_hold_db = None
            self._max_hold_line.set_visible(False)
        self._update_fft_legend()
        self._fft_dirty = self._last_fft_frame is not None
        self._fft_canvas.draw_idle()

    def _reset_max_hold(self):
        self._max_hold_db = None
        self._max_hold_line.set_visible(False)
        self._fft_canvas.draw_idle()

    def _reset_average(self):
        self._avg_power = None
        self._fft_dirty = self._last_fft_frame is not None

    def _averaging_n(self) -> int:
        try:
            return int(self._avg_var.get())
        except ValueError:
            return 1

    def _on_bypass_autoscale(self):
        self._bypass_axis_key = None  # forzar recálculo de límites
        self._bypass_dirty = self._last_bypass_frame is not None
        if not self._bypass_autoscale.get():
            self._bypass_ax.set_ylim(*BYPASS_YLIM)
            self._bypass_canvas.draw_idle()

    def _on_key_press(self, event):
        if isinstance(event.widget, (ttk.Combobox, ttk.Entry, tk.Entry, tk.Text)):
            return
        ch = event.char.lower()
        if ch and ch in COMMANDS.values():
            self._send(ch)
        elif event.keysym == "space":
            self._toggle_pause()

    # -------------------------------------------- interacción con gráficos
    def _full_freq_xlim(self, fs: float, n: int):
        if self._freq_scale_var.get() == FREQ_SCALES[1]:
            return fs / n, fs / 2.0  # en log no existe 0 Hz: arrancar en Δf
        return 0.0, fs / 2.0

    def _current_fs_n(self, ax):
        frame = self._last_fft_frame if ax is self._fft_ax else self._last_bypass_frame
        if frame is None:
            frame = self._last_frame
        if frame is None:
            return VALID_FS[-1], VALID_N[0]
        return frame.fs, frame.n

    def _apply_freq_scale(self):
        log = self._freq_scale_var.get() == FREQ_SCALES[1]
        for ax in (self._fft_ax, self._ref_ax):
            ax.set_xscale("log" if log else "linear")
            ax.xaxis.set_major_formatter(EngFormatter(unit="Hz"))
            ax.set_xlim(*self._full_freq_xlim(*self._current_fs_n(ax)))
        self._fft_canvas.draw_idle()
        self._bypass_canvas.draw_idle()

    def _on_scroll_zoom(self, event):
        ax = event.inaxes
        if ax not in (self._fft_ax, self._ref_ax) or event.xdata is None:
            return
        factor = ZOOM_STEP if event.button == "up" else 1.0 / ZOOM_STEP
        lo_lim, hi_lim = self._full_freq_xlim(*self._current_fs_n(ax))
        x0, x1 = ax.get_xlim()
        x = event.xdata
        if ax.get_xscale() == "log":
            lx0, lx1, lx = np.log10([x0, x1, x])
            nx0, nx1 = 10 ** (lx - (lx - lx0) * factor), 10 ** (lx + (lx1 - lx) * factor)
        else:
            nx0, nx1 = x - (x - x0) * factor, x + (x1 - x) * factor
        ax.set_xlim(max(nx0, lo_lim), min(nx1, hi_lim))
        event.canvas.draw_idle()

    def _on_plot_click(self, event):
        ax = event.inaxes
        if event.dblclick and ax in (self._fft_ax, self._ref_ax):
            ax.set_xlim(*self._full_freq_xlim(*self._current_fs_n(ax)))
            event.canvas.draw_idle()

    def _hide_cursor(self):
        self._cursor_vline.set_visible(False)
        self._cursor_dot.set_visible(False)
        self._cursor_info_var.set("Pasá el mouse sobre el espectro para medir")
        self._fft_canvas.draw_idle()

    def _on_fft_hover(self, event):
        if event.inaxes is not self._fft_ax or event.xdata is None:
            return
        frame = self._last_fft_frame
        if self._fft_display is None or frame is None:
            self._cursor_info_var.set(f"f = {event.xdata:9.1f} Hz   |   {event.ydata:6.1f} dBFS")
            return
        freqs, db = self._fft_display
        k = int(np.clip(round(event.xdata * frame.n / frame.fs), 0, len(freqs) - 1))
        f, m = freqs[k], db[k]
        self._cursor_vline.set_xdata([f, f])
        self._cursor_vline.set_visible(True)
        self._cursor_dot.set_data([f], [m])
        self._cursor_dot.set_visible(True)
        rel = m - frame.peak_db
        self._cursor_info_var.set(
            f"bin {k:4d}   f = {f:9.1f} Hz   |X| = {m:6.1f} dBFS   Δ al pico = {rel:+6.1f} dB")
        self._fft_canvas.draw_idle()

    # --------------------------------------------------------------- data
    def _poll_queue(self):
        try:
            while True:
                chunk = self._chunk_queue.get_nowait()
                for kind, payload in self._parser.feed(chunk):
                    self._handle_event(kind, payload)
        except queue.Empty:
            pass
        if self._connected and not self._worker.is_running:
            # El hilo lector murió (placa desenchufada, puerto cerrado, etc.)
            self._worker.disconnect()
            self._set_connected_ui(False, "Conexión perdida")
            self._log("[ERROR] Se perdió la conexión con el puerto serie.")
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
            # Actualiza la superposición de la placa en la pestaña de validación
            self._bypass_dirty = self._last_bypass_frame is not None
        else:
            self._last_bypass_frame = frame
            self._bypass_dirty = True

    def _log(self, line: str):
        tag = "normal"
        if "[ESTADO]" in line:
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
        self._log_count_var.set(f"{num_lines} líneas")
        if self._autoscroll_var.get():
            self._log_text.see(tk.END)
        self._log_text.config(state=tk.DISABLED)

    def _clear_log(self):
        self._log_text.config(state=tk.NORMAL)
        self._log_text.delete("1.0", tk.END)
        self._log_count_var.set("0 líneas")
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

    def _averaged_db(self, db: np.ndarray) -> np.ndarray:
        """Promedio exponencial en potencia (no en dB, que sesga hacia abajo)."""
        n_avg = self._averaging_n()
        if n_avg <= 1:
            self._avg_power = None
            return db
        power = 10.0 ** (db / 10.0)
        if self._avg_power is None or len(self._avg_power) != len(power):
            self._avg_power = power
        else:
            alpha = 1.0 / n_avg
            self._avg_power = (1.0 - alpha) * self._avg_power + alpha * power
        return 10.0 * np.log10(np.maximum(self._avg_power, 1e-20))

    def _redraw_fft(self):
        frame = self._last_fft_frame
        if frame is None:
            return
        freqs, raw_db = frame.freqs_hz(), frame.db()

        key = (frame.fs, frame.n)
        if key != self._fft_axis_key:
            self._fft_axis_key = key
            self._max_hold_db = None
            self._avg_power = None
            self._fft_ax.set_xlim(*self._full_freq_xlim(frame.fs, frame.n))
            self._fft_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
            self._fft_ax.set_title(
                f"Espectro de magnitud   ·   N = {frame.n}   ·   Fs = {frame.fs / 1000:g} kHz"
                f"   ·   Δf = {frame.fs / frame.n:.2f} Hz")

        db = self._averaged_db(raw_db)
        self._fft_display = (freqs, db)
        self._fft_line.set_data(freqs, db)

        # Relleno bajo la curva (se omite el bin 0 para que funcione en escala log)
        verts = np.empty((len(freqs) + 1, 2))
        verts[:-2, 0], verts[:-2, 1] = freqs[1:], db[1:]
        verts[-2] = (freqs[-1], -400.0)
        verts[-1] = (freqs[1], -400.0)
        self._fft_fill.set_xy(verts)
        self._fft_fill.set_visible(True)

        # Max-Hold sobre el espectro crudo (sin promediar)
        if self._max_hold_enabled.get():
            if self._max_hold_db is None or len(self._max_hold_db) != len(raw_db):
                self._max_hold_db = np.copy(raw_db)
            else:
                self._max_hold_db = np.maximum(self._max_hold_db, raw_db)
            self._max_hold_line.set_data(freqs, self._max_hold_db)
            self._max_hold_line.set_visible(True)
        else:
            self._max_hold_line.set_visible(False)

        # Marcador del pico (de la trama cruda, igual que la tarjeta KPI)
        if 0 < frame.peak_bin < frame.length:
            peak_freq, peak_db = frame.peak_freq_hz, frame.peak_db
            self._fft_peak_marker.set_xdata([peak_freq, peak_freq])
            self._fft_peak_marker.set_visible(True)
            self._fft_peak_dot.set_data([peak_freq], [peak_db])
            self._fft_peak_dot.set_visible(True)
            # Ubicar la etiqueta del lado con más espacio para que no se salga
            x0, x1 = self._fft_ax.get_xlim()
            if self._fft_ax.get_xscale() == "log":
                pos = (np.log10(peak_freq) - np.log10(x0)) / max(np.log10(x1) - np.log10(x0), 1e-9)
            else:
                pos = (peak_freq - x0) / max(x1 - x0, 1e-9)
            right_side = pos > 0.7
            self._fft_peak_text.xy = (peak_freq, peak_db)
            self._fft_peak_text.set_position((-10, -14) if right_side else (10, -14))
            self._fft_peak_text.set_ha("right" if right_side else "left")
            self._fft_peak_text.set_text(f"Pico  {peak_freq:.1f} Hz\n{peak_db:.1f} dBFS")
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
        self._bypass_placeholder.set_visible(False)
        t_ms = frame.time_ms()
        samples = frame.payload
        self._bypass_line.set_data(t_ms, samples)

        key = (frame.fs, frame.n)
        if key != self._bypass_axis_key:
            self._bypass_axis_key = key
            self._bypass_ax.set_xlim(0, t_ms[-1] if len(t_ms) > 1 else 1)
            self._ref_ax.set_xlim(*self._full_freq_xlim(frame.fs, frame.n))
            self._ref_ax.set_ylim(FFT_DB_MIN, FFT_DB_MAX)
            self._bypass_ax.set_ylim(*BYPASS_YLIM)

        if self._bypass_autoscale.get() and len(samples):
            peak = max(int(np.max(np.abs(samples.astype(np.int32)))), 256)
            self._bypass_ax.set_ylim(-1.15 * peak, 1.15 * peak)

        # Mediciones de la señal en el tiempo
        x = samples.astype(np.float64) / 32768.0
        if len(x):
            rms = float(np.sqrt(np.mean(x ** 2)))
            rms_db = 20.0 * np.log10(max(rms, 1e-12))
            vpp = int(samples.max()) - int(samples.min())
            dc = float(np.mean(samples))
            self._bypass_stats.set_text(
                f"RMS {rms_db:6.1f} dBFS   Vpp {vpp:6d}   DC {dc:+7.1f}")

        freqs, db = local_reference_spectrum(frame)
        if freqs is not None:
            self._ref_line.set_data(freqs, db)

        board = self._last_fft_frame
        if board is not None and board.fs == frame.fs:
            self._board_line.set_data(board.freqs_hz(), board.db())
            self._board_line.set_visible(True)
        else:
            self._board_line.set_visible(False)

        self._bypass_canvas.draw_idle()

    def _refresh_info_panel(self):
        frame = self._last_frame
        if frame is not None:
            df = frame.fs / frame.n
            tw = frame.n / frame.fs
            self._info_vars["fs"].set(f"{frame.fs / 1000:g} kHz")
            self._info_sub_vars["fs"].set(f"Nyquist {frame.fs / 2000:g} kHz")
            self._info_vars["n"].set(f"{frame.n}")
            self._info_sub_vars["n"].set(f"muestras · {frame.n // 2} bins")
            self._info_vars["df"].set(f"{df:.2f} Hz")
            self._info_sub_vars["df"].set("Fs / N")
            self._info_vars["tw"].set(f"{tw * 1000:.2f} ms")
            self._info_sub_vars["tw"].set("N / Fs")
            self._frame_info_var.set(
                f"Última trama: {frame.type_name} · N={frame.n} · Fs={frame.fs} Hz · "
                f"ventana {frame.window_name.lower()}")

        fft = self._last_fft_frame
        if fft is not None and 0 < fft.peak_bin < fft.length:
            self._info_vars["peak"].set(f"{fft.peak_freq_hz:.1f} Hz")
            self._info_sub_vars["peak"].set(f"{fft.peak_db:.1f} dBFS · bin {fft.peak_bin}")
            floor = float(np.median(fft.db()[1:]))
            self._info_vars["floor"].set(f"{floor:.1f} dBFS")
            self._info_sub_vars["floor"].set(f"mediana · SNR≈{fft.peak_db - floor:.1f} dB")

        now = time.monotonic()
        while self._frame_times and now - self._frame_times[0] > RATE_WINDOW_S:
            self._frame_times.popleft()
        self._info_vars["rate"].set(f"{len(self._frame_times)}/s")
        self._info_sub_vars["rate"].set("tramas por segundo")
        self._info_vars["total"].set(f"{self._total_frames}")
        self._info_sub_vars["total"].set("desde la conexión")

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
        fig.savefig(png_path, dpi=200, facecolor=fig.get_facecolor(), edgecolor="none",
                    bbox_inches="tight")

        self._log(f"[EXPORT] CSV: {csv_path.name}")
        self._log(f"[EXPORT] PNG: {png_path.name}")


if __name__ == "__main__":
    app = FftGuiApp()
    app.mainloop()
