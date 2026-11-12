/*
 * signal_gen.h
 *
 * EJEMPLO (rama tp3-ejemplo-senal-interna, no se integra al proyecto).
 *
 * Generador de tono senoidal interno, para probar la FFT sin el generador de señales
 * externo. Elige de dónde sale la muestra que entra a la ventana de N muestras:
 *
 *  - SIGNAL_SOURCE_ADC:          como siempre, la señal del generador externo en el ADC.
 *  - SIGNAL_SOURCE_DAC_LOOPBACK: el DAC0 saca el tono generado y el ADC lo vuelve a leer.
 *                                Hace falta un cable de J1[4] (P4_2, DAC0_OUT) a J9[23]
 *                                (P1_23, ADC1_A23). Pasa por toda la cadena analógica.
 *  - SIGNAL_SOURCE_SYNTH:        el tono se escribe directo en la ventana, sin pasar por
 *                                el DAC ni el ADC (sin señal analógica de por medio). El
 *                                DAC igual lo saca, para verlo en el osciloscopio.
 *
 * El tono se calcula muestra a muestra con sinf() a partir de un acumulador de fase de
 * 32 bits (DDS), así la frecuencia no depende de Fs: al cambiar Fs se recalcula el
 * incremento de fase y el tono sigue en los mismos Hz.
 */

#ifndef SIGNAL_GEN_H_
#define SIGNAL_GEN_H_

#include <stdint.h>
#include "arm_math.h" // q15_t

// Amplitud pico del tono, como fracción de la escala completa q15. Menor que 1 para que
// en DAC LOOPBACK no se recorte contra los rieles del DAC ni del ADC.
#define SIGNAL_GEN_AMPLITUDE 0.8f

typedef enum {
    SIGNAL_SOURCE_ADC = 0,
    SIGNAL_SOURCE_DAC_LOOPBACK,
    SIGNAL_SOURCE_SYNTH,
    SIGNAL_SOURCE_COUNT
} signal_source_t;

/**
 * @brief Deja el generador en su estado inicial: fuente ADC, primer tono de la lista.
 */
void signal_gen_init(void);

/**
 * @brief Avanza a la siguiente fuente: ADC -> DAC LOOPBACK -> SINTETICA -> ADC.
 */
signal_source_t signal_gen_next_source(void);

signal_source_t signal_gen_get_source(void);
const char *signal_gen_get_source_name(signal_source_t source);

/**
 * @brief Avanza al siguiente tono de la lista (ver s_tones_hz en signal_gen.c).
 * @return La frecuencia del tono nuevo, en Hz.
 */
uint32_t signal_gen_next_tone(void);

/**
 * @brief Frecuencia del tono actual, en Hz.
 */
uint32_t signal_gen_get_tone_hz(void);

/**
 * @brief Calcula la próxima muestra del tono. Llamar una vez por período de muestreo
 *        (desde la ISR del ADC).
 * @param fs_hz Frecuencia de muestreo actual, para el incremento de fase.
 * @return Muestra q15 del tono (amplitud SIGNAL_GEN_AMPLITUDE).
 */
q15_t signal_gen_next_sample(uint32_t fs_hz);

#endif /* SIGNAL_GEN_H_ */
