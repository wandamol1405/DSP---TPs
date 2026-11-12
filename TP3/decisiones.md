# TP3 FFT – Arquitectura y decisiones de diseño

La consigna dice qué hay que hacer. Este documento registra cómo lo resolvió el equipo en los puntos que la consigna deja abiertos, y sirve de base para la sección de diseño del informe.

## Contexto

Lo que la consigna sí pide:

- FFT sobre buffers de distinto tamaño (512, 1024 y 2048 muestras) adquiridos con el Laboratorio #1, con los cambios de frecuencia de muestreo.
- Una tecla de la placa habilita la FFT o hace bypass hacia un buffer de salida distinto del de entrada.
- Transmitir el resultado por el puerto serie y graficar el espectro en la PC.
- Sacar conclusiones sobre la FFT al cambiar la frecuencia de muestreo o el tamaño de ventana.

Todo lo demás no está especificado: cómo se adquiere la ventana, qué pasa al cambiar Fs o N a mitad de una ventana, el formato de la trama, qué tecla hace qué y en qué modo arranca.

Cada decisión indica dónde quedó en el código y su estado. Las que dicen "Pendiente" ya están tomadas pero todavía no están en el código: el número es la issue donde se implementan.

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
- **UART:** PendSV manda el buffer de salida como trama binaria: con `c`, en continuo, cada ventana que encuentre la UART libre; con `s`, una sola.
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
| N | 512 |

### SW3: modo y tamaño de ventana

Cada vez que se presiona SW3, o se manda `m` por UART, se avanza un paso del ciclo:

```
               ┌─────────────────────────── SW3 ────────────────────────────┐
               ▼                                                             │
 encendido ─► BYPASS ──SW3──► FFT 512 ──SW3──► FFT 1024 ──SW3──► FFT 2048 ──┘
```

| Pulsación | Modo | N | Qué sale por UART |
| --- | --- | --- | --- |
| (al encender) | BYPASS | 512 | 512 muestras q15 |
| 1.ª | FFT | 512 | 256 bins en dB × 100 |
| 2.ª | FFT | 1024 | 512 bins |
| 3.ª | FFT | 2048 | 1024 bins |
| 4.ª | BYPASS | 512 | 512 muestras q15 |
| 5.ª | FFT | 512 | 256 bins (y sigue el ciclo) |

El cambio no es inmediato: la ventana que se está llenando termina con el modo y el N con que empezó, y lo nuevo se aplica desde la ventana siguiente.

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

## Formato de la trama

La placa envía el buffer de salida en una trama binaria, little-endian:

| Campo | Tipo | Tamaño | Descripción |
| --- | --- | --- | --- |
| Sync | 2 bytes | 2 B | `0xA5 0x5A` |
| Tipo | uint8 | 1 B | `0` = FFT, `1` = BYPASS |
| N | uint16 | 2 B | Tamaño de la ventana: 512, 1024 o 2048 |
| Fs | uint32 | 4 B | Frecuencia de muestreo, en Hz |
| Datos | int16 × L | 2 × L B | FFT: magnitud en dBFS × 100 de los bins 0 a N/2 − 1 (L = N/2). BYPASS: muestras q15 (L = N) |

La cantidad de datos no se transmite: se deduce del tipo y de N.

### Campos que se evaluaron y no se agregaron

La interfaz gráfica se escribió primero para una trama con más campos. Se revisó cada uno y se decidió dejar la trama mínima y adaptar la interfaz (issue #24):

| Campo | Para qué servía | Por qué no se agrega |
| --- | --- | --- |
| Ventana | Indicar si la FFT usó ventana rectangular o Hann | El firmware solo usa la rectangular: valdría siempre lo mismo |
| Longitud | Saber cuántos datos leer | Se deduce del tipo y de N |
| Número de secuencia | Contar tramas perdidas | El firmware descarta ventanas a propósito cuando la UART está ocupada y ya lleva ese contador (comando `t`) |
| Bin del pico | Marcar el pico en el gráfico | La PC lo calcula sobre los datos recibidos |
| CRC | Descartar tramas corruptas | El enlace es corto y por USB, sin errores de bit en la práctica. Si una trama llega cortada, la interfaz dibuja una ventana mala y se vuelve a sincronizar sola en la siguiente |

Para no confundir datos con un comienzo de trama, la interfaz comprueba que el tipo, el N y la frecuencia de muestreo de la cabecera sean valores válidos.

## Comunicación por UART

```
 loop principal: respuestas de texto ───────────────────────────► LPUART4 TX ──► PC
        ▲            (solo entre dos tramas)                          ▲
        │                                                             │
        │                              PendSV: trama binaria ─────────┘
        │
 comandos ◄── buffer circular de recepción ◄── LPUART4 RX (interrupción) ◄── PC
```

- **Una sola salida de datos:** las tramas binarias, en continuo (`c`) o de a una (`s`). Salen desde PendSV, sin bloquear, apenas se calcula la ventana.
- **Texto:** queda solo para las respuestas a los comandos y los avisos. Sale desde el loop principal con `PRINTF`.
- **Turno entre texto y tramas:** comparten LPUART4, así que el texto sale solo entre dos tramas. Cuando hay texto pendiente y una trama saliendo, el turno queda pedido: no arranca otra trama, y el texto sale apenas termina la actual. Así el texto espera como mucho una trama, también con el envío continuo activo. Las ventanas que se completan mientras se imprime se descartan y se cuentan.
- **Recepción por interrupción:** cada byte recibido se guarda en un buffer circular de 64 bytes, aunque esté saliendo una trama. El loop principal atiende los comandos en cada vuelta: aplica el cambio en el momento y deja la respuesta pendiente para cuando consiga el turno.
- **Comandos:** `m`, `f`, `r`, `c`, `s`, `t` y `h`. El comando `t` muestra, entre otros contadores, cuántos bytes de comandos se perdieron; tiene que quedar en 0.

### Cómo era antes y por qué se cambió

Los comandos se leían consultando la UART, y solo cuando el loop principal conseguía el turno de texto. Con el envío continuo activo, la UART está casi siempre transmitiendo: a 115200 baudios una trama de FFT de 2048 puntos tarda cerca de 180 ms. Mientras tanto no se leían los comandos. Los bytes recibidos se acumulaban hasta desbordar, la UART marcaba el error de desborde y, mientras esa marca seguía puesta, no guardaba ningún byte nuevo: la placa dejaba de responder.

Además de las tramas había tres salidas de texto heredadas del TP1 y del TP2: el envío muestra a muestra (`p`), su cambio de formato (`o`) y el volcado del buffer circular (`d`). Se quitaron: con la trama binaria y la interfaz gráfica no hacen falta. Con ellas activas tampoco se imprimían las respuestas a los comandos.

### Velocidad del puerto

#### Análisis teórico y justificación de 921600 baudios

Para que el envío continuo de datos (`c`) funcione en tiempo real sin descartar ventanas por congestión de la UART, la transmisión de cada trama debe completarse antes de que el ADC termine de llenar la siguiente ventana:

$$T_{\text{tx}} \le T_{\text{w}}$$

Donde:
* $T_{\text{w}} = \frac{N}{F_{\text{s}}}$ es el tiempo que tarda el ADC en adquirir una ventana de $N$ muestras a frecuencia $F_{\text{s}}$. El caso más exigente (ventana más corta con mayor cantidad de datos) ocurre con $N = 2048$ y $F_{\text{s}} = 48\text{ kHz}$:
  $$T_{\text{w, min}} = \frac{2048}{48000\text{ Hz}} \approx 42,67\text{ ms}$$
* $T_{\text{tx}} = \frac{\text{Bits totales}}{\text{Baudrate}}$ es el tiempo que tarda la UART en transmitir la trama. Con configuración 8N1 (1 bit de start, 8 de datos, 1 de stop, sin paridad), cada byte transmitido requiere 10 bits en la línea serie.

El tamaño de la trama mínima está dado por:
* Cabecera: 9 bytes (`Sync(2) + Type(1) + N(2) + Fs(4)`).
* Carga útil (*payload*):
  * **Modo FFT:** $L = N / 2$ bins de frecuencia en `int16_t` (2 bytes c/u) $= N$ bytes.
  * **Modo BYPASS:** $L = N$ muestras temporales en `int16_t` (2 bytes c/u) $= 2N$ bytes.

Para $N = 2048$ en FFT, la trama mide:
$$\text{Tamaño} = 9\text{ B} + 2048\text{ B} = 2057\text{ bytes} \implies 2057 \times 10 = 20.570\text{ bits}$$

#### Comparación entre velocidades candidatas

Calculando el tiempo de transmisión para el caso crítico ($N = 2048$, $F_{\text{s}} = 48\text{ kHz}$, $T_{\text{w}} \approx 42,67\text{ ms}$):

| Baudrate | Tiempo de transmisión ($T_{\text{tx}}$) | Comparación con $T_{\text{w}}$ (42,7 ms) | Comportamiento |
| --- | :---: | :---: | --- |
| **115.200** | $\frac{20.570}{115.200} \approx \mathbf{178,6\text{ ms}}$ | $T_{\text{tx}} \approx 4,18 \times T_{\text{w}}$ | **No cumple.** Descarta entre 3 y 4 ventanas por cada una transmitida. |
| **460.800** | $\frac{20.570}{460.800} \approx \mathbf{44,6\text{ ms}}$ | $T_{\text{tx}} > T_{\text{w}}$ ($44,6 > 42,7\text{ ms}$) | **No cumple estrictamente.** La trama tarda un poco más que la ventana, descartando 1 de cada 2 ventanas (50% de descarte). |
| **921.600** | $\frac{20.570}{921.600} \approx \mathbf{22,3\text{ ms}}$ | $T_{\text{tx}} \approx 0,52 \times T_{\text{w}}$ | **Cumple.** Tarda la mitad de la ventana, permitiendo transmitir el 100% de las ventanas sin congestión. |

A continuación se compara el tiempo de transmisión de cada tipo de trama con la duración de la ventana a 48 kHz:

| Trama | Tamaño | A 115200 baudios | A 460800 baudios | A 921600 baudios | Ventana a 48 kHz |
| --- | --- | :---: | :---: | :---: | :---: |
| FFT 512 | 521 B (5210 bits) | 45,2 ms | 11,3 ms | 5,7 ms | 10,7 ms |
| FFT 1024 | 1033 B (10330 bits) | 89,7 ms | 22,4 ms | 11,2 ms | 21,3 ms |
| FFT 2048 | 2057 B (20570 bits) | 178,6 ms | 44,6 ms | 22,3 ms | 42,7 ms |
| BYPASS 512 | 1033 B (10330 bits) | 89,7 ms | 22,4 ms | 11,2 ms | 10,7 ms |

*(Nota: En BYPASS 512 a 48 kHz la ventana dura 10,7 ms y la trama a 921600 tarda 11,2 ms, por lo que a esa frecuencia máxima se produce un descarte mínimo de 1 ventana cada ~20 enviadas, pero en todas las configuraciones de FFT se transmite sin pérdidas).*

#### Viabilidad en hardware y error de reloj (FRO 12 MHz)

En el microcontrolador FRDM-MCXN947, el periférico `LPUART4` se alimenta del oscilador interno FRO configurado en 12 MHz (`BOARD_DEBUG_UART_CLK_FREQ = 12000000U`).

El generador de baudrate del LPUART calcula el divisor a partir de la tasa de sobremuestreo ($OSR$, entre 4 y 32) y el divisor $SBR$:
$$\text{Baudrate} = \frac{F_{\text{src}}}{OSR \times SBR}$$

Para 921.600 baudios con $F_{\text{src}} = 12\text{ MHz}$, el driver optimiza seleccionando $OSR = 13$ y $SBR = 1$:
$$\text{Baud}_{\text{real}} = \frac{12.000.000}{13 \times 1} \approx 923.076,92\text{ baudios}$$

El error relativo de sincronización es:
$$\text{Error} = \frac{923.076,92 - 921.600}{921.600} \times 100 \approx \mathbf{+0,16\%}$$

Un error del 0,16% es totalmente despreciable frente al margen admisible de la norma UART ($\approx \pm 2\%$ a $\pm 3\%$), asegurando una comunicación libre de errores con el chip puente USB-VCOM MCU-Link. Por todo esto, **921600 baudios es la velocidad elegida** para el sistema.

## Decisiones

### Adquisición y ventanas

| Decisión | Por qué | Dónde | Estado |
| --- | --- | --- | --- |
| Dos buffers de ventana ping-pong de 2048 muestras: la ISR del ADC llena uno mientras PendSV procesa el otro | La ISR no puede bloquearse; el cálculo pesado va en PendSV, con menor prioridad | `pipeline.c` | Implementado |
| El cambio de N y de modo se aplica recién al empezar la ventana siguiente | Una ventana nunca mezcla muestras de dos N distintos | `pipeline_step()` | Implementado |
| Cada ventana guarda con qué Fs, N y modo se tomó, capturados al arrancarla | La trama enviada tiene que llevar los parámetros reales, aunque se toquen mientras se llena | `frame_info_t`, `pipeline.h` | Implementado |
| Al cambiar Fs se descarta la ventana a medio llenar | Ninguna ventana mezcla dos frecuencias de muestreo | `pipeline_next_sample_rate()` | Implementado |
| Si se completa una ventana mientras PendSV sigue con la anterior, se descarta y se cuenta; el modo no cambia | No hay un tercer buffer libre; el contador muestra cuándo no entra en tiempo real | `pipeline_step()` | Implementado |
| El DAC repite la señal de entrada, no la salida | El resultado de la FFT es un espectro, no una señal para el osciloscopio | `pipeline_step()` | Implementado |

### Modos y FFT

| Decisión | Por qué | Dónde | Estado |
| --- | --- | --- | --- |
| SW3 y el comando `m` recorren BYPASS → FFT 512 → FFT 1024 → FFT 2048 | Una sola tecla cubre el bypass y los tres tamaños | `pipeline_next_processing_mode()` | Implementado |
| BYPASS usa siempre N = 512 | Es el tamaño del buffer del Laboratorio #1, y el N de BYPASS no tiene que depender de qué FFT se usó antes | `pipeline.c` | Implementado |
| La FFT usa `arm_cfft_q15` de CMSIS-DSP con las instancias de 512, 1024 y 2048, y después `arm_cmplx_mag_q15` | Funciones de la biblioteca recomendada; el resultado va a un buffer de salida propio | `processing_stage.c` | Implementado |
| Se mandan los bins 0 a N/2 − 1 en dBFS × 100 | 0 dB es un tono de amplitud máxima para cualquier N, y un bin en 0 vale −100 dB. Entero de 16 bits con dos decimales, sin floats en la trama | `processing_stage.c` | Implementado |
| Sin ventana Hann ni remoción de la media en el firmware: solo ventana rectangular | La consigna no las pide. El efecto de la ventana se muestra en un cuaderno de Jupyter con señales simuladas | `notebooks/fft_ventanas_y_fenomenos.ipynb` | Implementado |
| El firmware busca el bin de mayor magnitud, sin contar el de continua, y muestra su frecuencia en el OLED | Es el resultado de la FFT que se puede leer en la placa sin la PC. No se agrega a la trama | `processing_stage.c`, `ui.c` | Implementado |
| El OLED muestra Fs, modo, N, resolución en frecuencia y frecuencia del pico | Representa el estado del sistema | `ui.c` | Implementado |
| Se mide cuánto tarda el procesador en calcular la FFT para cada N | Compararlo con la duración de la ventana (ver abajo) | `pipeline.c` | Pendiente (#30) |

**Sobre la medición del tiempo de cálculo.** Hay dos tiempos distintos. La duración de la ventana, T = N/Fs, es lo que tarda el ADC en juntar las N muestras y se calcula. El tiempo de cálculo es lo que tarda el procesador en ejecutar la FFT sobre la ventana ya completa: depende de N y del procesador, no de Fs, y hay que medirlo. Si el cálculo tarda menos que la ventana, la placa procesa todas las ventanas.

Para medirlo se usa un contador que trae el procesador y que suma uno por cada ciclo de su reloj. Se lee antes y después de la FFT y se restan los dos valores. Como el reloj es de 150 MHz, dividir esa diferencia por 150 millones da el tiempo en segundos. En el código ese contador es el registro `CYCCNT`, de un bloque de depuración del procesador llamado DWT.

### UART y trama

| Decisión | Por qué | Dónde | Estado |
| --- | --- | --- | --- |
| Trama binaria mínima: `A5 5A`, tipo, N, Fs y datos | La consigna solo pide transmitir el buffer y graficarlo (ver "Formato de la trama") | `uart_stage.c` | Implementado |
| Envío de la trama sin bloquear, por interrupción | No frenar el procesamiento esperando a la UART | `uart_stage_send_frame()` | Implementado |
| Si la UART sigue ocupada con la trama anterior, la nueva se descarta y se cuenta | Ídem; el comando `t` muestra los contadores | `uart_stage_send_frame()` | Implementado |
| La trama se manda desde PendSV, apenas se calcula la ventana | El buffer de salida se copia a la UART antes de que lo pise la ventana siguiente | `send_output_frame()`, `pipeline.c` | Implementado |
| El texto y las tramas se turnan la UART, y el texto pendiente tiene prioridad sobre la trama siguiente | Comparten LPUART4; si se escriben a la vez se mezclan los bytes y la trama llega rota. Con la prioridad, el texto espera como mucho una trama | `uart_stage_try_begin_text()` | Implementado |
| Los comandos se reciben por interrupción y se procesan siempre | Que la placa responda aunque esté enviando tramas | `uart_stage.c` | Implementado |
| Una sola salida de datos: se quitan `p`, `o` y `d` | Eran salidas del TP1 y del TP2 para otros programas de PC; con la trama binaria y la interfaz gráfica no hacen falta | `uart_stage.c`, `MCXN947_Project_TP3.c` | Implementado |
| Las respuestas de texto salen entre tramas, también con el envío continuo | Poder consultar el estado sin cortar el envío | `MCXN947_Project_TP3.c` | Implementado |
| Se sube la velocidad del puerto serie | A 115200 baudios se descartan muchas ventanas | Configuración desde MCUXpresso | Pendiente (#25) |

### Programas de PC

| Decisión | Por qué | Dónde | Estado |
| --- | --- | --- | --- |
| Interfaz gráfica en Python con Tkinter y matplotlib | Graficar el espectro y la señal en el tiempo, y enviar comandos | `tools/fft_gui.py` | Implementado |
| La interfaz se adapta a la trama mínima y a los comandos del firmware | Decodifica la trama mínima (sync, tipo, N, Fs y datos) y envía los comandos reales del firmware | `tools/fft_gui.py`, `tests/` | Implementado |
| Simulador de la placa para probar la interfaz sin hardware | Desarrollar la interfaz y el firmware por separado | `tests/fake_board.py` | Implementado |

### Proyecto de señal interna

Para probar la placa sin generador de señales se va a mantener un proyecto de MCUXpresso aparte, copia del original, que agrega un generador de tono interno (issue #29). Tiene tres fuentes: el ADC, un lazo del DAC al ADC por cable y una señal sintética escrita directo en la ventana. Usa la misma trama y los mismos comandos, así que funciona con la misma interfaz gráfica. El proyecto original no cambia.

La versión de prueba está en la rama `tp3-ejemplo-senal-interna`.

## Sin decidir

- Buffer circular de entrada y reproducción por el DAC en STOP, heredados del TP1: no se definió si se mantienen.
- Uso del DAC: la consigna no lo pide. Quedó pendiente consultarlo con el profesor.
