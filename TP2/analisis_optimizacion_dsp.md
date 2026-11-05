# Análisis de Alternativas de Optimización para el Pipeline de Procesamiento DSP (FIR Q15) en FRDM-MCXN947

**Asignatura:** Procesamiento Digital de Señales  
**Plataforma:** NXP FRDM-MCXN947 (Dual ARM Cortex-M33 @ 150 MHz)  
**Módulo:** TP2 — Filtros Digitales FIR  
**Fecha:** Octubre de 2026  

---

## 1. Diagnóstico del Cuello de Botella Actual

### 1.1. El límite físico de ciclos por muestra
En la arquitectura actual de `MCXN947_Project_TP2`, el filtrado se ejecuta dentro de la interrupción periódica del convertidor analógico a digital (`ADC1_IRQHANDLER`):

```
ADC1_IRQHANDLER() -> pipeline_step() -> processing_stage_process_block(..., length=1) -> arm_fir_q15(..., blockSize=1)
```

A una frecuencia de CPU de **150 MHz**, el tiempo disponible entre dos muestras consecutivas ($T_s = 1/F_s$) impone un presupuesto estricto de ciclos de reloj:

| Frecuencia ($F_s$) | Período ($T_s$) | Ciclos Totales de CPU ($T_s \times 150\text{ MHz}$) | Presupuesto 50% CPU |
| :---: | :---: | :---: | :---: |
| **8 kHz** | $125.0\ \mu\text{s}$ | **18.750 ciclos** | 9.375 ciclos |
| **16 kHz** | $62.5\ \mu\text{s}$ | **9.375 ciclos** | 4.687 ciclos |
| **22 kHz** | $45.45\ \mu\text{s}$ | **6.818 ciclos** | 3.409 ciclos |
| **44 kHz** | $22.73\ \mu\text{s}$ | **3.409 ciclos** | 1.704 ciclos |
| **48 kHz** | $20.83\ \mu\text{s}$ | **3.125 ciclos** | 1.562 ciclos |

Con los coeficientes generados originalmente por ventaneo de Kaiser sin presupuesto de CPU:
* **Elimina Banda (BS) @ 48 kHz:** **3864 taps**.
* **Pasa Altos (HP) / Pasa Banda (BP) @ 48 kHz:** **2052 / 2064 taps**.

Incluso en un procesador teórico con ejecución de 1 instrucción MAC por ciclo sin penalizaciones de memoria ni overhead de interrupción:
$$\text{Tiempo de cómputo (BS 48k)} \ge 3864\text{ ciclos} > 3125\text{ ciclos disponibles}$$

El tiempo de cómputo del filtro excede el período de muestreo. Como resultado, la ISR nunca termina antes de que arribe la siguiente muestra; el procesador queda atrapado en la interrupción, se pierden muestras, el loop principal (`while(1)`) no puede atender la UART ni los botones, y el pipeline colapsa.

---

### 1.2. El problema interno de `arm_fir_q15` con `blockSize = 1`
Al inspeccionar la implementación interna de CMSIS-DSP (`CMSIS/DSP/Source/FilteringFunctions/arm_fir_q15.c`), se observan dos fenómenos críticos cuando se procesa muestra a muestra (`blockSize = 1`):

1. **Inhabilitación del pipeline SIMD:**
   CMSIS-DSP implementa un bucle desenrollado que calcula **4 salidas simultáneamente** (`blkCnt = blockSize >> 2`). Dentro de ese bucle utiliza la instrucción DSP dual-MAC `__SMLALD` (que ejecuta dos multiplicaciones-acumulaciones de 16 bits en un solo ciclo de reloj).
   **Cuando `blockSize = 1`, `blkCnt = 0`.** El bucle SIMD se omite en su totalidad y el procesamiento cae en el bucle residual escalar, que no aprovecha las ventajas dual-MAC de la arquitectura.

2. **Copia redundante de estados $O(N)$ en cada muestra:**
   Al finalizar cada llamada a `arm_fir_q15()`, la función ejecuta una copia en memoria de los últimos $(N - 1)$ estados hacia el inicio del buffer `pState`:
   ```c
   /* Copy the last numTaps - 1 samples to the start of the state buffer. */
   tapCnt = (numTaps - 1U) >> 2U;
   while (tapCnt > 0U) {
       *pStateCurnt++ = *pState++;
       *pStateCurnt++ = *pState++;
       *pStateCurnt++ = *pState++;
       *pStateCurnt++ = *pState++;
       tapCnt--;
   }
   ```
   Si `numTaps = 1000` y `blockSize = 1`, el procesador copia **999 palabras de 16 bits en RAM 48.000 veces por segundo** ($\approx 48\text{ millones de lecturas/escrituras por segundo}$ solo en mantenimiento del buffer de estado).

---

## 2. Cómo Verificar el Uso de Instrucciones DSP en Cortex-M33

Para garantizar que el compilador efectivamente emite código aprovechando las extensiones DSP de la arquitectura, se deben comprobar tres niveles:

### 2.1. Nivel Preprocesador y Banderas de Compilación
El procesador NXP MCXN947 incorpora un núcleo ARM Cortex-M33 con extensiones DSP (ARMv8-M Mainline con DSP Extension).

1. **Bandera `-mcpu=cortex-m33`:**
   En GCC, al pasar `-mcpu=cortex-m33` (sin el sufijo `+nodsp`), el compilador define automáticamente la macro interna:
   ```c
   #define __ARM_FEATURE_DSP 1
   ```
2. **Definiciones en `arm_math_types.h`:**
   La cabecera de CMSIS-DSP evalúa esta característica en las líneas 114–117:
   ```c
   #if (defined (__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP == 1))
     #define ARM_MATH_DSP                   1
   #endif
   ```
   Y define el desenrollado de bucles:
   ```c
   #define ARM_MATH_LOOPUNROLL            1
   ```

3. **Verificación estática en código C:**
   Se puede agregar una guarda en `processing_stage.c`:
   ```c
   #if !defined(ARM_MATH_DSP) || (ARM_MATH_DSP != 1)
   #error "¡ATENCIÓN! Las extensiones DSP no están activadas en la compilación."
   #endif
   ```

### 2.2. Nivel Ensamblador y Desensamblado (`objdump`)
La prueba definitiva consiste en inspeccionar las instrucciones en el archivo objeto generado (`FilteringFunctions.o` o el ejecutable `.elf`).

Ejecutando desde la terminal:
```bash
arm-none-eabi-objdump -d <ruta>/FilteringFunctions.o | grep -E "smlald|smlad|pkhbt"
```

Las instrucciones clave que confirman el soporte hardware DSP son:
* `smlald` / `smlaldx`: Signed Multiply Accumulate Long Dual (realiza dos productos de $16\times 16$ bits y los acumula en un registro de 64 bits en **un único ciclo de reloj**).
* `smlad` / `smladx`: Signed Multiply Accumulate Dual (dos productos $16\times 16$ acumulados en 32 bits).
* `pkhbt` / `pkhtb`: Pack Halfword (empaqueta dos muestras de 16 bits dentro de un registro de 32 bits en 1 ciclo).
* `ssat`: Signed Saturate (saturación aritmética hardware para evitar overflow).

*(Nota: En la compilación de este proyecto ya se verificó la presencia de `smlald`, `smlad` y `pkhbt` en `FilteringFunctions.o`).*

### 2.3. Nivel Dinámico: Medición de Ciclos con `DWT->CYCCNT`
Para medir en la placa física el costo real en ciclos de la llamada a `arm_fir_q15()`:

```c
// Inicialización única (en main o init)
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

// Medición alrededor del filtrado
uint32_t t_start = DWT->CYCCNT;
processing_stage_process_block(in_buf, out_buf, block_size);
uint32_t t_elapsed = DWT->CYCCNT - t_start;
```
Dividiendo `t_elapsed / (numTaps * block_size)` se obtiene el valor empírico exacto de **ciclos por tap**.

---

## 3. Estrategias de Optimización

Existen tres caminos complementarios para asegurar que el sistema procese audio en tiempo real sin saturar el procesador:

---

### Estrategia A: Procesamiento por Bloques (Block Processing)

En lugar de filtrar muestra a muestra en la ISR del ADC, las muestras se acumulan en un buffer intermedio de longitud $B$. Cuando el bloque se completa, se ejecuta `arm_fir_q15()` sobre las $B$ muestras simultáneamente.

#### Beneficios Técnicos del Procesamiento por Bloques:
1. **Activación de instrucciones dual-MAC SIMD:** Para $B \ge 4$, CMSIS-DSP calcula 4 salidas en simultáneo con `__SMLALD`, reduciendo a la mitad los accesos a los coeficientes del filtro.
2. **Amortización de la copia de estado:** La copia de los $(N-1)$ estados de memoria RAM ocurre **una vez cada $B$ muestras**, dividiendo el costo de movimiento de memoria por un factor de $B$.
3. **Reducción del overhead de función y llamadas:** El epílogo y prólogo de función se ejecutan cada $B$ muestras.

#### Análisis Comparativo según el Tamaño de Bloque ($B$):

| Tamaño de Bloque ($B$) | Latencia Adicional @ 48 kHz | Latencia Adicional @ 8 kHz | Factor de Reducción Copia de Estado | Eficiencia SIMD CMSIS-DSP | Impacto en RAM (Buffers ping-pong) |
| :---: | :---: | :---: | :---: | :---: | :---: |
| **$B = 1$ (Actual)** | $0.021\text{ ms}$ | $0.125\text{ ms}$ | $1\times$ (Cero ahorro) | **0%** (Solo bucle residual escalar) | Mínimo ($2\times 1$ muestra) |
| **$B = 4$** | $0.083\text{ ms}$ | $0.500\text{ ms}$ | **$4\times$** menos copias | **Habilitada** (1 iteración SIMD) | Negligible ($2\times 4$ muestras) |
| **$B = 8$** | $0.167\text{ ms}$ | $1.000\text{ ms}$ | **$8\times$** menos copias | **Alta** (2 iteraciones SIMD) | $32\text{ bytes}$ |
| **$B = 16$** | $0.333\text{ ms}$ | $2.000\text{ ms}$ | **$16\times$** menos copias | **Muy alta** (Balance óptimo de registros) | $64\text{ bytes}$ |
| **$B = 32$** | $0.667\text{ ms}$ | $4.000\text{ ms}$ | **$32\times$** menos copias | **Excelente** (Overhead residual $<3\%$) | $128\text{ bytes}$ |
| **$B = 64$** | $1.333\text{ ms}$ | $8.000\text{ ms}$ | **$64\times$** menos copias | **Máxima saturación de hardware** | $256\text{ bytes}$ |

#### Evaluación y Recomendación de Tamaño de Bloque:
* En aplicaciones de audio, la latencia humana perceptible ronda los **$5\text{ ms}$ a $10\text{ ms}$**.
* Con **$B = 16$ o $B = 32$**, la latencia agregada a 48 kHz es inferior a **$1\text{ ms}$** (completamente imperceptible en audio y monitoreo de osciloscopio), pero el throughput de cómputo del filtro se multiplica por un factor de **$3\times$ a $5\times$** respecto a muestra a muestra.

#### Arquitectura de Interrupción y Contexto:
Para no bloquear la ISR del ADC:
* La ISR del ADC únicamente almacena la muestra adquirida en el buffer de entrada activo y envía al DAC la muestra procesada del buffer de salida.
* Al completarse $B$ muestras, la ISR conmuta los buffers (ping-pong) y dispara una interrupción de baja prioridad (`PendSV` o flag para tarea en `main`), donde se ejecuta `processing_stage_process_block(..., B)`.

---

### Estrategia B: Uso de `arm_fir_fast_q15`

CMSIS-DSP provee dos variantes para filtrado FIR en Q15:

1. **`arm_fir_q15()` (Estándar):**
   * Utiliza acumuladores internos de **64 bits** (formato 34.30).
   * Imposible que ocurra desbordamiento (overflow) durante la sumatoria de productos, sin importar el número de taps.
   * Requiere instrucciones de 64 bits (`smlald`).

2. **`arm_fir_fast_q15()` (Rápida):**
   * Utiliza acumuladores de **32 bits** (formato 2.30) con un solo bit de guarda.
   * Ejecuta con menor cantidad de ciclos de reloj.
   * **Riesgo:** Si la suma de productos excede el rango de 32 bits, se produce desbordamiento con distorsión armónica severa (wrap-around). Para usarla con seguridad, los coeficientes deben escalarse por $1/\sum |h_k|$.

---

### Estrategia C: Rediseño Algorítmico y Presupuesto de Taps (Nivel Python)

Aun con procesamiento por bloques y optimización `-O3`, ningún microcontrolador de 150 MHz puede calcular un filtro FIR de 3864 taps a 48 kHz en tiempo real (requeriría al menos 3864 ciclos, y solo hay 3125 disponibles por muestra).

La consigna del trabajo práctico establece:
* Pasa Bajos: $F_c = 3600\text{ Hz}$, $A_{stop} \ge 30\text{ dB}$
* Pasa Altos: $F_c = 35\text{ Hz}$, $A_{stop} \ge 30\text{ dB}$
* Pasa Banda: $F_{c1} = 35\text{ Hz}$, $F_{c2} = 3500\text{ Hz}$, $A_{stop} \ge 30\text{ dB}$
* Elimina Banda: $F_r = 50\text{ Hz}$, $BW = 15\text{ Hz}$, $A_{stop} \ge 25\text{ dB}$

#### La física del ventaneo de Kaiser:
El orden del filtro se rige por:
$$N \approx \frac{A_{stop} - 7.95}{2.285 \cdot 2\pi \cdot (\Delta f / F_s)}$$

Si se mantiene $\Delta f = 20\text{ Hz}$ constante para todas las frecuencias de muestreo, $N$ se multiplica por 6 al pasar de 8 kHz a 48 kHz.

#### Solución Algorítmica Propuesta:
1. **Fijar un tope máximo de taps por diseño:**
   Establecer un límite estricto de taps dependiente de la frecuencia (ej. $\text{taps}_{\max} \le 128$ o $\le 256$).
2. **Ensanchamiento adaptativo de transiciones ($\Delta f$):**
   Para frecuencias altas (44k y 48k), ensanchar la banda de transición $\Delta f$ hacia las frecuencias adyacentes manteniendo la frecuencia central o el borde de rechazo exigido.
3. **Garantizar cumplimiento de $A_{stop}$ en la banda crítica:**
   Verificar que dentro de la banda de rechazo ($42.5\text{ Hz} - 57.5\text{ Hz}$ en el Notch) la atenuación se mantenga en los $25\text{ dB}$ requeridos, aun si la transición hacia las bandas de paso es más suave.

---

## 4. Cuadro Comparativo Global de Alternativas

| Alternativa | Complejidad de Implementación | Reducción de Carga CPU | Latencia Añadida | Modificaciones Requeridas |
| :--- | :---: | :---: | :---: | :--- |
| **1. Acotar taps en Python ($\text{taps}_{\max} \le 128 \dots 256$)** | **Baja** | **$70\% - 90\%$** | $0\text{ ms}$ | Modificar `disenio_filtros_tp2.py` y regenerar `filter_coeffs.h`. El firmware C no cambia. |
| **2. Procesamiento por bloques ($B = 16$ o $B = 32$)** | **Media** | **$60\% - 75\%$** | $<0.7\text{ ms}$ (@ 48k) | Modificar `pipeline.c` para acumular bloques y llamar al FIR fuera de la ISR crítica. |
| **3. `arm_fir_fast_q15()`** | **Baja** | **$15\% - 25\%$** | $0\text{ ms}$ | Reemplazar la llamada en `processing_stage.c` y verificar margen dinámico Q15. |
| **4. Combinación (1 + 2)** | **Media** | **$>95\%$** | $<0.7\text{ ms}$ | **Solución robusta definitiva**: FIRs esbeltos y procesador operando al $<25\%$ de carga total. |

---

## 5. Recomendación de Próximos Pasos

1. **Corto Plazo (Inmediato - Sin tocar código C):**
   Reintroducir el límite de taps y ensanche de transición en `disenio_filtros_tp2.py` y en el notebook `disenio_filtros_tp2.ipynb`. Regenerar `filter_coeffs.h` asegurando que ningún filtro supere los **128 a 192 taps**. Probar en la placa FRDM: el pipeline dejará de congestionarse de inmediato.
2. **Mediano Plazo (Arquitectura óptima):**
   Migrar `pipeline.c` a procesamiento en bloques de $B = 16$ o $B = 32$ muestras, habilitando la aceleración SIMD nativa de Cortex-M33 y eliminando la sobrecarga por muestra en la ISR.

