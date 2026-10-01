# Plan de Implementación: Integración de Filtros FIR y Control en TP2

Este documento detalla paso a paso las modificaciones necesarias en el proyecto MCUXpresso (`MCXN947_Project_TP2`) para implementar los filtros FIR, reasignar el pulsador SW2 al ciclado de filtros/bypass, mantener el control de frecuencia de muestreo con SW3 y sincronizar los modos de procesamiento con la frecuencia activa.

---

## 1. Definición de la Estructura de Coeficientes (`filter_coeffs.h`)

Dado que los coeficientes finales se calcularán posteriormente y variarán según la frecuencia de muestreo ($8$, $16$, $22$, $44$ y $48\text{ kHz}$), se crea una cabecera para centralizar:

1. **Definiciones de número de coeficientes (Taps):**
   * Longitudes parametrizadas por tipo de filtro y frecuencia (ej. `NUM_TAPS_LP_8K`, etc.).
2. **Arreglos de coeficientes en formato Q15 (`q15_t`):**
   * Arreglos placeholder/mock con coeficientes normalizados o identidad para permitir compilación y testing inmediato.
3. **Estructura o tabla de descriptores de filtro:**
   * Empaquetar puntero a coeficientes y cantidad de taps por cada combinación `[filtro][frecuencia]`:
   ```c
   typedef struct {
       const q15_t *pCoeffs;
       uint16_t numTaps;
   } fir_filter_config_t;
   ```
4. **Reserva de buffer de estados CMSIS-DSP:**
   * Dimensión del buffer de estado: `max(numTaps) + blockSize - 1` (para procesamiento muestra a muestra, `blockSize = 1`).

---

## 2. Actualización de `processing_stage`

### 2.1. Archivo de cabecera (`processing_stage.h`)
- [ ] **Redefinir enum de modos:**
  ```c
  typedef enum {
      PROCESSING_MODE_BYPASS = 0,    /**< Entrada directa a salida (Bypass) */
      PROCESSING_MODE_LOWPASS,       /**< Filtro FIR Pasa Bajos (Fc = 3600 Hz, Astop = 30 dB) */
      PROCESSING_MODE_HIGHPASS,      /**< Filtro FIR Pasa Altos (Fc = 35 Hz, Astop = 30 dB) */
      PROCESSING_MODE_BANDPASS,      /**< Filtro FIR Pasa Banda (35 Hz - 3500 Hz, Astop = 30 dB) */
      PROCESSING_MODE_BANDSTOP,      /**< Filtro FIR Elimina Banda (Notch 50 Hz, BW = 15 Hz) */
      PROCESSING_MODE_COUNT
  } processing_mode_t;
  ```
- [ ] **Actualizar/Agregar funciones de la API pública:**
  * `processing_mode_t processing_stage_next_mode(void);`: Avanza cíclicamente al siguiente modo (`(mode + 1) % PROCESSING_MODE_COUNT`) y actualiza el filtro activo.
  * `const char* processing_stage_get_mode_name(processing_mode_t mode);`: Retorna el nombre en texto para impresión por UART.
  * `void processing_stage_set_sample_rate(sample_rate_t rate);`: Notifica un cambio de frecuencia para reconfigurar los coeficientes e inicializar el estado del filtro.

### 2.2. Implementación (`processing_stage.c`)
- [ ] **Instancia CMSIS-DSP:**
  * Declarar variable de instancia `arm_fir_instance_q15 s_fir_instance;` y buffer de estado `q15_t s_fir_state[...]`.
- [ ] **Lógica de reconfiguración:**
  * Función estática `reconfigure_filter(processing_mode_t mode, sample_rate_t rate)` que obtiene los coeficientes correspondientes y llama a `arm_fir_init_q15(&s_fir_instance, config.numTaps, config.pCoeffs, s_fir_state, 1U)`.
- [ ] **Procesamiento de muestra (`processing_stage_process_sample`):**
  * Si el modo es `PROCESSING_MODE_BYPASS`, retornar directamente `in_sample`.
  * Si es cualquier filtro, invocar `arm_fir_q15(&s_fir_instance, &in_sample, &out_sample, 1U)` y retornar `out_sample`.
- [ ] Eliminar los bloques no utilizados del TP1 (`INVERT`, `FLOAT_CONV`, `GAIN`).

---

## 3. Coordinación en `pipeline` (`pipeline.h` y `pipeline.c`)

### 3.1. Buffer de Salida Diferenciado
- [ ] Implementar un buffer circular de salida independiente `s_out_circ_buffer` con su almacenamiento `s_out_buffer_storage[PIPELINE_BUFFER_SIZE]` para cumplir con el requisito de almacenamiento antes del DAC.

### 3.2. Sincronización de Frecuencia y Modo
- [ ] En `pipeline_init()`: inicializar `processing_stage` con la frecuencia de muestreo inicial.
- [ ] En `pipeline_next_sample_rate()` y `pipeline_set_sample_rate()`: llamar a `processing_stage_set_sample_rate()` para sincronizar los filtros.
- [ ] Exponer una función pasarela:
  * `processing_mode_t pipeline_next_processing_mode(void);` que invoque a `processing_stage_next_mode()`.
  * `processing_mode_t pipeline_get_processing_mode(void);`
  * `const char* pipeline_get_processing_mode_name(void);`

---

## 4. Reasignación de Pulsadores y Comandos UART (`MCXN947_Project_TP2.c`)

### 4.1. Pulsador SW2 (GPIO0_INT_1_IRQHANDLER)
- [ ] **Cambiar comportamiento del botón:**
  * Quitar llamada a `pipeline_toggle_run_stop()`.
  * Invocar `pipeline_next_processing_mode()`.
  * Levantar un flag no bloqueante para la consola: `mode_print_flag = true;`.

### 4.2. Pulsador SW3 (GPIO0_INT_0_IRQHANDLER)
- [ ] **Mantener intacto:** Sigue alternando entre frecuencias de muestreo (8k, 16k, 22k, 44k, 48k) y cambiando el color del LED RGB.

### 4.3. Consola UART e Interfaz de Comandos
- [ ] **Comando `'m'` / `'M'`:**
  * Actualizar para ciclar entre los 5 modos (`BYPASS`, `LOWPASS`, `HIGHPASS`, `BANDPASS`, `BANDSTOP`).
- [ ] **Comando `'r'` / `'R'`:**
  * Mantenerlo por UART para quien desee pausar/congelar la adquisición en el buffer, pero sin asignar al botón físico.
- [ ] **Loop principal (`while(1)`):**
  * Atender `mode_print_flag`: imprimir mensaje `"[DSP] Modo/Filtro activo: %s\r\n"` con el nombre devuelto por `processing_stage_get_mode_name()`.
- [ ] **Menú de ayuda (`'h'`):**
  * Actualizar el texto indicando los 5 modos disponibles para el comando `'m'` y el funcionamiento de los botones de la placa.

---

## 5. Matriz de Verificación y Criterios de Éxito

| Prueba | Acción | Resultado Esperado |
| :--- | :--- | :--- |
| **Ciclado de filtros por botón** | Presionar SW2 | Cambia secuencialmente: Bypass -> Lowpass -> Highpass -> Bandpass -> Bandstop -> Bypass. Notificación por UART. |
| **Ciclado por UART** | Enviar carácter `'m'` | Mismo comportamiento y estado que el pulsador SW2. |
| **Cambio de frecuencia** | Presionar SW3 | Cambia frecuencia de muestreo, actualiza color del LED RGB y reinicializa el estado del filtro en la nueva frecuencia. |
| **Integridad de audio/señal** | Inyectar señal (generador) y medir con osciloscopio | En Bypass la señal sale idéntica. Al conmutar filtros, la señal se procesa sin cuelgues ni saturación. |
