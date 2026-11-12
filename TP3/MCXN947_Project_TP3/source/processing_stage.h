/*
 * processing_stage.h
 *
 * Etapa de procesamiento digital de señales (DSP).
 * Módulo portable e independiente del hardware de periféricos.
 *
 * Procesa una ventana de N muestras (512, 1024 o 2048) en uno de dos modos:
 *  - FFT: espectro de magnitud con arm_cfft_q15 + arm_cmplx_mag_q15 (CMSIS-DSP), en dB x 100.
 *  - BYPASS: copia la ventana de entrada al buffer de salida, sin procesarla.
 */

#ifndef PROCESSING_STAGE_H_
#define PROCESSING_STAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h" // tipo q15_t y funciones de CMSIS-DSP

// Tamaño máximo de ventana (N) que puede procesar la etapa, en muestras.
#define PROCESSING_MAX_FRAME 2048U

// Piso del espectro en dB x 100: es lo que se devuelve para un bin de magnitud 0, que no
// tiene logaritmo. El bin no nulo más chico (1 LSB) da unos -78 dB, así que no se confunden.
#define PROCESSING_DB_FLOOR_X100 (-10000)

/**
 * @brief Modos de operación del bloque de procesamiento.
 */
typedef enum {
    PROCESSING_MODE_BYPASS = 0,    /**< La ventana de entrada se copia al buffer de salida */
    PROCESSING_MODE_FFT,           /**< Se calcula la FFT de la ventana */
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
 * @brief Obtiene el modo de procesamiento actual.
 */
processing_mode_t processing_stage_get_mode(void);

/**
 * @brief Obtiene el nombre en texto del modo dado (útil para UART).
 */
const char* processing_stage_get_mode_name(processing_mode_t mode);

/**
 * @brief Cantidad de coeficientes (taps) del filtro activo. Sin filtros FIR, siempre 0.
 *        Solo queda porque la UI del OLED (del TP2) lo sigue mostrando.
 */
uint16_t processing_stage_get_num_taps(void);

/**
 * @brief Procesa una ventana de N muestras y escribe el resultado en un buffer distinto
 *        del de entrada (la entrada no se modifica).
 *
 *        FFT: out[k] = magnitud del bin k en dBFS x 100, k = 0 .. N/2 - 1. El bin k
 *        corresponde a la frecuencia k * Fs / N. 0 dB es un tono senoidal de amplitud
 *        máxima (q15 = 1.0); un bin de magnitud 0 vale PROCESSING_DB_FLOOR_X100.
 *        BYPASS: out[i] = in[i], i = 0 .. N - 1 (muestras q15).
 *
 * @param mode Modo con el que se tomó la ventana.
 * @param in   N muestras q15 de entrada.
 * @param n    Tamaño de la ventana: 512, 1024 o 2048.
 * @param out  Buffer de salida, de al menos N valores.
 * @return Cantidad de valores escritos en out (N/2 en FFT, N en BYPASS), o 0 si los
 *         argumentos no son válidos (en ese caso out no se toca).
 */
uint32_t processing_stage_process_frame(processing_mode_t mode, const q15_t *in, uint32_t n, int16_t *out);

#endif /* PROCESSING_STAGE_H_ */
