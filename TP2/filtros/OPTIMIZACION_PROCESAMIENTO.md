# Optimización del procesamiento FIR en la FRDM-MCXN947

Análisis de las alternativas para que los filtros FIR del TP2 entren en
tiempo real en las cinco frecuencias de muestreo. Parte de las mediciones de
`PRUEBAS_PLACA.md` y del análisis del código compilado.

## Resumen

1. **Hoy CMSIS-DSP no usa las instrucciones DSP de multiplicación doble
   (`SMLALD`).** `arm_fir_q15` se compila en su versión genérica en C, que
   hace una multiplicación por instrucción. El código optimizado de CMSIS
   solo se activa si se define `ARM_MATH_LOOPUNROLL`, y ese símbolo no está
   definido en el proyecto. Subir la optimización a `-O3` no lo cambia: por
   eso `-O3` dio los mismos resultados que `-Os` en la placa.
2. **Procesar de a una muestra impide usar ese código aunque se active.** La
   versión optimizada calcula 4 salidas a la vez, así que solo se aplica con
   bloques de 4 muestras o más. Además, cada llamada a `arm_fir_q15` copia
   todo el buffer de estado (N valores): con bloques de 1 muestra esa copia
   se paga en cada muestra.
3. **Combinando `ARM_MATH_LOOPUNROLL` con bloques de 32 muestras**, el costo
   estimado baja de ~12 a ~2 ciclos por tap (unas 5 veces menos). Con eso,
   según la proyección, los cuatro filtros nominales entrarían hasta 22 kHz.
4. **A 44 y 48 kHz el pasa altos, el pasa banda y el elimina banda siguen sin
   entrar.** Para esos casos hace falta procesamiento multitasa (todo FIR) o
   alguna de las optimizaciones de la sección 7, que se pueden combinar.

Orden recomendado: sección 2 (activar el código DSP) → sección 4
(procesamiento por bloques) → medir con `DWT->CYCCNT` (sección 10) → recién
ahí decidir entre las alternativas de las secciones 5 a 8.

> Las cifras de ciclos de este documento salen del análisis del código
> compilado y de las pruebas sin instrumental de `PRUEBAS_PLACA.md`. Son
> estimaciones: hay que confirmarlas midiendo en la placa (sección 10).

---

## 1. Punto de partida

| | |
|---|---|
| Firmware | `tp2-dev-coeff` @ `1126790`, procesamiento muestra a muestra en la ISR del ADC |
| Compilación | Release. Todo el proyecto en `-Os`, incluida la carpeta `CMSIS/DSP` (en el `.cproject` commiteado la carpeta tiene una configuración propia, también en `-Os`) |
| `ARM_MATH_LOOPUNROLL` | No definido |
| Clock | 150 MHz |

Ciclos disponibles por muestra (`150e6 / Fs`):

| Fs | 8 kHz | 16 kHz | 22 kHz | 44 kHz | 48 kHz |
|---|---|---|---|---|---|
| Ciclos por muestra | 18 750 | 9 375 | 6 818 | 3 409 | 3 125 |

De las pruebas en placa surge un costo de **~11,5 a 12,5 ciclos por tap**
más un costo fijo de **~800 a 1450 ciclos por muestra** (ver
`PRUEBAS_PLACA.md`, "Costo de procesamiento estimado").

---

## 2. ¿Se están usando las instrucciones DSP?

### 2.1 Qué son

El Cortex-M33 del MCXN947 incluye la **extensión DSP** del set de
instrucciones ARMv8-M. Las más importantes para un FIR en Q15 son:

| Instrucción | Qué hace | Multiplicaciones por instrucción |
|---|---|---|
| `SMLALBB` | Multiplica dos valores de 16 bits y acumula en 64 bits | 1 |
| `SMLALD` | Toma dos pares de valores de 16 bits empaquetados en registros de 32 bits, multiplica cada par y acumula ambos productos en 64 bits | 2 |
| `SMLALDX` | Igual que `SMLALD`, pero cruzando las mitades | 2 |
| `PKHBT` | Arma un registro de 32 bits con dos mitades de 16 bits de registros distintos | — |

`SMLALD` permite además cargar dos muestras (o dos coeficientes) con una sola
lectura de 32 bits, en lugar de dos lecturas de 16 bits.

### 2.2 Los tres niveles que tienen que estar activos

Para que `arm_fir_q15` use `SMLALD` tienen que cumplirse tres condiciones:

| Nivel | Condición | Estado actual |
|---|---|---|
| Compilador | `-mcpu=cortex-m33` (sin `+nodsp`) define `__ARM_FEATURE_DSP = 1` | ✅ Verificado con el compilador de MCUXpresso |
| CMSIS | `arm_math_types.h` define `ARM_MATH_DSP` si `__ARM_FEATURE_DSP == 1` | ✅ Automático |
| `arm_fir_q15.c` | El código con `__SMLALD`/`__SMLALDX` está dentro de `#if defined (ARM_MATH_LOOPUNROLL)` | ❌ **No definido** |

Sin `ARM_MATH_LOOPUNROLL`, `arm_fir_q15` usa este bucle genérico:

```c
while (tapCnt > 0U) {
  acc0 += (q31_t) *px++ * *pb++;
  acc0 += (q31_t) *px++ * *pb++;
  tapCnt--;
}
```

que el compilador traduce a lecturas de 16 bits (`ldrh`) y `SMLALBB`: usa una
instrucción DSP, pero de a una multiplicación.

### 2.3 Evidencia

**En el firmware actual.** Desensamblando `arm_fir_q15` del ejecutable
`Release/MCXN947_Project_TP2.axf`, el bucle interno es:

```
ldrh.w   r2, [r7, r4, lsl #2]     ; muestra
ldrh.w   r8, [ip, r4, lsl #2]     ; coeficiente
smlalbb  sl, fp, r2, r8           ; 1 multiplicación
ldrh.w   r2, [lr, r4, lsl #2]
ldrh.w   r8, [r9, r4, lsl #2]
adds     r4, #1
smlalbb  sl, fp, r2, r8           ; 1 multiplicación
b.n      ...
```

No aparece ninguna instrucción `smlald`.

**Compilando `arm_fir_q15.c` por separado**, con el mismo compilador y las
mismas opciones de CPU que el proyecto:

| Optimización | `ARM_MATH_LOOPUNROLL` | `smlald`/`smlaldx` | `smlalbb` | Tamaño |
|---|---|---|---|---|
| `-Os` | no | 0 | 2 | 160 B |
| `-Os` | sí | 12 | 2 | 696 B |
| `-O3` | no | 0 | 2 | 264 B |
| `-O3` | sí | 12 | 2 | 816 B |

El nivel de optimización no habilita `SMLALD`: solo lo hace
`ARM_MATH_LOOPUNROLL`. Los `smlalbb` que quedan con unroll corresponden al
bucle que procesa las muestras sobrantes cuando el bloque no es múltiplo de 4.

### 2.4 Cómo activarlo en MCUXpresso

1. Clic derecho en el proyecto → *Properties* → *C/C++ Build → Settings*.
2. *Configuration*: **Release** (o *All configurations*).
3. *MCU C Compiler → Preprocessor → Defined symbols* → agregar
   **`ARM_MATH_LOOPUNROLL`**.
4. *Apply and Close*, *Clean* y compilar.

Se puede definir para todo el proyecto: solo afecta a los fuentes de CMSIS-DSP.

### 2.5 Cómo verificarlo

Desde una terminal, con el compilador que trae MCUXpresso:

```bash
T=$(ls -d /usr/local/mcuxpressoide*/ide/plugins/com.nxp.mcuxpresso.tools.linux*/tools/bin | head -1)
"$T/arm-none-eabi-objdump" -d TP2/MCXN947_Project_TP2/Release/MCXN947_Project_TP2.axf \
  | awk '/<arm_fir_q15>:/{f=1} f&&/^$/{exit} f' | grep -c smlald
```

Tiene que dar un número mayor que 0 (con unroll debería dar 12). También se
puede ver en MCUXpresso: en una sesión de debug, abrir la vista
*Disassembly*, ir a `arm_fir_q15` y buscar `smlald`.

Para comprobar el nivel de optimización que se usó realmente en CMSIS:

```bash
grep -o -- '-O[0-3sg]' TP2/MCXN947_Project_TP2/Release/CMSIS/DSP/Source/FilteringFunctions/subdir.mk
```

---

## 3. Anatomía del costo de `arm_fir_q15`

Cada llamada a `arm_fir_q15(S, in, out, B)` hace tres cosas:

1. **Copia las B muestras nuevas** al final del buffer de estado.
2. **Calcula las B salidas**: N multiplicaciones y sumas por salida.
3. **Desplaza el buffer de estado**: copia las últimas N−1 muestras al
   principio, para la próxima llamada.

El paso 3 es independiente de B: siempre copia N−1 valores. Con B = 1 esa
copia se paga en cada muestra; con B = 32 se reparte entre 32 muestras.

Con `ARM_MATH_LOOPUNROLL`, el paso 2 calcula **4 salidas a la vez** con
4 acumuladores: cada par de coeficientes cargado se usa para las 4 salidas, y
cada par de muestras cargado se reutiliza. Esto reduce las lecturas de memoria
por multiplicación y permite usar `SMLALD`. Solo se aplica a grupos de 4
salidas: con B = 1 nunca se ejecuta.

### Instrucciones por tap

Contadas en el código compilado (bucle interno; no incluye lo que se hace
una vez por llamada):

| Versión | Bucle de cálculo | Copia del estado (por elemento) |
|---|---|---|
| Genérica (hoy) | 10 instrucciones por 2 taps → **~5 por tap** | ~6 instrucciones |
| `ARM_MATH_LOOPUNROLL` | ~29 instrucciones por 16 multiplicaciones (4 taps × 4 salidas) → **~1,8 por tap** | ~2,3 instrucciones (copia de a 4) |

Modelo por muestra de salida, con N taps y bloques de B muestras:

$$\text{instrucciones por tap} \approx c_{mac} + \frac{c_{copia}}{B}$$

Con la versión genérica y B = 1: 5 + 6 = **~11 instrucciones por tap**. Es
consistente con los **~12 ciclos por tap** medidos en la placa (algunas
instrucciones, como las lecturas, tardan más de un ciclo). También explica que
el procesamiento por bloques de la prueba anterior rindiera unas 2 veces más:
5 + 6/32 ≈ 5,2 instrucciones por tap.

En el bucle optimizado el compilador guarda y recupera acumuladores en la pila
(`strd`/`ldrd` sobre `sp`) porque cuatro acumuladores de 64 bits no entran en
los registros disponibles. Ver `arm_fir_fast_q15` en la sección 7.1.

---

## 4. Procesamiento por bloques

### 4.1 Esquema

1. La ISR del ADC sigue corriendo una vez por muestra, pero solo guarda la
   muestra en un buffer de entrada y escribe en el DAC una muestra del último
   bloque ya filtrado.
2. Al completar B muestras, avisa (por ejemplo, disparando `PendSV`, como en
   el commit `b0468c8`) que hay un bloque listo.
3. El filtrado del bloque completo corre fuera de la ISR, con menor
   prioridad, mientras la ISR sigue llenando el otro buffer (doble buffer o
   *ping-pong*).
4. Si un bloque no termina de filtrarse antes de que se complete el siguiente,
   hay sobrecarga y se puede detectar (el commit `b0468c8` pasaba a BYPASS y
   lo informaba por UART).

El DAC sigue recibiendo una muestra por interrupción, así que la salida
mantiene la temporización exacta. Lo que cambia es la latencia: una muestra
recorre el bloque en el que entró y el bloque en el que sale, es decir, unas
**2B muestras de retardo** además del retardo propio del filtro.

### 4.2 Comparación de tamaños de bloque

Estimación con `ARM_MATH_LOOPUNROLL` activado, usando el modelo de la
sección 3. "Velocidad relativa" se compara contra la situación actual
(genérica, B = 1, ~11 instrucciones por tap). La memoria es la de los buffers
de entrada y salida duplicados (4 × B muestras de 2 bytes).

| B | Instrucciones por tap | Velocidad relativa | Latencia extra (2B) a 8 kHz | a 48 kHz | Memoria de buffers |
|---|---|---|---|---|---|
| 1 | ~7,3 (sin `SMLALD`) | 1,5× | 0,25 ms | 0,04 ms | 8 B |
| 2 | ~6,2 (sin `SMLALD`) | 1,8× | 0,5 ms | 0,08 ms | 16 B |
| 4 | ~2,4 | 4,6× | 1 ms | 0,17 ms | 32 B |
| 8 | ~2,1 | 5,3× | 2 ms | 0,33 ms | 64 B |
| 16 | ~1,95 | 5,7× | 4 ms | 0,67 ms | 128 B |
| **32** | **~1,87** | **5,9×** | **8 ms** | **1,3 ms** | **256 B** |
| 64 | ~1,84 | 6,0× | 16 ms | 2,7 ms | 512 B |
| 128 | ~1,82 | 6,0× | 32 ms | 5,3 ms | 1 KB |
| 256 | ~1,81 | 6,1× | 64 ms | 10,7 ms | 2 KB |

Observaciones:

- **El salto grande está entre B = 2 y B = 4**, cuando empieza a usarse el
  código con `SMLALD`. B tiene que ser **múltiplo de 4**: las muestras
  sobrantes se procesan con el bucle genérico.
- **A partir de B = 16 la mejora es marginal.** El costo restante es casi todo
  el cálculo en sí (~1,8 instrucciones por tap) y la copia del estado ya pesa
  poco.
- **El costo fijo por llamada** (entrada a la función, prólogo, punteros) no
  está en la tabla, pero también se divide por B. Con bloques chicos pesa más,
  sobre todo en los filtros cortos (pasa bajos, 34 a 186 taps).
- **La latencia crece linealmente con B.** En audio, retardos de menos de
  ~10 ms no se perciben como eco. El retardo de grupo de los filtros ya es
  de ~2 ms (LP), ~21 ms (HP/BP) y ~40 ms (BS), así que hasta B = 32 la
  latencia extra es chica comparada con la del filtro.
- **El cambio de filtro o de Fs se aplica entre bloques.** Con B grande el
  cambio tarda más en notarse, aunque con B ≤ 64 sigue siendo imperceptible.
- **Con bloques grandes, cada sobrecarga hace perder más muestras.**

**Recomendación: B = 32.** Es el tamaño del commit `b0468c8`, que ya se probó
en la placa, y está en la zona donde el beneficio ya se estabilizó. B = 16 es
una alternativa razonable si se quiere menos latencia, y B = 64 no aporta una
mejora significativa.

### 4.3 Requisitos de `arm_fir_q15` con bloques

- `arm_fir_init_q15` recibe el tamaño de bloque, y `arm_fir_q15` debe
  llamarse siempre con ese mismo tamaño.
- El buffer de estado debe tener `numTaps + blockSize` elementos (con
  `ARM_MATH_DSP`, `arm_fir_init_q15` limpia esa cantidad).
- `numTaps` debe ser par y mayor o igual a 4 (ya lo garantiza el script de
  diseño).

---

## 5. Reducir el costo fijo por muestra

Las pruebas estiman entre ~800 y ~1450 ciclos fijos por muestra. A 48 kHz eso
es entre un cuarto y casi la mitad del tiempo disponible, antes de calcular
un solo tap. El código de la ISR es corto (leer el ADC, escribir el DAC,
guardar en buffers), así que conviene medir dónde se van esos ciclos
(sección 10) antes de optimizar. Alternativas:

| Alternativa | Qué cambia | Comentario |
|---|---|---|
| FIFO del LPADC con interrupción por *watermark* | Una interrupción cada varias muestras en lugar de una por muestra | El driver tiene `kLPADC_FIFO0WatermarkInterruptEnable`. Encaja con el procesamiento por bloques |
| DMA para ADC y DAC | Las muestras entran y salen sin intervención del CPU; solo hay una interrupción por bloque | El driver del DAC tiene `DAC_EnableDMA`. Es la opción más eficiente, y también la más compleja |
| Simplificar la ISR | Menos llamadas anidadas, sin escrituras a variables que solo sirven para depurar | Mejora menor. Con `-Os` el compilador no siempre inserta las funciones cortas |
| Sacar trabajo que no hace falta en cada muestra | `circular_buffer_push` del buffer de entrada solo es necesario para el modo STOP y el volcado | Mejora menor |

---

## 6. Compilador y memoria

| Alternativa | Efecto esperado | Comentario |
|---|---|---|
| `-O3` en `CMSIS/DSP` (con `ARM_MATH_LOOPUNROLL`) | Moderado | Sin unroll no cambia el bucle interno (sección 2.3). Con unroll puede mejorar algo el orden de las instrucciones. Hay que medirlo |
| `-O2`/`-O3` en `source/` | Bajo a moderado | Afecta a la ISR y al costo fijo, no al cálculo del FIR |
| Optimización en tiempo de enlace (`-flto`) | Bajo a moderado | Permite insertar funciones de un archivo en otro (ISR → pipeline → etapas). Puede complicar la depuración |
| Ejecutar `arm_fir_q15` desde RAM | A medir | El código corre desde la flash, que a 150 MHz necesita ciclos de espera; la caché de la flash lo compensa en parte. Se coloca con una sección de RAM (`.ramfunc` o equivalente en el linker de MCUXpresso) |
| Coeficientes en RAM | A medir | Hoy son `const` y quedan en flash. Copiarlos a RAM al iniciar evita las esperas de la flash al leerlos. El costo es RAM: hasta 2 × `MAX_FIR_TAPS` bytes para el filtro activo |

Las dos últimas son hipótesis: dependen de cómo se comporte la caché de la
flash con este patrón de acceso, y solo se pueden confirmar midiendo.

---

## 7. Variantes del algoritmo FIR

### 7.1 `arm_fir_fast_q15`

Versión de CMSIS con **acumulador de 32 bits** (`SMLAD` en lugar de
`SMLALD`). Usa menos registros, así que el compilador no necesita guardar
acumuladores en la pila, y cada multiplicación-acumulación es más barata. La
API es la misma que `arm_fir_q15`, y usa la misma estructura de estado.

**Riesgo: desborde del acumulador.** El producto de dos valores Q15 es Q30.
Con la entrada a escala completa, la salida acumulada puede llegar a
`sum(|h|)` veces el máximo, y un acumulador de 32 bits solo tiene margen para
un factor 2. Para los filtros actuales:

| Filtro | `sum(abs(h))` | Bits de reducción de la entrada necesarios en el peor caso |
|---|---|---|
| Pasa bajos | 1,97 a 2,05 | 1 |
| Pasa altos | 2,09 a 2,11 | 1 |
| Elimina banda | 2,34 a 2,35 | 1 |
| Pasa banda | 3,16 a 3,22 | 2 |

Con el peor caso de entrada hay que reducir la entrada 1 o 2 bits (dividirla
por 2 o por 4), lo que pierde rango dinámico. La documentación de CMSIS
recomienda reducir `log2(numTaps)` bits para evitar el desborde en cualquier
caso, que es mucho más conservador. Hay que evaluarlo con las señales reales
del TP.

### 7.2 Aprovechar la simetría de los coeficientes

Los filtros de fase lineal cumplen `h[k] = h[N−1−k]`. Entonces:

$$y[n] = \sum_{k=0}^{N/2-1} h[k] \cdot (x[n-k] + x[n-N+1+k])$$

Se suman primero las dos muestras que comparten coeficiente y se multiplica
una sola vez: **la mitad de multiplicaciones**. CMSIS no trae esta variante,
así que hay que escribir el bucle (con `SMLAD`/`SMLALD` y `QADD16`/`SADD16`
para sumar dos pares de muestras por instrucción). La suma previa de dos
valores Q15 puede desbordar: o se usa suma saturada, o se reduce la entrada
1 bit. Ganancia esperada: entre 1,5 y 2 veces sobre la versión con unroll.

### 7.3 Procesamiento multitasa (decimación e interpolación)

Es la única alternativa que resuelve de fondo el pasa altos, el pasa banda y
el elimina banda a 44 y 48 kHz sin dejar de usar FIR.

La cantidad de taps de un FIR es proporcional a `Fs / Δf`. En esos filtros la
transición no se puede ensanchar (un corte de 35 Hz no admite transiciones
mucho mayores a ~60 Hz), así que la única forma de reducir taps es reducir la
Fs a la que se calcula la parte de baja frecuencia:

- **Pasa altos 35 Hz** = señal retrasada − (parte de baja frecuencia).
- **Elimina banda 50 Hz** = señal retrasada − (banda de 42,5 a 57,5 Hz).
- **Pasa banda 35–3500 Hz** = pasa altos de 35 Hz seguido del pasa bajos de
  3500 Hz.

La parte de baja frecuencia se calcula a una Fs reducida en un factor M:

1. Filtro anti-alias y decimación por M (`arm_fir_decimate_q15`).
2. Filtro a Fs/M (`arm_fir_q15`).
3. Interpolación por M de vuelta a Fs (`arm_fir_interpolate_q15`).
4. Resta contra la señal original retrasada lo mismo que la rama de baja
   frecuencia (el retardo total es entero si todos los filtros son tipo I).

Estimación para el pasa altos a 48 kHz con M = 32 (Fs reducida = 1500 Hz):

| Etapa | Taps | Multiplicaciones por muestra de salida |
|---|---|---|
| Decimador (paso hasta ~60 Hz, rechazo desde ~1440 Hz) | ~55 | ~55 / 32 ≈ 1,7 |
| Pasa bajos a 1500 Hz (corte 35 Hz, transición 50 Hz) | ~50 | ~50 / 32 ≈ 1,6 |
| Interpolador | ~55 | ~55 / 32 ≈ 1,7 |
| **Total** | | **~5** |

Contra **~2000** del FIR directo. Para el elimina banda la cuenta es similar
(~90 taps a 1500 Hz → ~3 multiplicaciones por muestra, más el decimador y el
interpolador).

Requisitos y costos:

- Necesita procesamiento por bloques: `arm_fir_decimate_q15` exige que el
  bloque sea múltiplo de M. Con B = 32 y M = 32 hay una muestra de baja
  frecuencia por bloque. Para M grandes conviene decimar en dos etapas
  (por ejemplo, 4 × 8).
- El rizado de la cadena decimador-filtro-interpolador en la banda de paso
  se traslada al resultado de la resta, así que esos filtros tienen que
  diseñarse con poco rizado.
- Es bastante más código y más difícil de verificar que un FIR directo. Se
  puede prototipar primero en el notebook de diseño, en Python.

### 7.4 Filtros IIR

Un filtro IIR de pocas secciones de segundo orden
(`arm_biquad_cascade_df1_q15`) logra estos cortes con decenas de ciclos por
muestra a cualquier Fs. Pero la consigna pide **filtros FIR**, y los IIR no
tienen fase lineal. Solo es una opción si la cátedra lo acepta.

---

## 8. Recursos de hardware del MCXN947

### 8.1 Coprocesador PowerQuad

El MCXN947 tiene un coprocesador DSP **PowerQuad**
(`FSL_FEATURE_SOC_POWERQUAD_COUNT = 1`, registros en `0x500BF000`), que puede
calcular filtros FIR por hardware mientras el CPU hace otra cosa. El proyecto
tiene la definición de sus registros (`PERI_POWERQUAD.h`), pero **no tiene el
driver** (`fsl_powerquad`): habría que agregarlo desde el SDK de MCUXpresso.

Hay que evaluar los formatos de datos que acepta, el costo de configurarlo y
de transferirle los datos, y cómo se integra con el procesamiento por
bloques. Puede ser la solución más eficiente, pero es la que requiere más
investigación.

### 8.2 Segundo núcleo

El MCXN947 tiene dos Cortex-M33 (al conectar la placa aparecen como Device 0
y Device 1). Se podría ejecutar el filtrado en el segundo núcleo, pero hay que
iniciarlo, compartir memoria y sincronizar los dos núcleos. Es mucha
complejidad para este TP.

---

## 9. Proyección: qué entraría en cada escenario

Taps máximos con el 80 % del CPU (el resto queda para la consola y los
botones) y un costo fijo de ~1000 ciclos por muestra. Los ciclos por tap de
las filas 2 y 3 se estiman a partir de las instrucciones contadas (×1,1, que
es la relación entre instrucciones y ciclos que surge de la situación
actual).

| Escenario | Ciclos por tap | 8 kHz | 16 kHz | 22 kHz | 44 kHz | 48 kHz |
|---|---|---|---|---|---|---|
| 1. Actual (genérica, B = 1), medido | ~12 | 1166 | 541 | 371 | 143 | 125 |
| 2. Genérica, B = 32 | ~5,7 | 2456 | 1140 | 781 | 303 | 263 |
| 3. `ARM_MATH_LOOPUNROLL`, B = 32 | ~2,1 | 6666 | 3095 | 2121 | 822 | 714 |

Taps de los diseños nominales (sin presupuesto de CPU):

| Filtro | 8 kHz | 16 kHz | 22 kHz | 44 kHz | 48 kHz |
|---|---|---|---|---|---|
| Pasa bajos | 34 | 64 | 86 | 170 | 186 |
| Pasa altos | 344 | 688 | 948 | 1890 | 2052 |
| Pasa banda | 346 | 690 | 946 | 1890 | 2064 |
| Elimina banda | 646 | 1288 | 1774 | 3534 | 3864 |

Con el escenario 3, **los cuatro filtros nominales entrarían hasta 22 kHz**,
y el pasa bajos en todas las Fs. A 44 y 48 kHz el pasa altos, el pasa banda y
el elimina banda necesitan entre 2 y 5,5 veces más de lo disponible. Para
cubrirlos:

- **Multitasa** (sección 7.3): los resuelve con margen.
- **Simetría + transiciones ensanchadas + menos costo fijo** (secciones 7.2,
  5 y el diseño con presupuesto de CPU): el pasa altos y el pasa banda
  ensanchados necesitan ~1060–1160 taps a 44/48 kHz. Con simetría (~1,2
  ciclos por tap) y un costo fijo menor podrían entrar justo. El elimina
  banda probablemente no.

Estas proyecciones dependen de un modelo simple. Hay que validarlas midiendo
el escenario 3 antes de decidir.

---

## 10. Cómo medir

El Cortex-M33 tiene un contador de ciclos (`DWT->CYCCNT`) que avanza uno por
cada ciclo de reloj. Para habilitarlo una vez, al iniciar:

```c
DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;   /* habilita la unidad de trazas */
DWT->CYCCNT = 0;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;  /* arranca el contador */
```

Para medir una sección de código:

```c
uint32_t t0 = DWT->CYCCNT;
arm_fir_q15(&s_fir_instance, in, out, blockSize);
uint32_t ciclos = DWT->CYCCNT - t0;   /* la resta funciona aunque el contador dé la vuelta */
```

Conviene guardar el máximo y el promedio en variables globales y mostrarlos
desde el loop principal (por ejemplo, con un comando de la consola), nunca
con `PRINTF` dentro de la ISR. Mediciones útiles:

| Qué medir | Para qué |
|---|---|
| `arm_fir_q15` con distintos N y B | Ciclos por tap reales de cada configuración |
| La ISR del ADC completa en BYPASS | Costo fijo por muestra |
| La ISR completa con cada filtro | Uso de CPU real: ciclos medidos / (150e6 / Fs) |

Con el valor medido de ciclos por tap y de costo fijo se actualizan
`MODELOS_COSTO` y `CPU_MAX` en el notebook de diseño, que calcula
automáticamente cuántos taps entran en cada Fs.

---

## 11. Plan recomendado

| Paso | Qué hacer | Esfuerzo | Impacto esperado |
|---|---|---|---|
| 1 | Definir `ARM_MATH_LOOPUNROLL` y verificar `smlald` en el desensamblado (sección 2) | Muy bajo | Ninguno todavía con B = 1, salvo la copia del estado |
| 2 | Volver al procesamiento por bloques con B = 32 (commit `b0468c8`) | Bajo | ~5–6 veces menos ciclos por tap, y vuelve la detección de sobrecarga |
| 3 | Medir con `DWT->CYCCNT` (sección 10) | Bajo | Reemplaza las estimaciones por valores reales |
| 4 | Con los valores medidos, regenerar los coeficientes con presupuesto de CPU | Bajo | Ningún filtro bloquea la placa |
| 5 | Elegir para 44/48 kHz: multitasa, simetría, DMA, PowerQuad o documentar el límite | Medio a alto | Según la alternativa |

Los pasos 1 a 4 tienen costo bajo y no cambian la arquitectura del firmware.
El paso 5 conviene consultarlo con la cátedra: la consigna pide FIR, y no
está claro cuánto esperan que se avance sobre el problema de las Fs altas.
