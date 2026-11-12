# Laboratorio #3: Implementación de la FFT en FRDM-MCXN947

## Objetivo
Implementar la Transformada Rápida de Fourier (FFT) para ventanas de $N \in \{512, 1024, 2048\}$ muestras en la placa de desarrollo **FRDM-MCXN947** (Cortex-M33 con FPU / DSP), analizando el compromiso entre frecuencia de muestreo $f_s$, resolución en frecuencia $\Delta f$, tiempo de ventana $T_w$ y costo computacional.

---

## Estructura del Directorio

```text
TP3/
├── MCXN947_Project_TP3/       # Proyecto MCUXpresso (Cortex-M33 / SDK)
│   ├── source/               # Código fuente del firmware
│   │   ├── MCXN947_Project_TP3.c
│   │   ├── adc_stage.c / .h
│   │   ├── pipeline.c / .h
│   │   ├── processing_stage.c / .h
│   │   ├── uart_stage.c / .h
│   │   ├── ui.c / .h
│   │   └── display_port.c / .h
│   └── ...
├── tools/                    # Aplicación de PC para visualización y control
│   ├── fft_gui.py            # GUI en Python (matplotlib, pyserial)
│   └── requirements.txt      # Dependencias (pyserial, matplotlib, numpy)
├── images/                   # Capturas de pantalla, espectros y mediciones
├── tests/                    # Scripts de prueba y validación matemática
├── informe.md                # Informe técnico final
└── README.md
```

---

## Hoja de Ruta e Issues Relacionadas

El desarrollo se organiza según los issues de GitHub:

1. **[Issue #14](https://github.com/wandamol1405/DSP---TPs/issues/14) - Base del proyecto y adquisición por ventanas de N muestras**:
   - Duplicación y renombrado del proyecto a `MCXN947_Project_TP3`.
   - Adquisición continua por DMA/LPADC + CTIMER a $f_s \in \{8, 16, 22, 44, 48\}\text{ kHz}$.
   - Manejo de ventanas $N \in \{512, 1024, 2048\}$ con doble buffer (`s_frame_in[2][2048]`) y procesamiento diferido en PendSV.
   - Conmutación limpia de $f_s$ y $N$ entre ventanas consecutivas.

2. **[Issue #15](https://github.com/wandamol1405/DSP---TPs/issues/15) - Implementación de la FFT con CMSIS-DSP y modo Bypass**:
   - Alternancia entre modo **FFT** y modo **Bypass** (buffer de salida independiente del de entrada).
   - Cálculo de la FFT en punto fijo $q_{15}$, como el resto del pipeline: remoción de la media, enventanado (Rectangular / Hann, tablas $q_{15}$), `arm_rfft_q15`, magnitud con `arm_cmplx_mag_q15` y búsqueda del pico con `arm_max_q15`. La conversión a dBFS se hace en la PC.
   - Medición de ciclos de CPU mediante DWT (`CYCCNT`).

3. **[Issue #16](https://github.com/wandamol1405/DSP---TPs/issues/16) - Transmisión del resultado por UART**:
   - Transmisión de tramas binarias (`0xA5 0x5A`) con el buffer de salida (FFT o Bypass).
   - Envío desacoplado de la última salida disponible para no saturar el canal serie.

4. **[Issue #17](https://github.com/wandamol1405/DSP---TPs/issues/17) - GUI en PC para graficar el espectro y ensayos**:
   - Interfaz gráfica en Python (`fft_gui.py`) para recepción en tiempo real, calibración de ejes ($t$ en ms, $f$ en Hz/dBFS) y detección de picos.
   - Ensayos comparativos de variación de $N$ y $f_s$, fuga espectral (*spectral leakage*) y resolución.

---

## Formato de Trama UART (Binario Little-Endian)

Protocolo mínimo: lo que pide la consigna es transmitir el buffer de salida
(FFT o Bypass) y graficarlo en la PC. No se agregó CRC, número de secuencia
ni bin de pico transmitido (la GUI lo calcula del lado de la PC) — son
robustez/telemetría extra que la consigna no pide; si hace falta más
adelante, se agregan sin romper lo que ya hay (quedan como campos nuevos al
final de la trama).

| Campo | Tipo | Tamaño | Descripción |
| :--- | :--- | :--- | :--- |
| **Sync** | uint16 | 2 B | `0xA5 0x5A` |
| **Type** | uint8 | 1 B | `0` = FFT (dB × 100), `1` = Bypass ($q_{15}$) |
| **N** | uint16 | 2 B | Tamaño de ventana ($512, 1024, 2048$) |
| **Fs (Hz)** | uint32 | 4 B | Frecuencia de muestreo |
| **Payload** | int16[Length] | $2 \times \text{Length}$ B | Datos de salida. Length = N/2 (FFT) o N (Bypass) — no se transmite, se deriva de Type y N |
