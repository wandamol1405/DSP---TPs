<p align="center">
  <img src="./images/Isologotipo_FCEFyN_y_UNC-original_Sin_fondo-Con_bajada.png" alt="Isologotipo de la Facultad de Ciencias Exactas, Físicas y Naturales y la Universidad Nacional de Córdoba" width="70%" />
</p>

# TP2 — Filtros FIR con MCXN947

Realizar el diseños e implementación de filtros FIR usando procesamiento por muestras.

|              |                                   |
| ------------ | --------------------------------- |
| **Materia:** | Procesamiento Digital de Señales  |
| **Carrera:** | Ingeniería en Computación         |
| **Alumnos:** | García, Lautaro Misael            |
|              | Molina, María Wanda               |
|              | Renaudo Gaggioli, Valentino       |
|              | Verdú, Melisa Noel                |
| **Fecha:**   | Octubre de 2026                   |

---

## Índice

1. [Objetivo](#objetivo)
2. [Marco teórico](#marco-teórico)
3. [Configuración del ambiente](#configuración-del-ambiente)
4. [Diseño de la aplicación](#diseño-de-la-aplicación)
5. [Código fuente relevante](#código-fuente-relevante)
6. [Demostración y resultados](#demostración-y-resultados)
7. [Conclusiones](#conclusiones)

## Objetivo

---

## Marco teórico

---

## Configuración del ambiente

Igual que en [TP1](../TP1/informe.md).

---

## Diseño de la aplicación

### Arquitectura del sistema de filtrado

A partir de la arquitectura modular desarrollada en el TP1, se rediseñaron las etapas de procesamiento, coordinación y control para incorporar filtros digitales FIR y permitir su conmutación dinámica en tiempo de ejecución.

### Procesamiento digital y coeficientes

Se incorporó la librería **CMSIS-DSP** para realizar el filtrado digital en aritmética de punto fijo en formato Q15 (`q15_t`), aprovechando el soporte de hardware del núcleo ARM Cortex-M33 del microcontrolador MCXN947. Los coeficientes y metadatos de los filtros se centralizan en el archivo autogenerado `filter_coeffs.h`, utilizando la estructura `fir_filter_config_t` para asociar los coeficientes ordenados temporalmente en reversa y la longitud `numTaps` para cada filtro y frecuencia de muestreo.

El módulo `processing_stage` implementa los cinco modos operativos del sistema: `BYPASS`, `LOWPASS`, `HIGHPASS`, `BANDPASS` y `BANDSTOP`. Para el filtrado se utiliza la estructura `arm_fir_instance_q15` de CMSIS-DSP:

* **Transición de muestra a muestra a procesamiento por bloques:** Inicialmente, el filtro se ejecutaba muestra a muestra ($B = 1$). No obstante, este enfoque presentaba dos limitaciones críticas: omitía completamente el camino vectorial SIMD de CMSIS-DSP (que requiere bloques múltiplos de 4) y obligaba a desplazar en memoria $(numTaps - 1)$ muestras de estado en la RAM en cada muestra ($48.000$ copias/s a $48\text{ kHz}$). Para resolver la congestión del pipeline, se adoptó **procesamiento por bloques** con tamaño fijo $B = 32$ muestras (`PROCESSING_BLOCK_SIZE`), ejecutando `processing_stage_process_block()` en lugar de procesar muestra a muestra.
* **Reconfiguración segura:** Ante un cambio de filtro o de frecuencia de muestreo, se difiere la reinicialización de la instancia mediante `arm_fir_init_q15()` hasta el límite entre bloques (`s_reconfig_pending`), garantizando que la memoria de estados no se modifique de forma concurrente mientras el filtro está computando.

### Gestión del flujo de datos y desacoplamiento con PendSV

El pipeline gestiona el flujo de señales desacoplando la adquisición temporal estricta del cómputo intensivo del filtro digital:

1. **Estructura Ping-Pong:** En `pipeline.c` se implementaron buffers dobles para entrada y salida:
   * `s_blk_in[2][32]`: almacena las muestras crudas provenientes del ADC.
   * `s_blk_out[2][32]`: almacena las muestras ya filtradas para enviar al DAC.
   * `s_out_circ_buffer`: buffer circular de 512 muestras para inspección o reproducción en modo `STOP`.
2. **Interrupción del ADC estrictamente determinística $O(1)$:** La rutina de servicio del convertidor analógico-digital (`ADC1_IRQHANDLER`, prioridad alta 0) no realiza cómputo de convolución. En cada período de muestreo ($T_s$), la ISR:
   * Escribe la muestra previamente filtrada del bloque actual al DAC (`dac_stage_write_sample()`).
   * Almacena la nueva muestra del ADC en el buffer ping-pong activo.
   * Incrementa el índice de bloque. Esta secuencia insume menos de $0.5\ \mu\text{s}$, eliminando todo riesgo de jitter en la adquisición.
3. **Desacoplamiento asíncrono con PendSV:** Al completarse la muestra número 32 del bloque:
   * La ISR conmuta el índice de ping-pong (`s_blk_cur = cur ^ 1`).
   * Activa la excepción del sistema del núcleo ARM `PendSV` escribiendo en el registro de control de interrupciones:
     ```c
     SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
     ```
   * `PendSV_Handler()` está configurado con la menor prioridad del NVIC (`priority 7`). Por lo tanto, el núcleo ejecuta el filtrado del bloque completo en segundo plano (`pipeline_process_block()`), y cualquier nueva muestra del ADC puede interrumpir a `PendSV` en cualquier instante sin bloquear la entrada/salida de audio.
4. **Detección de sobrecarga (Overrun Protection):** Si la convolución de un bloque no logra completarse antes de que se llenen las 32 muestras del bloque siguiente (`s_blk_busy == true`), el pipeline registra un evento de sobrecarga (`s_overrun_count++`), activa la bandera `s_overrun_pending` y conmuta de emergencia a modo `BYPASS`, alertando al usuario por UART y previniendo caídas del sistema o glitches severos en el DAC.

### Nota técnica: Diagnóstico a bajo nivel y optimizaciones DSP (SIMD y memcpy)

Durante las pruebas iniciales en la placa FRDM-MCXN947 con frecuencias de muestreo elevadas (22, 44 y 48 kHz), los filtros con más de 700 coeficientes disparaban el mecanismo de sobrecarga. Se llevó a cabo un diagnóstico a bajo nivel desensamblando los binarios de compilación (`.axf`) con la herramienta `arm-none-eabi-objdump -d`, identificándose dos cuellos de botella fundamentales:

1. **Ausencia de instrucciones SIMD dual-MAC (`ARM_MATH_LOOPUNROLL`):**
   * *Diagnóstico:* Al inspeccionar la función `arm_fir_q15` en el binario compilado, se descubrió que no contenía ninguna instrucción vectorial `smlald` ni `smlaldx`. El código CMSIS-DSP caía por defecto en un bucle escalar de 32 bits muestra a muestra (`acc0 += (q31_t) *px++ * *pb++`), consumiendo entre 3 y 5 ciclos de reloj por tap.
   * *Solución:* CMSIS-DSP requiere la macro `ARM_MATH_LOOPUNROLL` para habilitar el algoritmo desenrollado de 4 muestras en paralelo. Se incorporó `#define ARM_MATH_LOOPUNROLL 1` en `arm_math_types.h`.
2. **Cuello de botella de llamadas a `memcpy` por optimización de tamaño:**
   * *Diagnóstico:* En la configuración `Release`, el compilador utilizaba el flag de optimización por tamaño `-Os`. En `arm_math_memory.h`, las macros de lectura no alineada `read_q15x2` invocaban la función estándar `memcpy(&val, pQ15, 4)`. Debido a `-Os`, GCC optaba por emitir llamadas a subrutina de librería (`bl memcpy`) en lugar de expandir la instrucción en línea. Para un filtro de 1062 taps y un bloque de 32 muestras, esto generaba más de **4.000 llamadas a función `memcpy` por bloque**, desperdiciando más de $120.000$ ciclos de reloj únicamente en prólogos y epílogos de llamada.
   * *Solución:* Se reemplazó el acceso en `arm_math_memory.h` por el intrínseco del compilador `__builtin_memcpy`, forzando al compilador a generar instrucciones de carga no alineada de 32 bits de hardware (`ldr`) en un único ciclo de reloj sin llamadas a función. Asimismo, se configuró el nivel de optimización en `-O3` (`gnu.c.optimization.level.most`) en la configuración Release del proyecto.

**Verificación y resultados:**
Tras estas optimizaciones a nivel ensamblador, la rutina interna de convolución FIR quedó reducida exclusivamente a lecturas hardware de 32 bits y operaciones de multiplicación y acumulación doble en un solo ciclo:
```assembly
3550:   ldr     r6, [r7, #0]
3554:   smlald  r4, r5, sl, r6   @ Dual-MAC en 1 ciclo
3558:   smlald  r8, r9, r3, r6   @ Dual-MAC en 1 ciclo
```
Con esta mejora, el filtro Pasa Altos de **1062 taps** a **44,1 kHz** pasó a ejecutarse en tiempo real de forma continua (insumiendo menos de $727\ \mu\text{s}$ por bloque de 32 muestras), alcanzando una tasa de éxito de 16 sobre 20 combinaciones de filtro y frecuencia en la placa.

### Interfaz de usuario y comunicación UART

En `MCXN947_Project_TP2.c` se adaptaron los controles físicos y los comandos UART para gestionar los nuevos modos de procesamiento sin bloquear las rutinas de servicio de interrupción.

El pulsador SW2 y el comando UART `m` permiten alternar entre los modos de filtrado. Cada cambio activa la bandera `dsp_mode_print_flag`, que permite al programa principal informar por UART el modo seleccionado. Por su parte, SW3 conserva la función de alternar entre las cinco frecuencias de muestreo disponibles (8, 16, 22, 44 y 48 kHz), actualizando también el color del LED RGB.

De esta manera, la aplicación mantiene una separación funcional entre la adquisición de muestras, la coordinación de los buffers, el procesamiento digital mediante CMSIS-DSP y la comunicación con el usuario.

---

## Código fuente relevante

---

## Demostración y resultados

---

## Conclusiones
