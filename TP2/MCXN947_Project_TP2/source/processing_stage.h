/*
 * processing_stage.h
 *
 * Etapa de procesamiento digital de señales (DSP).
 * Módulo portable e independiente del hardware de periféricos.
 * Implementa la estructura y el control de los filtros FIR del TP2.
 */

#ifndef PROCESSING_STAGE_H_
#define PROCESSING_STAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h" // tipo q15_t y funciones de CMSIS-DSP
#include "adc_stage.h" // Para usar sample_rate_t

/*
 * Tamaño de bloque del FIR, en muestras. pipeline_step() llama a
 * processing_stage_process_block() una vez por muestra del ADC (sin acumular),
 * así que queda en 1: cada muestra se filtra en el momento, sin latencia extra
 * de bufferizado.
 */
#define PROCESSING_BLOCK_SIZE 1U

/**
 * @brief Modos de operación del bloque de procesamiento (TP2).
 */
typedef enum {
    PROCESSING_MODE_BYPASS = 0,    /**< Entrada directa a salida (Bypass) */
    PROCESSING_MODE_LOWPASS,       /**< Filtro FIR Pasa Bajos (Fc = 3600 Hz, Astop = 30 dB) */
    PROCESSING_MODE_HIGHPASS,      /**< Filtro FIR Pasa Altos (Fc = 35 Hz, Astop = 30 dB) */
    PROCESSING_MODE_BANDPASS,      /**< Filtro FIR Pasa Banda (35 Hz - 3500 Hz, Astop = 30 dB) */
    PROCESSING_MODE_BANDSTOP,      /**< Filtro FIR Elimina Banda (Notch 50 Hz, BW = 15 Hz) */
    PROCESSING_MODE_COUNT
} processing_mode_t;

/**
 * @brief Inicializa la etapa de procesamiento DSP.
 */
void processing_stage_init(void);

/*
 * Los setters de modo y frecuencia solo registran el cambio: se aplica al comienzo del
 * próximo processing_stage_process_block(). Se pueden llamar desde cualquier ISR.
 */

/**
 * @brief Configura el modo de procesamiento de forma directa.
 * @param mode Modo deseado.
 */
void processing_stage_set_mode(processing_mode_t mode);

/**
 * @brief Avanza cíclicamente al siguiente modo de procesamiento.
 * @return El nuevo modo activo.
 */
processing_mode_t processing_stage_next_mode(void);

/**
 * @brief Obtiene el modo de procesamiento actual.
 */
processing_mode_t processing_stage_get_mode(void);

/**
 * @brief Obtiene el nombre en texto del modo dado (útil para UART).
 */
const char* processing_stage_get_mode_name(processing_mode_t mode);

/**
 * @brief Notifica a la etapa de procesamiento que la frecuencia de muestreo cambió,
 *        permitiendo reconfigurar los coeficientes del filtro FIR si está activo.
 * @param rate Nueva frecuencia de muestreo.
 */
void processing_stage_set_sample_rate(sample_rate_t rate);

/**
 * @brief Procesa un bloque de muestras contiguas.
 *        Antes de filtrar aplica el cambio de modo/frecuencia pendiente, si lo hay
 *        (reinicializa el FIR). Debe llamarse siempre desde el mismo contexto.
 * @param in Puntero al arreglo de entrada.
 * @param out Puntero al arreglo de salida.
 * @param length Cantidad de muestras a procesar.
 */
void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length);

#endif /* PROCESSING_STAGE_H_ */
