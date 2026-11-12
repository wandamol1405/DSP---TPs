/*
 * signal_gen.c
 *
 * EJEMPLO (rama tp3-ejemplo-senal-interna, no se integra al proyecto).
 * Implementación del generador de tono interno. Ver signal_gen.h.
 */

#include "signal_gen.h"
#include <math.h> // sinf

// Tonos que recorre el comando 'w'. Elegidos para los ensayos de la consigna, pensando en
// Fs = 8 kHz y N = 1024 (Δf = 7,8125 Hz):
//  -  250 Hz: tono bajo, bin 32.
//  - 1000 Hz: cae justo en el bin 128 (muestreo coherente): sin fuga espectral.
//  - 1004 Hz: cae a mitad de camino entre los bins 128 y 129: fuga espectral máxima.
//  - 3000 Hz: cerca de Fs/2 a 8 kHz.
//  - 5000 Hz: por encima de Fs/2 a 8 kHz, aparece reflejado en 3000 Hz (aliasing); a
//             16 kHz o más se ve en su lugar.
static const uint32_t s_tones_hz[] = {250U, 1000U, 1004U, 3000U, 5000U};
#define TONE_COUNT (sizeof(s_tones_hz) / sizeof(s_tones_hz[0]))

static volatile signal_source_t s_source = SIGNAL_SOURCE_ADC;
static volatile uint32_t s_tone_idx = 1U; // arranca en 1000 Hz

// Estado del DDS. Solo lo toca signal_gen_next_sample (ISR del ADC).
static uint32_t s_phase = 0U;     // fase actual: 0 .. 2^32 - 1 equivale a 0 .. 2π
static uint32_t s_phase_inc = 0U; // incremento por muestra: f / Fs * 2^32
static uint32_t s_inc_fs_hz = 0U; // con qué Fs y tono se calculó s_phase_inc
static uint32_t s_inc_tone_hz = 0U;

static const char *const s_source_names[SIGNAL_SOURCE_COUNT] = {
    [SIGNAL_SOURCE_ADC] = "ADC (generador externo)",
    [SIGNAL_SOURCE_DAC_LOOPBACK] = "DAC LOOPBACK (J1[4] -> J9[23])",
    [SIGNAL_SOURCE_SYNTH] = "SINTETICA (sin ADC)",
};

void signal_gen_init(void) {
    s_source = SIGNAL_SOURCE_ADC;
    s_tone_idx = 1U;
    s_phase = 0U;
    s_phase_inc = 0U;
    s_inc_fs_hz = 0U;
    s_inc_tone_hz = 0U;
}

signal_source_t signal_gen_next_source(void) {
    s_source = (signal_source_t)((s_source + 1) % SIGNAL_SOURCE_COUNT);
    return s_source;
}

signal_source_t signal_gen_get_source(void) { return s_source; }

const char *signal_gen_get_source_name(signal_source_t source) {
    if ((unsigned int)source < (unsigned int)SIGNAL_SOURCE_COUNT) {
        return s_source_names[source];
    }
    return "?";
}

uint32_t signal_gen_next_tone(void) {
    s_tone_idx = (s_tone_idx + 1U) % TONE_COUNT;
    return s_tones_hz[s_tone_idx];
}

uint32_t signal_gen_get_tone_hz(void) { return s_tones_hz[s_tone_idx]; }

q15_t signal_gen_next_sample(uint32_t fs_hz) {
    uint32_t tone_hz = s_tones_hz[s_tone_idx];

    // El incremento solo se recalcula cuando cambia Fs o el tono (la división de 64 bits
    // es por software, no conviene hacerla en cada muestra)
    if ((fs_hz != s_inc_fs_hz) || (tone_hz != s_inc_tone_hz)) {
        s_phase_inc = (fs_hz != 0U) ? (uint32_t)(((uint64_t)tone_hz << 32) / fs_hz) : 0U;
        s_inc_fs_hz = fs_hz;
        s_inc_tone_hz = tone_hz;
    }

    // Fase de 32 bits -> ángulo en radianes. El float tiene 24 bits de mantisa: el error de
    // fase es ~2^-24 de vuelta, muy por debajo del piso del espectro en q15.
    float angle = (float)s_phase * (2.0f * PI / 4294967296.0f);
    float y = SIGNAL_GEN_AMPLITUDE * 32767.0f * sinf(angle);

    s_phase += s_phase_inc; // el desborde de 32 bits es la vuelta de 2π

    // Redondeo al entero más cercano (con |y| < 32767 no hace falta saturar)
    return (q15_t)((y >= 0.0f) ? (y + 0.5f) : (y - 0.5f));
}
