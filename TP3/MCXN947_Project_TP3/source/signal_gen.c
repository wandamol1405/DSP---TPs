/*
 * signal_gen.c
 *
 * Implementación del generador de señal interna mediante acumulador de fase de 32 bits (DDS).
 */

#include "signal_gen.h"
#include <math.h> // sinf

// Tonos predefinidos que recorre el comando 'w' (útiles para los ensayos a Fs = 8 kHz y N = 1024).
static const uint32_t s_tones_hz[] = {250U, 1000U, 1004U, 3000U, 5000U};
#define TONE_COUNT (sizeof(s_tones_hz) / sizeof(s_tones_hz[0]))

static volatile signal_source_t s_source = SIGNAL_SOURCE_ADC;
static volatile signal_waveform_t s_wave = SIGNAL_WAVE_SINE;
static volatile uint32_t s_tone_idx = 1U; // arranca en 1000 Hz
static volatile uint32_t s_freq_hz = 1000U;

// Estado del DDS (solo lo toca signal_gen_next_sample desde la ISR del ADC).
static uint32_t s_phase = 0U;      // fase actual: 0 .. 2^32 - 1 equivale a 0 .. 2*pi
static uint32_t s_phase_inc = 0U;  // incremento por muestra: f / Fs * 2^32
static uint32_t s_inc_fs_hz = 0U;  // Fs con la que se calculó s_phase_inc
static uint32_t s_inc_freq_hz = 0U;

static const char *const s_source_names[SIGNAL_SOURCE_COUNT] = {
    [SIGNAL_SOURCE_ADC]          = "ADC (generador externo)",
    [SIGNAL_SOURCE_DAC_LOOPBACK] = "DAC LOOPBACK (J1[4] -> J9[23])",
    [SIGNAL_SOURCE_SYNTH]        = "SINTETICA (sin ADC)",
};

static const char *const s_waveform_names[SIGNAL_WAVE_COUNT] = {
    [SIGNAL_WAVE_SINE]     = "SENO",
    [SIGNAL_WAVE_SQUARE]   = "CUADRADA",
    [SIGNAL_WAVE_TRIANGLE] = "TRIANGULAR",
    [SIGNAL_WAVE_SAWTOOTH] = "SIERRA",
};

static uint32_t clamp_freq_hz(int64_t freq_hz) {
    if (freq_hz < (int64_t)SIGNAL_GEN_MIN_FREQ_HZ) {
        return SIGNAL_GEN_MIN_FREQ_HZ;
    }
    if (freq_hz > (int64_t)SIGNAL_GEN_MAX_FREQ_HZ) {
        return SIGNAL_GEN_MAX_FREQ_HZ;
    }
    return (uint32_t)freq_hz;
}

void signal_gen_init(void) {
    s_source = SIGNAL_SOURCE_ADC;
    s_wave = SIGNAL_WAVE_SINE;
    s_tone_idx = 1U;
    s_freq_hz = s_tones_hz[s_tone_idx];
    s_phase = 0U;
    s_phase_inc = 0U;
    s_inc_fs_hz = 0U;
    s_inc_freq_hz = 0U;
}

signal_source_t signal_gen_next_source(void) {
    s_source = (signal_source_t)((s_source + 1U) % SIGNAL_SOURCE_COUNT);
    return s_source;
}

void signal_gen_set_source(signal_source_t source) {
    if ((unsigned int)source < (unsigned int)SIGNAL_SOURCE_COUNT) {
        s_source = source;
    }
}

signal_source_t signal_gen_get_source(void) {
    return s_source;
}

const char *signal_gen_get_source_name(signal_source_t source) {
    if ((unsigned int)source < (unsigned int)SIGNAL_SOURCE_COUNT) {
        return s_source_names[source];
    }
    return "?";
}

signal_waveform_t signal_gen_next_waveform(void) {
    s_wave = (signal_waveform_t)((s_wave + 1U) % SIGNAL_WAVE_COUNT);
    return s_wave;
}

void signal_gen_set_waveform(signal_waveform_t wave) {
    if ((unsigned int)wave < (unsigned int)SIGNAL_WAVE_COUNT) {
        s_wave = wave;
    }
}

signal_waveform_t signal_gen_get_waveform(void) {
    return s_wave;
}

const char *signal_gen_get_waveform_name(signal_waveform_t wave) {
    if ((unsigned int)wave < (unsigned int)SIGNAL_WAVE_COUNT) {
        return s_waveform_names[wave];
    }
    return "?";
}

uint32_t signal_gen_next_tone(void) {
    s_tone_idx = (s_tone_idx + 1U) % TONE_COUNT;
    s_freq_hz = s_tones_hz[s_tone_idx];
    return s_freq_hz;
}

uint32_t signal_gen_set_freq_hz(uint32_t freq_hz) {
    uint32_t clamped = clamp_freq_hz((int64_t)freq_hz);
    s_freq_hz = clamped;
    return clamped;
}

uint32_t signal_gen_step_freq_hz(int32_t delta_hz) {
    uint32_t clamped = clamp_freq_hz((int64_t)s_freq_hz + (int64_t)delta_hz);
    s_freq_hz = clamped;
    return clamped;
}

uint32_t signal_gen_get_freq_hz(void) {
    return s_freq_hz;
}

q15_t signal_gen_next_sample(uint32_t fs_hz) {
    uint32_t freq_hz = s_freq_hz;
    signal_waveform_t wave = s_wave;

    // Recalcular el incremento de fase solo cuando cambia Fs o la frecuencia del tono.
    if ((fs_hz != s_inc_fs_hz) || (freq_hz != s_inc_freq_hz)) {
        s_phase_inc = (fs_hz != 0U) ? (uint32_t)(((uint64_t)freq_hz << 32) / fs_hz) : 0U;
        s_inc_fs_hz = fs_hz;
        s_inc_freq_hz = freq_hz;
    }

    q15_t sample = 0;
    switch (wave) {
    case SIGNAL_WAVE_SINE: {
        float angle = (float)s_phase * (2.0f * PI / 4294967296.0f);
        float y = SIGNAL_GEN_AMPLITUDE * 32767.0f * sinf(angle);
        sample = (q15_t)((y >= 0.0f) ? (y + 0.5f) : (y - 0.5f));
        break;
    }

    case SIGNAL_WAVE_SQUARE:
        sample = (s_phase < 0x80000000U) ? (q15_t)SIGNAL_GEN_AMPLITUDE_Q15
                                         : (q15_t)(-SIGNAL_GEN_AMPLITUDE_Q15);
        break;

    case SIGNAL_WAVE_TRIANGLE: {
        // Desplazar 90 grados para arrancar en 0 con pendiente positiva.
        uint32_t p = s_phase + 0x40000000U;
        int32_t tri = (p < 0x80000000U) ? ((int32_t)(p >> 15) - 32768)
                                        : (32767 - (int32_t)((p - 0x80000000U) >> 15));
        sample = (q15_t)((tri * SIGNAL_GEN_AMPLITUDE_Q15) >> 15);
        break;
    }

    case SIGNAL_WAVE_SAWTOOTH: {
        int32_t ramp = (int32_t)(s_phase >> 16) - 32768;
        sample = (q15_t)((ramp * SIGNAL_GEN_AMPLITUDE_Q15) >> 15);
        break;
    }

    default:
        sample = 0;
        break;
    }

    s_phase += s_phase_inc; // el desborde de 32 bits equivale al módulo 2*pi
    return sample;
}
