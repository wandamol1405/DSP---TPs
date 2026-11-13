/*
 * signal_gen.h
 *
 * Generador de señal interna configurable para probar el pipeline (FFT y BYPASS)
 * sin generador de señales externo. Selecciona de dónde proviene la muestra de entrada:
 *
 *  - SIGNAL_SOURCE_ADC:          señal externa adquirida por el ADC (comportamiento por defecto).
 *  - SIGNAL_SOURCE_DAC_LOOPBACK: el DAC0 saca la señal generada y el ADC la vuelve a leer
 *                                mediante un cable J1[4] (P4_2, DAC0_OUT) -> J9[23] (P1_23, ADC1_A23).
 *  - SIGNAL_SOURCE_SYNTH:        loopback digital: la señal se inyecta directo en la ventana
 *                                sin pasar por el DAC ni el ADC (el DAC igual la saca para
 *                                poder observarla en el osciloscopio).
 *
 * La señal se genera mediante un acumulador de fase de 32 bits (DDS) con soporte para
 * cuatro formas de onda (SENO, CUADRADA, TRIANGULAR, SIERRA) y frecuencia configurable.
 */

#ifndef SIGNAL_GEN_H_
#define SIGNAL_GEN_H_

#include <stdint.h>
#include "arm_math.h" // tipo q15_t

// Amplitud pico como fracción de la escala completa q15 (< 1.0 para no recortar en DAC LOOPBACK).
#define SIGNAL_GEN_AMPLITUDE     0.8f
#define SIGNAL_GEN_AMPLITUDE_Q15 26214

// Rango válido de frecuencia configurable, en Hz.
#define SIGNAL_GEN_MIN_FREQ_HZ   1U
#define SIGNAL_GEN_MAX_FREQ_HZ   50000U

typedef enum {
    SIGNAL_SOURCE_ADC = 0,
    SIGNAL_SOURCE_DAC_LOOPBACK,
    SIGNAL_SOURCE_SYNTH,
    SIGNAL_SOURCE_COUNT
} signal_source_t;

typedef enum {
    SIGNAL_WAVE_SINE = 0,
    SIGNAL_WAVE_SQUARE,
    SIGNAL_WAVE_TRIANGLE,
    SIGNAL_WAVE_SAWTOOTH,
    SIGNAL_WAVE_COUNT
} signal_waveform_t;

/**
 * @brief Deja el generador en su estado inicial: fuente ADC, onda SENO, 1000 Hz.
 */
void signal_gen_init(void);

/**
 * @brief Avanza a la siguiente fuente: ADC -> DAC LOOPBACK -> SINTETICA -> ADC.
 */
signal_source_t signal_gen_next_source(void);

/**
 * @brief Fija la fuente de señal activa.
 */
void signal_gen_set_source(signal_source_t source);

/**
 * @brief Obtiene la fuente de señal activa.
 */
signal_source_t signal_gen_get_source(void);

/**
 * @brief Obtiene el nombre legible de la fuente dada.
 */
const char *signal_gen_get_source_name(signal_source_t source);

/**
 * @brief Avanza a la siguiente forma de onda: SENO -> CUADRADA -> TRIANGULAR -> SIERRA -> SENO.
 */
signal_waveform_t signal_gen_next_waveform(void);

/**
 * @brief Fija la forma de onda activa.
 */
void signal_gen_set_waveform(signal_waveform_t wave);

/**
 * @brief Obtiene la forma de onda activa.
 */
signal_waveform_t signal_gen_get_waveform(void);

/**
 * @brief Obtiene el nombre legible de la forma de onda dada.
 */
const char *signal_gen_get_waveform_name(signal_waveform_t wave);

/**
 * @brief Avanza al siguiente tono predefinido (250, 1000, 1004, 3000, 5000 Hz)
 *        y actualiza la frecuencia actual.
 * @return La nueva frecuencia en Hz.
 */
uint32_t signal_gen_next_tone(void);

/**
 * @brief Fija la frecuencia de la señal en Hz, limitada al rango [SIGNAL_GEN_MIN_FREQ_HZ, SIGNAL_GEN_MAX_FREQ_HZ].
 * @return La frecuencia aplicada en Hz.
 */
uint32_t signal_gen_set_freq_hz(uint32_t freq_hz);

/**
 * @brief Ajusta la frecuencia actual sumando delta_hz, saturando en el rango válido.
 * @return La nueva frecuencia en Hz.
 */
uint32_t signal_gen_step_freq_hz(int32_t delta_hz);

/**
 * @brief Obtiene la frecuencia actual del generador en Hz.
 */
uint32_t signal_gen_get_freq_hz(void);

/**
 * @brief Calcula la próxima muestra de la señal generada. Llamar una vez por período
 *        de muestreo (desde la ISR del ADC).
 * @param fs_hz Frecuencia de muestreo actual, para calcular el incremento de fase.
 * @return Muestra q15 con amplitud pico SIGNAL_GEN_AMPLITUDE.
 */
q15_t signal_gen_next_sample(uint32_t fs_hz);

#endif /* SIGNAL_GEN_H_ */
