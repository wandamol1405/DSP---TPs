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
   - Cálculo de la FFT mediante `arm_rfft_fast_f32` (aprovechando FPU) con enventanado (Rectangular / Hann) y magnitud en dBFS (`arm_cmplx_mag_f32` + `log10`).
   - Medición de ciclos de CPU mediante DWT (`CYCCNT`).

3. **[Issue #16](https://github.com/wandamol1405/DSP---TPs/issues/16) - Transmisión del resultado por UART**:
   - Transmisión no bloqueante de tramas binarias (`0xA5 0x5A`) con CRC-16/CCITT.
   - Envío desacoplado de la última salida disponible para no saturar el canal serie.

4. **[Issue #17](https://github.com/wandamol1405/DSP---TPs/issues/17) - GUI en PC para graficar el espectro y ensayos**:
   - Interfaz gráfica en Python (`fft_gui.py`) para recepción en tiempo real, calibración de ejes ($t$ en ms, $f$ en Hz/dBFS) y detección de picos.
   - Ensayos comparativos de variación de $N$ y $f_s$, fuga espectral (*spectral leakage*) y resolución.

---

## Formato de Trama UART (Binario Little-Endian)

| Campo | Tipo | Tamaño | Descripción |
| :--- | :--- | :--- | :--- |
| **Sync** | uint16 | 2 B | `0xA5 0x5A` |
| **Type** | uint8 | 1 B | `0` = FFT (dB × 100), `1` = Bypass ($q_{15}$) |
| **Window** | uint8 | 1 B | `0` = Rectangular, `1` = Hann |
| **N** | uint16 | 2 B | Tamaño de ventana ($512, 1024, 2048$) |
| **Length** | uint16 | 2 B | Cantidad de puntos en el payload ($N/2$ para FFT, $N$ para Bypass) |
| **Fs (Hz)** | uint32 | 4 B | Frecuencia de muestreo |
| **Seq** | uint32 | 4 B | Número secuencial de ventana |
| **Peak Bin** | uint16 | 2 B | Bin del armónico fundamental (0 en Bypass) |
| **Payload** | int16[Length] | $2 \times \text{Length}$ B | Datos de salida |
| **CRC** | uint16 | 2 B | CRC-16/CCITT sobre todos los campos previos (excluyendo Sync) |
