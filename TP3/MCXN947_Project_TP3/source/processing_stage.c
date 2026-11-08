/*
 * processing_stage.c
 *
 * Implementación de la etapa de procesamiento digital de señales.
 *
 * TP3: sin los filtros FIR del TP2, la etapa solo hace bypass. Se mantiene la misma
 * interfaz para que pipeline.c y main sigan compilando hasta que se agregue la FFT
 * (issue #15).
 */

#include "processing_stage.h"
#include <stddef.h>

static volatile processing_mode_t s_current_mode = PROCESSING_MODE_BYPASS;
static volatile sample_rate_t s_current_rate = SAMPLE_RATE_8K;

void processing_stage_init(void) {
    s_current_mode = PROCESSING_MODE_BYPASS;
    s_current_rate = SAMPLE_RATE_8K;
}

void processing_stage_set_mode(processing_mode_t mode) {
    if (mode < PROCESSING_MODE_COUNT) {
        s_current_mode = mode;
    }
}

processing_mode_t processing_stage_next_mode(void) {
    processing_mode_t next = (processing_mode_t)((s_current_mode + 1) % PROCESSING_MODE_COUNT);
    s_current_mode = next;
    return next;
}

processing_mode_t processing_stage_get_mode(void) {
    return s_current_mode;
}

const char* processing_stage_get_mode_name(processing_mode_t mode) {
    switch (mode) {
        case PROCESSING_MODE_BYPASS: return "BYPASS";
        default:                     return "UNKNOWN";
    }
}

uint16_t processing_stage_get_num_taps(void) {
    return 0;
}

void processing_stage_set_sample_rate(sample_rate_t rate) {
    s_current_rate = rate;
}

void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length) {
    if (in == NULL || out == NULL || length == 0) {
        return;
    }

    for (uint32_t i = 0; i < length; i++) {
        out[i] = in[i];
    }
}
