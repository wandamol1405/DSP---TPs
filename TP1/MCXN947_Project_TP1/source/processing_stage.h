/*
 * processing_stage.h
 *
 * Etapa de procesamiento digital de señales (DSP).
 * Módulo portable e independiente del hardware de periféricos.
 * Diseñado para alojar futuros algoritmos (filtros FIR/IIR, FFT, CMSIS-DSP).
 */

#ifndef PROCESSING_STAGE_H_
#define PROCESSING_STAGE_H_

#include <stdint.h>   // tipo uint32_t
#include <stdbool.h>  // (reservado para futura extensión de la API, no usado aún)
#include "arm_math.h" // tipo q15_t y funciones de conversión (arm_q15_to_float, arm_float_to_q15)

/**
 * @brief Modos de operación del bloque de procesamiento.
 */
typedef enum {
    PROCESSING_MODE_PASSTHROUGH = 0,    /**< Passthrough directo (identidad) */
    PROCESSING_MODE_FLOAT_CONV,         /**< Conversión Q15 -> float -> Q15 (validación numérica) */
    PROCESSING_MODE_INVERT,             /**< Inversión de signo (fase 180°) */
    PROCESSING_MODE_GAIN,               /**< Escalamiento / Ganancia configurable */
    PROCESSING_MODE_CUSTOM              /**< Hook para futuros filtros (TP2 / TP3) */
} processing_mode_t;

/**
 * @brief Inicializa la etapa de procesamiento DSP.
 */
void processing_stage_init(void);

/**
 * @brief Configura el modo de procesamiento.
 * @param mode Modo deseado.
 */
void processing_stage_set_mode(processing_mode_t mode);

/**
 * @brief Obtiene el modo de procesamiento actual.
 */
processing_mode_t processing_stage_get_mode(void);

/**
 * @brief Procesa una única muestra de entrada (muestra a muestra).
 * @param in_sample Muestra de entrada en formato Q15.
 * @return Muestra procesada en formato Q15.
 */
q15_t processing_stage_process_sample(q15_t in_sample);

/**
 * @brief Procesa un bloque de muestras contiguas (preparado para filtros por bloques o ventanas FFT).
 * @param in Puntero al arreglo de entrada.
 * @param out Puntero al arreglo de salida.
 * @param length Cantidad de muestras a procesar.
 */
void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length);

/**
 * @brief Convierte una muestra Q15 a punto flotante (-1.0f a +0.999969f).
 * @param sample Muestra en Q15.
 * @return Valor en punto flotante.
 */
float processing_stage_q15_to_float(q15_t sample);

/**
 * @brief Convierte un valor de punto flotante a Q15 saturando si excede el rango.
 * @param value Valor flotante entre -1.0f y +1.0f.
 * @return Muestra en Q15.
 */
q15_t processing_stage_float_to_q15(float value);

#endif /* PROCESSING_STAGE_H_ */

