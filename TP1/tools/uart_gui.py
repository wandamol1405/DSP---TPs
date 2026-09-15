#!/usr/bin/env python3
"""
GUI de control y visualización para el TP1 DSP (FRDM-MCXN947).

Se conecta al puerto serie (LPUART4 / debug console, 115200 baud) del proyecto
MCXN947_Project_TP1, permite enviar los comandos definidos en
ProcessUartCommands() (source/MCXN947_Project_TP1.c) y grafica:
  - El streaming en tiempo real habilitado con el comando 'p' (entrada,salida).
    Es solo para inspección visual rápida: uart_stage.c decima por 32 sin
    filtro anti-aliasing, así que no es apto para medir frecuencia.
  - Un "osciloscopio" armado sobre el volcado de 512 muestras del buffer
    circular ('d'): sin decimar, a la frecuencia real del ADC. Con captura
    automática periódica y disparo (trigger) por cruce ascendente para que
    la forma de onda se vea estable, como en un osciloscopio real.
  - El espectro (FFT) y la estimación de la frecuencia fundamental, siempre
    calculados a partir de ese mismo volcado (nunca del streaming decimado,
    que aliasa cualquier señal por encima de fs_ADC/64).

Requisitos: pyserial, matplotlib, numpy (Tkinter suele venir con Python; en
Linux puede requerir el paquete del sistema "python3-tk").

Instalación:
    pip install -r requirements.txt

Uso:
    python3 uart_gui.py
"""

import queue
import re
import threading
import tkinter as tk
from collections import deque
from tkinter import messagebox, ttk

import numpy as np
import serial
import serial.tools.list_ports
from matplotlib import style as mpl_style
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

mpl_style.use("dark_background")

BAUD_RATE_DEFAULT = 115200
LOG_MAX_LINES = 500
STREAM_WINDOW = 1000          # muestras visibles en el gráfico de streaming
DUMP_BUFFER_SIZE = 512        # PIPELINE_BUFFER_SIZE en pipeline.h
DEFAULT_SAMPLE_RATE_HZ = 8000
PLOT_REFRESH_MS = 50          # ~20 Hz de refresco de gráficos
QUEUE_POLL_MS = 20
AUTO_CAPTURE_INTERVAL_MS = 400   # período del "osciloscopio" (envía 'd' solo)
SCOPE_CYCLES_TO_SHOW = 4         # ciclos de la fundamental a mostrar, tipo time/div
MIN_SCOPE_SAMPLES = 32           # piso para no mostrar ventanas degeneradas

DUMP_START_RE = re.compile(r"--- INICIO BUFFER DUMP")
DUMP_END_RE = re.compile(r"--- FIN BUFFER DUMP")
STREAM_SAMPLE_RE = re.compile(r"^-?\d+,-?\d+$")
INFO_FREQ_RE = re.compile(r"\[INFO\] Frecuencia de muestreo:\s*(\d+)\s*Hz")
DSP_MODE_RE = re.compile(r"\[DSP\] Modo de procesamiento:\s*(\S+)")
RUN_RE = re.compile(r"\[RUN\]")
STOP_RE = re.compile(r"\[STOP\]")

# Comandos válidos según ProcessUartCommands() en MCXN947_Project_TP1.c
COMMANDS = {
    "RUN / STOP": "r",
    "Cambiar frecuencia": "f",
    "Volcar buffer (512 muestras)": "d",
    "Streaming Serial Plotter": "p",
    "Modo de procesamiento DSP": "m",
    "Ayuda": "h",
}

# --- Paleta dark mode ---
DARK_BG = "#1e1e1e"
DARK_BG_ALT = "#2a2d2e"
DARK_FG = "#e6e6e6"
DARK_ACCENT = "#3a7ca5"


def estimate_spectrum(samples, fs):
    """Calcula el espectro de magnitud (dB) y estima la frecuencia fundamental.

    Devuelve (freqs_hz, mag_db, fundamental_hz). fundamental_hz es None si no
    se puede estimar (muy pocas muestras, señal nula, fs inválida, etc.).
    """
    n = len(samples)
    if n < 8 or fs is None or fs <= 0:
        return None, None, None

    x = np.asarray(samples, dtype=np.float64)
    x = x - np.mean(x)
    if not np.any(x):
        return None, None, None

    window = np.hanning(n)
    spectrum = np.fft.rfft(x * window)
    mag = np.abs(spectrum)
    freqs = np.fft.rfftfreq(n, d=1.0 / fs)
    mag_db = 20.0 * np.log10(mag + 1e-9)

    if len(mag) < 3:
        return freqs, mag_db, None

    # Pico de magnitud ignorando la continua (bin 0)
    peak_bin = 1 + int(np.argmax(mag[1:]))

    if 0 < peak_bin < len(mag) - 1:
        # Interpolación parabólica para afinar la estimación entre bins
        alpha, beta, gamma = mag[peak_bin - 1], mag[peak_bin], mag[peak_bin + 1]
        denom = alpha - 2.0 * beta + gamma
        p = 0.5 * (alpha - gamma) / denom if denom != 0 else 0.0
        fundamental = (peak_bin + p) * fs / n
    else:
        fundamental = freqs[peak_bin]

    return freqs, mag_db, fundamental


def prepare_scope_view(values, fs, fundamental):
    """Recorta y alinea el buffer para que se vea como un osciloscopio.

    - Si se conoce la fundamental, muestra ~SCOPE_CYCLES_TO_SHOW ciclos en
      vez del buffer completo (evita ver 0.3 ciclos a baja frecuencia o 40
      ciclos amontonados a alta frecuencia).
    - Busca el primer cruce ascendente por la media dentro del rango que
      todavía deja suficientes muestras después (disparo/trigger), para que
      la fase se vea estable entre capturas sucesivas en vez de "saltar".

    Devuelve (tiempos_ms, valores) ya recortados y alineados.
    """
    n = len(values)
    if n == 0 or fs is None or fs <= 0:
        return [], []

    if fundamental and fundamental > 0:
        samples_per_cycle = fs / fundamental
        display_len = int(min(n, max(MIN_SCOPE_SAMPLES, SCOPE_CYCLES_TO_SHOW * samples_per_cycle)))
    else:
        display_len = n

    arr = np.asarray(values, dtype=np.float64)
    mean = arr.mean()

    trigger_idx = 0
    search_end = max(1, n - display_len)
    for i in range(1, search_end):
        if arr[i - 1] < mean <= arr[i]:
            trigger_idx = i
            break

    window = values[trigger_idx: trigger_idx + display_len]
    t_ms = [i / fs * 1000.0 for i in range(len(window))]
    return t_ms, window


class SerialWorker:
    """Lee el puerto serie en un hilo aparte y expone una cola de líneas."""

    def __init__(self, line_queue: queue.Queue):
        self._line_queue = line_queue
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
        buffer = b""
        while not self._stop_event.is_set():
            try:
                chunk = self._ser.read(256)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                raw_line, buffer = buffer.split(b"\n", 1)
                line = raw_line.decode("ascii", errors="replace").rstrip("\r")
                self._line_queue.put(line)


class UartGuiApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TP1 DSP - Control UART (FRDM-MCXN947)")
        self.geometry("1050x760")

        self._line_queue: queue.Queue[str] = queue.Queue()
        self._worker = SerialWorker(self._line_queue)

        self._stream_x = deque(maxlen=STREAM_WINDOW)
        self._stream_in = deque(maxlen=STREAM_WINDOW)
        self._stream_out = deque(maxlen=STREAM_WINDOW)
        self._sample_counter = 0
        self._stream_dirty = False

        self._in_dump = False
        self._dump_chunks: list[str] = []
        self._last_dump_values: list[int] | None = None

        # Resultados ya calculados sobre el último volcado, listos para dibujar
        self._scope_dirty = False
        self._scope_view = ([], [])           # (t_ms, valores)
        self._spectrum_data = (None, None, None)  # (freqs, mag_db, fundamental)

        self._sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ

        self._q15_float = tk.BooleanVar(value=False)
        self._auto_capture = tk.BooleanVar(value=True)

        self._apply_dark_theme()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(QUEUE_POLL_MS, self._poll_queue)
        self.after(PLOT_REFRESH_MS, self._refresh_plots)
        self.after(AUTO_CAPTURE_INTERVAL_MS, self._auto_capture_tick)

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
        style.configure("TRadiobutton", background=DARK_BG, foreground=DARK_FG)
        style.map("TRadiobutton", background=[("active", DARK_BG)])
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
        self._port_combo = ttk.Combobox(top, width=20, state="readonly")
        self._port_combo.pack(side=tk.LEFT, padx=4)
        self._refresh_ports()

        ttk.Button(top, text="Refrescar", command=self._refresh_ports).pack(side=tk.LEFT, padx=4)

        ttk.Label(top, text="Baud:").pack(side=tk.LEFT, padx=(12, 0))
        self._baud_entry = ttk.Entry(top, width=8)
        self._baud_entry.insert(0, str(BAUD_RATE_DEFAULT))
        self._baud_entry.pack(side=tk.LEFT, padx=4)

        self._connect_btn = ttk.Button(top, text="Conectar", command=self._toggle_connection)
        self._connect_btn.pack(side=tk.LEFT, padx=12)

        self._status_var = tk.StringVar(value="Desconectado")
        ttk.Label(top, textvariable=self._status_var).pack(side=tk.LEFT, padx=8)

        # --- Comandos ---
        cmd_frame = ttk.LabelFrame(self, text="Comandos", padding=8)
        cmd_frame.pack(side=tk.TOP, fill=tk.X, padx=8, pady=4)
        for label, ch in COMMANDS.items():
            ttk.Button(
                cmd_frame, text=f"{label} ({ch})", command=lambda c=ch: self._send(c)
            ).pack(side=tk.LEFT, padx=4, pady=4)

        ttk.Checkbutton(
            cmd_frame, text="Mostrar como float Q15 (-1..1)", variable=self._q15_float
        ).pack(side=tk.RIGHT, padx=8)

        # --- Estado ---
        state_frame = ttk.Frame(self, padding=(8, 0))
        state_frame.pack(side=tk.TOP, fill=tk.X)
        self._freq_var = tk.StringVar(value="Frecuencia de muestreo: ?")
        self._mode_var = tk.StringVar(value="Modo DSP: ?")
        self._run_var = tk.StringVar(value="Estado: ?")
        self._fundamental_var = tk.StringVar(value="F. fundamental: -- Hz")
        for var in (self._run_var, self._freq_var, self._mode_var, self._fundamental_var):
            ttk.Label(state_frame, textvariable=var, relief=tk.SUNKEN, padding=4).pack(
                side=tk.LEFT, padx=4, pady=4, fill=tk.X, expand=True
            )

        # --- Notebook: gráficos + log ---
        notebook = ttk.Notebook(self)
        notebook.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=8, pady=4)

        self._build_stream_tab(notebook)
        self._build_dump_tab(notebook)
        self._build_spectrum_tab(notebook)
        self._build_log_tab(notebook)

    def _build_stream_tab(self, notebook):
        stream_tab = ttk.Frame(notebook)
        notebook.add(stream_tab, text="Streaming en vivo (p)")
        ttk.Label(
            stream_tab,
            text=("Solo inspección visual: decimado por 32 sin filtro anti-aliasing "
                  "(uart_stage.c). No se usa para medir frecuencia — ver pestaña Osciloscopio."),
            foreground="#c9a227", padding=(4, 4),
        ).pack(side=tk.TOP, fill=tk.X)
        self._stream_fig = Figure(figsize=(5, 3), dpi=100)
        self._stream_ax = self._stream_fig.add_subplot(111)
        self._stream_ax.set_title("Entrada vs Salida (Serial Plotter)")
        self._stream_ax.set_xlabel("Muestra")
        self._stream_ax.set_ylabel("Valor")
        (self._stream_line_in,) = self._stream_ax.plot([], [], label="Entrada", linewidth=0.8)
        (self._stream_line_out,) = self._stream_ax.plot([], [], label="Salida", linewidth=0.8)
        self._stream_ax.legend(loc="upper right")
        self._stream_fig.tight_layout()
        self._stream_canvas = FigureCanvasTkAgg(self._stream_fig, master=stream_tab)
        self._stream_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_dump_tab(self, notebook):
        dump_tab = ttk.Frame(notebook)
        notebook.add(dump_tab, text="Osciloscopio (buffer)")

        controls = ttk.Frame(dump_tab, padding=(0, 4))
        controls.pack(side=tk.TOP, fill=tk.X)
        ttk.Checkbutton(
            controls, text=f"Captura automática (cada {AUTO_CAPTURE_INTERVAL_MS} ms, envía 'd')",
            variable=self._auto_capture,
        ).pack(side=tk.LEFT, padx=4)
        ttk.Label(
            controls, text="Sin decimar, a la frecuencia real del ADC · con disparo por cruce ascendente",
        ).pack(side=tk.LEFT, padx=12)

        self._dump_fig = Figure(figsize=(5, 3), dpi=100)
        self._dump_ax = self._dump_fig.add_subplot(111)
        self._dump_ax.set_title("Forma de onda (último volcado, alineada por trigger)")
        self._dump_ax.set_xlabel("Tiempo (ms)")
        self._dump_ax.set_ylabel("Valor")
        self._dump_ax.axhline(0, color="#555555", linewidth=0.6)
        (self._dump_line,) = self._dump_ax.plot([], [], linewidth=1.0)
        self._dump_fig.tight_layout()
        self._dump_canvas = FigureCanvasTkAgg(self._dump_fig, master=dump_tab)
        self._dump_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_spectrum_tab(self, notebook):
        spectrum_tab = ttk.Frame(notebook)
        notebook.add(spectrum_tab, text="Espectro (FFT)")

        ttk.Label(
            spectrum_tab,
            text="Calculado siempre sobre el último volcado de buffer (fs completa, sin decimar).",
            padding=(4, 4),
        ).pack(side=tk.TOP, fill=tk.X)

        self._spectrum_fig = Figure(figsize=(5, 3), dpi=100)
        self._spectrum_ax = self._spectrum_fig.add_subplot(111)
        self._spectrum_ax.set_title("Espectro de magnitud")
        self._spectrum_ax.set_xlabel("Frecuencia (Hz)")
        self._spectrum_ax.set_ylabel("Magnitud (dB)")
        (self._spectrum_line,) = self._spectrum_ax.plot([], [], linewidth=0.9)
        self._fundamental_marker = self._spectrum_ax.axvline(
            0, color=DARK_ACCENT, linestyle="--", linewidth=1.0, visible=False
        )
        self._spectrum_fig.tight_layout()
        self._spectrum_canvas = FigureCanvasTkAgg(self._spectrum_fig, master=spectrum_tab)
        self._spectrum_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_log_tab(self, notebook):
        log_tab = ttk.Frame(notebook)
        notebook.add(log_tab, text="Log de consola")
        self._log_text = tk.Text(
            log_tab, wrap=tk.NONE, state=tk.DISABLED,
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
            baud = int(self._baud_entry.get())
        except ValueError:
            messagebox.showerror("Error", "Baud rate inválido.")
            return

        try:
            self._worker.connect(port, baud)
        except serial.SerialException as exc:
            messagebox.showerror("Error al conectar", str(exc))
            return

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
                line = self._line_queue.get_nowait()
                self._handle_line(line)
        except queue.Empty:
            pass
        self.after(QUEUE_POLL_MS, self._poll_queue)

    def _handle_line(self, line: str):
        if DUMP_START_RE.search(line):
            self._in_dump = True
            self._dump_chunks = []
            self._log(line)
            return

        if DUMP_END_RE.search(line):
            self._in_dump = False
            self._log(line)
            self._parse_dump()
            return

        if self._in_dump:
            self._dump_chunks.append(line)
            return

        if STREAM_SAMPLE_RE.match(line):
            in_s, out_s = line.split(",")
            self._sample_counter += 1
            self._stream_x.append(self._sample_counter)
            self._stream_in.append(int(in_s))
            self._stream_out.append(int(out_s))
            self._stream_dirty = True
            return

        # Cualquier otra línea: log + parseo de estado
        self._log(line)

        m = INFO_FREQ_RE.search(line)
        if m:
            self._sample_rate_hz = int(m.group(1))
            self._freq_var.set(f"Frecuencia de muestreo: {self._sample_rate_hz} Hz")

        m = DSP_MODE_RE.search(line)
        if m:
            self._mode_var.set(f"Modo DSP: {m.group(1)}")

        if RUN_RE.search(line):
            self._run_var.set("Estado: RUN")
        elif STOP_RE.search(line):
            self._run_var.set("Estado: STOP")

    def _parse_dump(self):
        joined = ",".join(self._dump_chunks)
        values = []
        for tok in joined.split(","):
            tok = tok.strip()
            if not tok:
                continue
            try:
                values.append(int(tok))
            except ValueError:
                continue
        if not values:
            return
        self._last_dump_values = values

        freqs, mag_db, fundamental = estimate_spectrum(values, self._sample_rate_hz)
        self._spectrum_data = (freqs, mag_db, fundamental)
        self._scope_view = prepare_scope_view(values, self._sample_rate_hz, fundamental)
        self._scope_dirty = True

    def _auto_capture_tick(self):
        if self._auto_capture.get() and self._worker.is_open and not self._in_dump:
            self._worker.send_char("d")
        self.after(AUTO_CAPTURE_INTERVAL_MS, self._auto_capture_tick)

    def _log(self, line: str):
        self._log_text.config(state=tk.NORMAL)
        self._log_text.insert(tk.END, line + "\n")
        num_lines = int(self._log_text.index("end-1c").split(".")[0])
        if num_lines > LOG_MAX_LINES:
            self._log_text.delete("1.0", f"{num_lines - LOG_MAX_LINES}.0")
        self._log_text.see(tk.END)
        self._log_text.config(state=tk.DISABLED)

    # -------------------------------------------------------------- plots
    def _scale(self, values):
        if self._q15_float.get():
            return [v / 32768.0 for v in values]
        return values

    def _refresh_plots(self):
        if self._stream_dirty:
            self._stream_dirty = False
            xs = list(self._stream_x)
            self._stream_line_in.set_data(xs, self._scale(list(self._stream_in)))
            self._stream_line_out.set_data(xs, self._scale(list(self._stream_out)))
            if xs:
                self._stream_ax.set_xlim(xs[0], xs[-1])
                ylim = (-1.05, 1.05) if self._q15_float.get() else (-33000, 33000)
                self._stream_ax.set_ylim(*ylim)
            self._stream_canvas.draw_idle()

        if self._scope_dirty:
            self._scope_dirty = False
            self._redraw_scope()
            self._redraw_spectrum()

        self.after(PLOT_REFRESH_MS, self._refresh_plots)

    def _redraw_scope(self):
        t_ms, values = self._scope_view
        if not values:
            return
        scaled = self._scale(values)
        self._dump_line.set_data(t_ms, scaled)
        self._dump_ax.set_xlim(0, t_ms[-1] if len(t_ms) > 1 else 1)
        ylim = (-1.05, 1.05) if self._q15_float.get() else (-33000, 33000)
        self._dump_ax.set_ylim(*ylim)
        self._dump_canvas.draw_idle()

    def _redraw_spectrum(self):
        freqs, mag_db, fundamental = self._spectrum_data
        if freqs is None:
            return

        self._spectrum_line.set_data(freqs, mag_db)
        self._spectrum_ax.set_xlim(0, freqs[-1] if len(freqs) else 1)
        finite = mag_db[np.isfinite(mag_db)]
        if finite.size:
            self._spectrum_ax.set_ylim(finite.min() - 5, finite.max() + 5)

        if fundamental is not None:
            self._fundamental_marker.set_xdata([fundamental, fundamental])
            self._fundamental_marker.set_visible(True)
            self._fundamental_var.set(f"F. fundamental: {fundamental:.1f} Hz")
        else:
            self._fundamental_marker.set_visible(False)
            self._fundamental_var.set("F. fundamental: -- Hz")

        self._spectrum_canvas.draw_idle()


if __name__ == "__main__":
    app = UartGuiApp()
    app.mainloop()
