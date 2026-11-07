# Rediseño de los filtros FIR con presupuesto de CPU

Cambios en `disenio_filtros_tp2.m` y en los coeficientes que genera
(`MCXN947_Project_TP2/source/filter_coeffs.h` y `graficas/`). No se tocó
el firmware C ni el `README.md` del TP.

## Problema de partida

El orden de un FIR por ventaneo de Kaiser es

```
N ≈ (Astop − 7.95) / (2.285 · 2π · df / Fs)
```

Como el ancho de transición `df` estaba fijo en Hz para las 5 frecuencias
de muestreo, `N` crecía proporcional a `Fs`. Por eso a 44k/48k aparecían
filtros de hasta 3864 taps (elimina banda), que según la propia estimación
del script necesitaban más del 100 % de la CPU.

## Cambios en el script

### 1. Presupuesto de CPU explícito

Parámetros nuevos (sección *PRESUPUESTO DE CPU*):

| Parámetro | Valor | Significado |
|---|---|---|
| `CPU_MAX` | 0.50 | Fracción de CPU para el FIR; el resto queda para ADC, DAC, UART y botones |
| `ANCHO_MIN_ZONA` | 5 Hz | Ancho mínimo que conserva cada banda de paso o de rechazo al ensanchar transiciones |
| `FACTOR_ENSANCHE` | 1.05 | Paso de ensanche si `fir1` necesita más taps que lo que predice Kaiser |
| `SI_NO_ENTRA` | `'cumplir'` | Qué exportar cuando un filtro no entra en el presupuesto (ver abajo) |

Para cada filtro y cada Fs se calcula

```
taps_max = CPU_MAX · F_CPU / (CICLOS_POR_TAP · Fs)
```

### 2. Se ensancha `df`, nunca se relaja el Astop

- Si el filtro entra en `taps_max` con el `df` nominal, queda como estaba
  (**nominal**).
- Si no entra, se ensanchan solo las transiciones más angostas hasta que
  entre (**df ensanchado**).
- Cada transición tiene un tope geométrico: ninguna banda puede quedar con
  menos de `ANCHO_MIN_ZONA`. Si la transición que fija `N` llega a ese tope
  y el filtro sigue sin entrar, se marca **NO VIABLE**.

Con `SI_NO_ENTRA = 'cumplir'` (por defecto) se exporta el diseño que cumple
el Astop aunque exceda el presupuesto. Con `'recortar'` se exporta un diseño
de `taps_max` que entra en el presupuesto pero **no** cumple el Astop.

### 3. Ubicación de las transiciones (`fref` y `ancla`)

El campo `cutoff` de cada filtro se reemplazó por `fref` + `ancla`:

- `'centro'` (pasa bajos, pasa altos, pasa banda): `fref` es el corte a −6 dB
  y la transición crece simétrica a su alrededor, así el corte no se mueve.
- `'stop'` (elimina banda): `fref = [42.5 57.5]` son los bordes de la banda de
  rechazo que exige la consigna (50 Hz ± 7.5 Hz). Las transiciones crecen hacia
  afuera, así la banda rechazada nunca se achica. Con `df = 20 Hz` da los mismos
  cortes que antes (32.5 y 67.5 Hz).

### 4. Astop verificado sobre los coeficientes Q15

Antes, la búsqueda de `N` verificaba el Astop con los coeficientes en float,
y algunos filtros quedaban en 29.9 dB tras cuantizar. Ahora se verifica sobre
los coeficientes normalizados y cuantizados a Q15, que son los que se
exportan. Todos los filtros cumplen el Astop en Q15.

### 5. Respaldo de `kaiser` y `fir1`

Si no está el Signal Processing Toolbox (ni el paquete `signal` de Octave), el
script define equivalentes propios: ventana de Kaiser con `besseli` y respuesta
ideal ventaneada con la misma escala que `fir1`. Se comprobó que reproducen
exactamente los coeficientes anteriores (pasa bajos, pasa altos y elimina
banda a 8 kHz).

### 6. Salidas más informativas

- La tabla por consola y el comentario de `filter_coeffs.h` muestran ahora,
  para cada filtro: taps usados y máximos, `df` nominal y usado, CPU estimada,
  Astop pedido y real, ripple, retardo de grupo y estado.
- Los filtros NO VIABLES se listan aparte, con la alternativa a evaluar.
- Los títulos de las gráficas incluyen el `df` usado y el estado.

## Resultados

Presupuesto: 50 % de CPU a 150 MHz, estimando 3 ciclos/tap.

| Filtro | Fs | Taps / máx | df nom → usado [Hz] | CPU est. | Astop req / Q15 [dB] | Retardo | Estado |
|---|---|---|---|---|---|---|---|
| Pasa bajos | 8k | 34 / 3125 | 400 → 400 | 1 % | 30 / 31.6 | 2.0 ms | nominal |
| Pasa bajos | 16k | 64 / 1562 | 400 → 400 | 2 % | 30 / 30.5 | 1.9 ms | nominal |
| Pasa bajos | 22k | 86 / 1136 | 400 → 400 | 4 % | 30 / 30.0 | 1.9 ms | nominal |
| Pasa bajos | 44k | 170 / 568 | 400 → 400 | 15 % | 30 / 30.2 | 1.9 ms | nominal |
| Pasa bajos | 48k | 186 / 520 | 400 → 400 | 18 % | 30 / 30.2 | 1.9 ms | nominal |
| Pasa altos | 8k | 344 / 3125 | 50 → 50 | 6 % | 30 / 30.0 | 21.4 ms | nominal |
| Pasa altos | 16k | 688 / 1562 | 50 → 50 | 22 % | 30 / 30.0 | 21.4 ms | nominal |
| Pasa altos | 22k | 948 / 1136 | 50 → 50 | 42 % | 30 / 30.2 | 21.5 ms | nominal |
| Pasa altos | 44k | 1062 / 568 | 50 → 60 | 93 % | 30 / 30.0 | 12.0 ms | **NO VIABLE** |
| Pasa altos | 48k | 1160 / 520 | 50 → 60 | 111 % | 30 / 30.2 | 12.1 ms | **NO VIABLE** |
| Pasa banda | 8k | 346 / 3125 | 50/400 → 50/400 | 6 % | 30 / 30.7 | 21.5 ms | nominal |
| Pasa banda | 16k | 690 / 1562 | 50/400 → 50/400 | 22 % | 30 / 30.2 | 21.5 ms | nominal |
| Pasa banda | 22k | 946 / 1136 | 50/400 → 50/400 | 42 % | 30 / 30.2 | 21.5 ms | nominal |
| Pasa banda | 44k | 1062 / 568 | 50/400 → 60/400 | 93 % | 30 / 30.0 | 12.0 ms | **NO VIABLE** |
| Pasa banda | 48k | 1158 / 520 | 50/400 → 60/400 | 111 % | 30 / 30.0 | 12.0 ms | **NO VIABLE** |
| Elimina banda | 8k | 646 / 3125 | 20/20 → 20/20 | 10 % | 25 / 25.2 | 40.2 ms | nominal |
| Elimina banda | 16k | 1288 / 1562 | 20/20 → 20/20 | 41 % | 25 / 25.0 | 40.2 ms | nominal |
| Elimina banda | 22k | 718 / 1136 | 20/20 → 32/32 | 32 % | 25 / 25.1 | 16.3 ms | df ensanchado |
| Elimina banda | 44k | 1478 / 568 | 20/20 → 38/92 | 130 % | 25 / 25.1 | 16.8 ms | **NO VIABLE** |
| Elimina banda | 48k | 1490 / 520 | 20/20 → 38/110 | 143 % | 25 / 25.1 | 15.5 ms | **NO VIABLE** |

`MAX_FIR_TAPS` bajó de 3864 a 1490 (14 564 coeficientes, 28.4 KB en flash).

## Consideraciones

### Pasa altos y pasa banda tampoco entran a 44k/48k, no solo el elimina banda

El borde inferior está en 35 Hz y la transición está centrada en el corte, así
que no puede superar ~60 Hz sin cruzar 0 Hz (con 5 Hz de banda de rechazo
mínima). Por eso no sirve escalar `df` proporcional a Fs: a 48 kHz serían
300 Hz de transición, físicamente imposible para un corte en 35 Hz. Con 60 Hz
siguen haciendo falta ~1100 taps, el doble de lo que permite el presupuesto.

Para el elimina banda pasa lo mismo: la banda de rechazo de 15 Hz con 25 dB
es intrínsecamente cara como FIR por ventaneo a cualquier Fs.

**Alternativa a evaluar (cambio de firmware, fuera de este rediseño):** filtros
IIR biquad con `arm_biquad_cascade_df1_q15`. Unas pocas secciones de segundo
orden logran estos cortes a cualquier Fs con un costo de decenas de ciclos por
muestra. Se pierde la fase lineal.

### En el elimina banda conviene ensanchar ambas transiciones

A 44k/48k la transición superior se ensancha más que la inferior (92 y 110 Hz).
Por teoría de Kaiser eso no reduce `N`, pero en la práctica sí: con β bajo
(Astop = 25 dB) el ripple de las dos transiciones se suma dentro de la banda
de rechazo angosta. Limitando la superior al ancho de la inferior el diseño
pasaba de ~1480 a ~2730 taps.

### El retardo es propio del FIR de fase lineal

El retardo de grupo es (N − 1) / (2 · Fs). Como `N` es proporcional a Fs, el
retardo depende solo de `df`, no de Fs: ~2 ms en el pasa bajos, ~21 ms en pasa
altos y pasa banda, y ~40 ms en el elimina banda nominal. Esto explica parte del
"atraso independiente del filtro y de Fs" visto en el osciloscopio. Lo que
supere esos valores apunta a sobrecarga de CPU.

### El costo de 3 ciclos/tap probablemente está subestimado

Con 3 ciclos/tap, el pasa altos a 16k y 22k usa 22 % y 42 % de CPU, pero en la
placa falló. Los datos de la prueba del pasa bajos dan una pista:

- El corte observado fue 1.2 kHz a 44k y 1.1 kHz a 48k (diseñado en 3.6 kHz).
- En ambos casos, corte observado × Fs ≈ 3600 Hz × 14.7 kHz. Es decir, la placa
  está procesando a ~14.7 kHz efectivos, unos 10 200 ciclos por muestra para
  170–186 taps: **del orden de 40–60 ciclos/tap**.
- Ese valor también explica el resto de la prueba. El pasa bajos anda hasta 22k
  y el pasa altos falla desde 16k. El elimina banda a 8k (646 taps) quedaría en
  ~4–5 kHz efectivos, con el notch corrido a ~25–30 Hz, lo que coincide con
  "corta entre 10 y 40" si eran Hz.

La causa probable es que el build Debug compila CMSIS-DSP desde el código
fuente sin optimización (en MCUXpresso el Debug suele quedar en `-O0`).

**Consecuencia:** los coeficientes actuales solo van a funcionar como indica la
tabla si el firmware se compila optimizado (Release, o al menos `-O3` en la
carpeta `CMSIS/DSP`). Pasos sugeridos:

1. Medir los ciclos reales por muestra en la placa con `DWT->CYCCNT` alrededor
   de `arm_fir_q15`, con el build actual y con el optimizado.
2. Poner el valor medido en `CICLOS_POR_TAP` y volver a correr el script.
   Con ~50 ciclos/tap, prácticamente solo el pasa bajos entra en el presupuesto
   en todas las Fs.

## Cómo correr el script

MATLAB no encuentra scripts dentro de la carpeta `DSP---TPs` (por la ruta),
así que hay que ejecutarlo así:

```
matlab -batch "cd('<ruta>/TP2/filtros'); eval(fileread('disenio_filtros_tp2.m'))"
```

En Octave: `pkg load signal` y ejecutarlo normalmente.
