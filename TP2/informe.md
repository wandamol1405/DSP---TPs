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

En `MCXN947_Project_TP2.c` se adaptaron los 
A partir de la arquitectura modular desarrollada en el TP1, se modificaron las etapas de procesamiento, coordinación y control para incorporar filtros FIR y permitir su selección en tiempo de ejecución.

### Procesamiento digital y coeficientes

Se incorporó la librería **CMSIS-DSP** para realizar el procesamiento de señales mediante rutinas optimizadas para arquitecturas ARM Cortex-M. Los coeficientes y tamaños de los filtros se centralizaron en el módulo `filter_coeffs.h`, utilizando la estructura `fir_filter_config_t` para asociar cada configuración de filtro con sus respectivos parámetros según el tipo de filtro y la frecuencia de muestreo seleccionada.

El módulo `processing_stage` implementa los cinco modos de operación: `BYPASS`, `LOWPASS`, `HIGHPASS`, `BANDPASS` y `BANDSTOP`. Para el procesamiento FIR se utiliza la estructura `arm_fir_instance_q15` de CMSIS-DSP. Ante un cambio de filtro o de frecuencia de muestreo, se reinicializa la instancia mediante `arm_fir_init_q15()`, limpiando el buffer de estados para evitar que las muestras anteriores afecten el procesamiento con la nueva configuración.

### Gestión del flujo de datos

En `pipeline.c` se incorporó el buffer circular de salida `s_out_circ_buffer`, independiente del buffer de entrada y con una capacidad de 512 muestras. Cada muestra adquirida se procesa mediante `processing_stage_process_sample()` y el resultado se almacena en el buffer de salida antes de enviarse al DAC. Este flujo se mantiene tanto durante la adquisición en modo `RUN` como durante el volcado continuo en modo `STOP`.

El pipeline también coordina la actualización de la etapa de procesamiento cuando se modifica la frecuencia de muestreo, manteniendo sincronizadas ambas configuraciones.

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
