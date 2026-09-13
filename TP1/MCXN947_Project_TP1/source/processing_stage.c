/*
 * processing_stage.c
 *
 * Implementación de la etapa de procesamiento digital de señales.
 */

#include "processing_stage.h"
#include <math.h>

static processing_mode_t s_current_mode = PROCESSING_MODE_PASSTHROUGH;
static float s_gain = 1.0f;

void processing_stage_init(void) {
    s_current_mode = PROCESSING_MODE_PASSTHROUGH;
    s_gain = 1.0f;
}

void processing_stage_set_mode(processing_mode_t mode) {
    s_current_mode = mode;
}

processing_mode_t processing_stage_get_mode(void) {
    return s_current_mode;
}

float processing_stage_q15_to_float(q15_t sample) {
    float fval = 0.0f;
    arm_q15_to_float(&sample, &fval, 1U);
    return fval;
}

q15_t processing_stage_float_to_q15(float value) {
    q15_t sample = 0;
    // Saturación en rango [-1.0, 1.0)
    if (value > 0.999969f) {
        value = 0.999969f;
    } else if (value < -1.0f) {
        value = -1.0f;
    }
    arm_float_to_q15(&value, &sample, 1U);
    return sample;
}

q15_t processing_stage_process_sample(q15_t in_sample) {
    q15_t out_sample = in_sample;

    switch (s_current_mode) {
    case PROCESSING_MODE_PASSTHROUGH:
        // Identidad: sin modificación de la señal
        out_sample = in_sample;
        break;

    case PROCESSING_MODE_FLOAT_CONV: {
        // Conversión a float y retorno a Q15 (cambio de tipo de variable y verificación)
        float fval = processing_stage_q15_to_float(in_sample);
        out_sample = processing_stage_float_to_q15(fval);
        break;
    }

    case PROCESSING_MODE_INVERT:
        // Inversión de signo (fase 180°), saturando -32768 a 32767
        if (in_sample == -32768) {
            out_sample = 32767;
        } else {
            out_sample = -in_sample;
        }
        break;

    case PROCESSING_MODE_GAIN: {
        float fval = processing_stage_q15_to_float(in_sample) * s_gain;
        out_sample = processing_stage_float_to_q15(fval);
        break;
    }

    case PROCESSING_MODE_CUSTOM:
        // Punto de anclaje para futuros filtros de TP2/TP3
        // ej. arm_fir_q15, arm_biquad_cascade_df1_q15
        out_sample = in_sample;
        break;

    default:
        out_sample = in_sample;
        break;
    }

    return out_sample;
}

void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length) {
    if (in == NULL || out == NULL || length == 0) {
        return;
    }

    for (uint32_t i = 0; i < length; i++) {
        out[i] = processing_stage_process_sample(in[i]);
    }
}

