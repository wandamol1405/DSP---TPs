# TP3 FFT – Decisiones de diseño no especificadas en la consigna

La consigna dice qué hay que hacer. Este documento registra cómo lo resolvió el equipo en los puntos que la consigna deja abiertos, y sirve de base para la sección de diseño del informe.

## Contexto

Lo que la consigna sí pide:

- FFT sobre buffers de distinto tamaño (512, 1024 y 2048 muestras) adquiridos con el Laboratorio #1, con los cambios de frecuencia de muestreo.
- Una tecla de la placa habilita la FFT o hace bypass hacia un buffer de salida distinto del de entrada.
- Transmitir el resultado por el puerto serie y graficar el espectro en la PC.
- Sacar conclusiones sobre la FFT al cambiar la frecuencia de muestreo o el tamaño de ventana.

Todo lo demás no está especificado: cómo se adquiere la ventana, qué pasa al cambiar Fs o N a mitad de una ventana, el formato de la trama, qué tecla hace qué y en qué modo arranca. Cada decisión indica quién la tomó y en qué rama o archivo quedó.

## Cómo funciona

### Flujo de datos

```
                       ┌──────────────────────────────► DAC ──► osciloscopio
                       │   (cada muestra, en cualquier modo)
 generador ──► ADC ────┤
  (señal)     (Fs)     │   ISR del ADC                     PendSV (menor prioridad)
                       └─► ventana de entrada ──────────► FFT o BYPASS ──► buffer de salida ──► UART ──► PC
                           (N muestras q15,                (según el modo       (N/2 valores en dB × 100
                            ping-pong de 2 buffers)         de esa ventana)      o N muestras q15)
```

- **ADC (ISR, una vez por muestra):** lee la muestra, la repite por el DAC y la guarda en la ventana que se está llenando. Al completar N muestras, deja la ventana lista y dispara PendSV.
- **PendSV:** procesa la ventana lista mientras la ISR llena la otra.
  - **FFT:** `arm_cfft_q15` y `arm_cmplx_mag_q15`, y después la conversión a dB.
  - **BYPASS:** copia la ventana al buffer de salida sin procesarla.
- **UART:** PendSV manda el buffer de salida con la trama binaria de Wanda: con `c`, en continuo, cada ventana que encuentre la UART libre; con `s`, una sola. Como salida de texto, por ahora sigue el streaming de muestras del TP2 (`p`).
- **DAC:** no lo pide la consigna. Se mantiene para ver en el osciloscopio la señal tal como se muestreó y para las fotos de formas de onda.

### Ping-pong de ventanas

El ADC no para nunca: a 8 kHz llega una muestra cada 125 µs, también mientras se calcula la FFT. Con dos buffers (A y B), mientras uno se llena el otro se procesa, y después cambian de rol. Así la FFT lee un buffer que nadie está escribiendo, y no se pierde ninguna muestra entre ventanas.

Ejemplo con N = 1024 y Fs = 8 kHz (cada ventana tarda 128 ms en llenarse):

```
tiempo →     0 ms          128 ms          256 ms          384 ms
             │               │               │               │
ADC escribe: │ ── en A ────► │ ── en B ────► │ ── en A ────► │ ── en B ──►
             │               │               │               │
FFT lee:     │  (nada aún)   │ A ■■          │ B ■■          │ A ■■
                               ↑               ↑
                               la FFT de A     la FFT de B
                               tarda pocos ms  tarda pocos ms
```

1. A los 128 ms se llena A. Se le avisa a PendSV que lo procese y, en la misma muestra, el ADC pasa a escribir en B.
2. Mientras se llena B, la FFT lee A sin que nadie lo toque.
3. A los 256 ms se llena B. Se procesa B y el ADC vuelve a A, que ya se terminó de usar.

La única condición es que la FFT de un buffer termine antes de que se llene el otro, es decir, en menos de T = N/Fs (ver la tabla de "Qué cambia con Fs y N"). Si no llega, el ADC no puede volver al buffer que la FFT todavía está leyendo: la ventana nueva se descarta y se cuenta como sobrecarga (`[DSP] Sobrecarga ... ventana descartada` por consola).

En el código (`pipeline.c`): `s_frame_in[2][2048]` son A y B, `s_cur` indica en cuál escribe el ADC, `s_frame_ready` cuál procesa PendSV, `s_cur = cur ^ 1U` hace el cambio (0 ↔ 1) y `s_frame_busy` está en true mientras la FFT sigue usando su buffer.

**Alternativa más simple, descartada:** con un solo buffer también funciona. Se llena, se calcula la FFT y recién después se llena de nuevo, descartando las muestras que llegan mientras tanto. Queda un hueco de señal entre ventanas, que para un tono estable no cambia el espectro. Igual se eligió el ping-pong porque ya estaba implementado (es el mismo patrón del TP2) y porque permite la comparación del tiempo de FFT contra T = N/Fs, que es parte de las conclusiones sobre el cambio de Fs y N.

### Al encender

| Parámetro | Valor inicial |
| --- | --- |
| Adquisición | RUN |
| Fs | 8 kHz (LED rojo) |
| Modo | BYPASS |
| N | 1024 (ver Pendientes de decidir) |

### SW3: modo y tamaño de ventana

Cada vez que se presiona SW3, o se manda `m` por UART, se avanza un paso del ciclo:

```
               ┌─────────────────────────── SW3 ────────────────────────────┐
               ▼                                                             │
 encendido ─► BYPASS ──SW3──► FFT 512 ──SW3──► FFT 1024 ──SW3──► FFT 2048 ──┘
```

| Pulsación | Modo | N | Qué sale por UART |
| --- | --- | --- | --- |
| (al encender) | BYPASS | 1024 | 1024 muestras q15 |
| 1.ª | FFT | 512 | 256 bins en dB × 100 |
| 2.ª | FFT | 1024 | 512 bins |
| 3.ª | FFT | 2048 | 1024 bins |
| 4.ª | BYPASS | 2048 | 2048 muestras q15 |
| 5.ª | FFT | 512 | 256 bins (y sigue el ciclo) |

En BYPASS se mantiene el N anterior: 1024 al encender y 2048 cuando se vuelve desde FFT 2048. El cambio no es inmediato: la ventana que se está llenando termina con el modo y el N con que empezó, y lo nuevo se aplica desde la ventana siguiente.

### SW2: frecuencia de muestreo

Cada vez que se presiona SW2, o se manda `f`, se pasa a la frecuencia siguiente y el LED cambia de color:

| Fs | LED |
| --- | --- |
| 8 kHz | rojo |
| 16 kHz | verde |
| 22 kHz | azul |
| 44 kHz | amarillo (rojo + verde) |
| 48 kHz | magenta (rojo + azul) |

Después de 48 kHz vuelve a 8 kHz. Al cambiar Fs se descarta la ventana a medio llenar, y la próxima arranca de cero con la frecuencia nueva.

### Qué cambia con Fs y N

La resolución en frecuencia es Δf = Fs / N y la duración de la ventana es T = N / Fs. T es también el tiempo que tiene PendSV para procesar cada ventana.

| Fs | Δf con N = 512 | Δf con 1024 | Δf con 2048 | T con N = 512 | T con 1024 | T con 2048 |
| --- | --- | --- | --- | --- | --- | --- |
| 8 kHz | 15,6 Hz | 7,8 Hz | 3,9 Hz | 64 ms | 128 ms | 256 ms |
| 16 kHz | 31,3 Hz | 15,6 Hz | 7,8 Hz | 32 ms | 64 ms | 128 ms |
| 22 kHz | 43,0 Hz | 21,5 Hz | 10,7 Hz | 23,3 ms | 46,5 ms | 93,1 ms |
| 44 kHz | 85,9 Hz | 43,0 Hz | 21,5 Hz | 11,6 ms | 23,3 ms | 46,5 ms |
| 48 kHz | 93,8 Hz | 46,9 Hz | 23,4 Hz | 10,7 ms | 21,3 ms | 42,7 ms |

El espectro muestra los bins de 0 a Fs/2. Una señal por encima de Fs/2 aparece reflejada (aliasing): por ejemplo, un tono de 5 kHz a Fs = 8 kHz se ve en 3 kHz.

## Decisiones de Wanda

Wanda definió la transmisión por UART y la GUI. Lo de UART ya está integrado en `tp3-base-adquisicion`. La GUI sigue en su rama hasta que se alinee con el protocolo.

| Decisión | Por qué | Dónde |
| --- | --- | --- |
| Trama binaria mínima: `A5 5A` · tipo (1 B) · N (2 B) · Fs (4 B) · payload, little-endian | La consigna solo pide transmitir el buffer y graficarlo; sin CRC, número de secuencia ni bin de pico | `uart_stage.c`, rama `dev-tp3-uart` |
| El largo del payload no se transmite: N/2 valores en FFT, N en bypass | Se deriva del tipo y de N | `uart_stage.c` |
| FFT se transmite como magnitud en dB × 100 (int16); bypass como muestras q15 | Entero de 16 bits con dos decimales, sin floats en la trama | `uart_stage.h` |
| Envío no bloqueante por interrupción (EDMA queda para después si hace falta) | Más simple de probar primero | `uart_stage.c` |
| Si la UART sigue ocupada con la trama anterior, la nueva se descarta y se cuenta | No frenar el procesamiento esperando a la UART | `uart_stage_send_frame()` |
| Comandos nuevos: `c` envío continuo, `s` una sola trama, `t` contadores de tramas | Controlar el envío desde la PC | `MCXN947_Project_TP3.c` |
| Streaming de texto (`p`) y binario (`c`) son excluyentes; no se imprime texto mientras alguno está activo | Comparten LPUART4 y se mezclarían los bytes | `uart_stage.c` |
| SW3 recorre BYPASS → FFT 512 → FFT 1024 → FFT 2048 (decisión del equipo registrada por Wanda) | Una sola tecla cubre el bypass y los tres tamaños | TODO en `MCXN947_Project_TP3.c`, implementado por Melisa |
| GUI en Python (Tkinter + matplotlib) con protocolo extendido: ventana, largo, secuencia, bin de pico y CRC-16 | Validar la trama y detectar tramas perdidas | `TP3/tools/fft_gui.py`, rama `dev-tp3-gui` |
| La GUI ofrece ventana rectangular o Hann (comando `w`) | Comparar fuga espectral | `fft_gui.py` |

## Decisiones de Melisa

Melisa definió cómo se adquiere y procesa cada ventana, y cómo se integra el trabajo de las ramas. Están en `tp3-base-adquisicion`.

| Decisión | Por qué | Dónde |
| --- | --- | --- |
| Dos buffers de ventana ping-pong de 2048 muestras: la ISR del ADC llena uno mientras PendSV procesa el otro | La ISR no puede bloquearse; el cálculo pesado va en PendSV, con menor prioridad | `pipeline.c` |
| El cambio de N se aplica recién al empezar la ventana siguiente | Una ventana nunca mezcla muestras de dos N distintos | `pipeline_step()` |
| Cada ventana guarda con qué Fs, N y modo se tomó, capturados al arrancarla | La trama enviada tiene que llevar los parámetros reales, aunque se toquen mientras se llena | `frame_info_t`, `pipeline.h` |
| Al cambiar Fs se descarta la ventana a medio llenar | Ninguna ventana mezcla dos frecuencias de muestreo | `pipeline_next_sample_rate()` |
| Si se completa una ventana mientras PendSV sigue con la anterior, se descarta y se cuenta; el modo no cambia | No hay un tercer buffer libre; el contador muestra cuándo no entra en tiempo real | `pipeline_step()` |
| El DAC repite la señal de entrada, no la salida | El resultado de la FFT es un espectro, no una señal para el osciloscopio | `pipeline_step()` |
| SW3 recorre BYPASS → FFT 512 → FFT 1024 → FFT 2048, el mismo ciclo que el comando `m`, con cambio diferido | Seguir la decisión del equipo registrada por Wanda | `pipeline_next_processing_mode()` |
| Sin ventana Hann en el firmware: solo rectangular | La consigna no la pide | `frame_info_t` sin campo de ventana |
| La FFT usa `arm_cfft_q15` de CMSIS-DSP con las instancias de 512, 1024 y 2048, y después `arm_cmplx_mag_q15` | Funciones de la biblioteca recomendada; el resultado va a un buffer de salida propio. Se mandan los bins 0 a N/2 − 1 en dBFS × 100: 0 dB es un tono de amplitud máxima para cualquier N, y un bin en 0 vale −100 dB | `processing_stage.c` |
| Se mide el tiempo de la FFT con el contador de ciclos DWT | Base para las conclusiones: tiempo de cálculo contra duración de la ventana | Pendiente |
| Firmware con el protocolo mínimo de `dev-tp3-uart`; adaptar la GUI le queda a Wanda | La GUI es de Wanda; el firmware sigue el protocolo más reciente | — |
| Se integró solo `dev-tp3-uart` a `tp3-base-adquisicion`; `dev-tp3-gui` entra después por su propio PR | La GUI todavía no es compatible con el protocolo del firmware | Merge `7f5cd63` |
| Se sacó el campo `seq` de `frame_info_t` | No lo usa la trama mínima; si la GUI lo necesita, se vuelve a agregar | `pipeline.h` |
| La trama binaria se manda desde PendSV, apenas se calcula la ventana: con `c` cada ventana que encuentre la UART libre, con `s` una sola | El buffer de salida se copia a la UART antes de que lo pise la ventana siguiente | `send_output_frame()`, `pipeline.c` |
| El texto (`PRINTF`) y las tramas binarias se turnan la UART: el texto espera a que termine la trama, y mientras se imprime no arranca ninguna | Comparten LPUART4; si se escriben a la vez se mezclan los bytes y la trama llega rota | `uart_stage_try_begin_text()`, `MCXN947_Project_TP3.c` |

## Pendientes de decidir

- [ ] Estado inicial y N del bypass. Hoy arranca en BYPASS con N = 1024 y, al volver de FFT 2048, el bypass queda en 2048. Propuesta: BYPASS siempre con N = 512, el caso que nombra la consigna y el tamaño del buffer del Laboratorio #1. Por ahora queda como está; se consulta al profesor por mail junto con el uso del DAC.
- [ ] Alinear la GUI con la trama mínima del firmware, o extender la trama con lo que espera la GUI (Wanda).
- [ ] Adaptar el OLED: todavía muestra la pantalla de filtros del TP2 y en modo FFT dice lo mismo que en BYPASS.
- [ ] Baudrate: a 115200 una trama FFT de 2048 tarda cerca de 180 ms en salir, así que con Fs alta se descartan varias ventanas. Subirlo se configura desde MCUXpresso.
