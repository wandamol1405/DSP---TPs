/*
 * processing_stage.h
 *
 * Etapa de procesamiento digital de señales (DSP).
 * Módulo portable e independiente del hardware de periféricos.
 *
 * TP3: los filtros FIR del TP2 se quitaron. Por ahora la etapa solo hace bypass;
 * la FFT se agrega en la issue #15.
 */

#ifndef PROCESSING_STAGE_H_
#define PROCESSING_STAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h" // tipo q15_t y funciones de CMSIS-DSP
#include "adc_stage.h" // Para usar sample_rate_t

/**
 * @brief Modos de operación del bloque de procesamiento.
 */
typedef enum {
    PROCESSING_MODE_BYPASS = 0,    /**< Entrada directa a salida (Bypass) */
    PROCESSING_MODE_COUNT
} processing_mode_t;

/**
 * @brief Inicializa la etapa de procesamiento DSP.
 */
void processing_stage_init(void);

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
 * @brief Cantidad de coeficientes (taps) del filtro activo. Sin filtros FIR, siempre 0.
 */
uint16_t processing_stage_get_num_taps(void);

/**
 * @brief Notifica a la etapa de procesamiento que la frecuencia de muestreo cambió.
 * @param rate Nueva frecuencia de muestreo.
 */
void processing_stage_set_sample_rate(sample_rate_t rate);

/**
 * @brief Procesa un bloque de muestras contiguas (por ahora, copia la entrada a la salida).
 * @param in Puntero al arreglo de entrada.
 * @param out Puntero al arreglo de salida.
 * @param length Cantidad de muestras a procesar.
 */
void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length);

#endif /* PROCESSING_STAGE_H_ */
