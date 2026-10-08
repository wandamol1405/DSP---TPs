/*
 * processing_stage.c
 *
 * Implementación de la etapa de procesamiento digital de señales.
 * Controla el ruteo de las muestras hacia el bypass o hacia el filtro FIR activo.
 */

#include "processing_stage.h"
#include "filter_coeffs.h"
#include <stddef.h>

static processing_mode_t s_current_mode = PROCESSING_MODE_BYPASS;
static sample_rate_t s_current_rate = SAMPLE_RATE_8K;

// Instancia de filtro FIR y buffer de estado para CMSIS-DSP
static arm_fir_instance_q15 s_fir_instance;
static q15_t s_fir_state[FIR_STATE_BUFFER_SIZE];

/**
 * @brief Reconfigura la instancia del filtro FIR de acuerdo al modo y frecuencia actuales.
 */
static void reconfigure_filter(processing_mode_t mode, sample_rate_t rate) {
    if (mode == PROCESSING_MODE_BYPASS) {
        return; // No requiere inicializar CMSIS-DSP en bypass
    }

    const fir_filter_config_t *config = NULL;

    switch (mode) {
        case PROCESSING_MODE_LOWPASS:
            config = &fir_config_lp[rate];
            break;
        case PROCESSING_MODE_HIGHPASS:
            config = &fir_config_hp[rate];
            break;
        case PROCESSING_MODE_BANDPASS:
            config = &fir_config_bp[rate];
            break;
        case PROCESSING_MODE_BANDSTOP:
            config = &fir_config_bs[rate];
            break;
        default:
            return;
    }

    if (config != NULL && config->pCoeffs != NULL) {
        arm_fir_init_q15(&s_fir_instance, config->numTaps, (q15_t *)config->pCoeffs, s_fir_state, FIR_BLOCK_SIZE);
    }
}

void processing_stage_init(void) {
    s_current_mode = PROCESSING_MODE_BYPASS;
    s_current_rate = SAMPLE_RATE_8K;
    reconfigure_filter(s_current_mode, s_current_rate);
}

void processing_stage_set_mode(processing_mode_t mode) {
    if (mode < PROCESSING_MODE_COUNT) {
        s_current_mode = mode;
        reconfigure_filter(s_current_mode, s_current_rate);
    }
}

processing_mode_t processing_stage_next_mode(void) {
    s_current_mode = (processing_mode_t)((s_current_mode + 1) % PROCESSING_MODE_COUNT);
    reconfigure_filter(s_current_mode, s_current_rate);
    return s_current_mode;
}

processing_mode_t processing_stage_get_mode(void) {
    return s_current_mode;
}

const char* processing_stage_get_mode_name(processing_mode_t mode) {
    switch (mode) {
        case PROCESSING_MODE_BYPASS:   return "BYPASS";
        case PROCESSING_MODE_LOWPASS:  return "LOWPASS";
        case PROCESSING_MODE_HIGHPASS: return "HIGHPASS";
        case PROCESSING_MODE_BANDPASS: return "BANDPASS";
        case PROCESSING_MODE_BANDSTOP: return "BANDSTOP";
        default:                       return "UNKNOWN";
    }
}

void processing_stage_set_sample_rate(sample_rate_t rate) {
    s_current_rate = rate;
    reconfigure_filter(s_current_mode, s_current_rate);
}

q15_t processing_stage_process_sample(q15_t in_sample) {
    if (s_current_mode == PROCESSING_MODE_BYPASS) {
        return in_sample;
    }

    q15_t out_sample = 0;
    // Procesamos la muestra individual utilizando el tamaño de bloque 1
    arm_fir_q15(&s_fir_instance, &in_sample, &out_sample, FIR_BLOCK_SIZE);
    return out_sample;
}

void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length) {
    if (in == NULL || out == NULL || length == 0) {
        return;
    }

    if (s_current_mode == PROCESSING_MODE_BYPASS) {
        for (uint32_t i = 0; i < length; i++) {
            out[i] = in[i];
        }
        return;
    }

    // Para bloques más grandes, procesamos de una sola pasada
    // NOTA: Para usar arm_fir_q15 en bloques de tamaño diferente a FIR_BLOCK_SIZE, 
    // tendríamos que reinicializar el filtro indicándole el nuevo tamaño de bloque, 
    // o asegurar que siempre se lo llama con bloques de longitud equivalente.
    // Dado que el TP procesa muestra a muestra (FIR_BLOCK_SIZE = 1) llamamos a la API 
    // repitiendo la función individual o modificamos la longitud aquí.
    
    // Lo más seguro es iterar sobre la función individual para no alterar el estado:
    for (uint32_t i = 0; i < length; i++) {
        arm_fir_q15(&s_fir_instance, (q15_t *)&in[i], &out[i], FIR_BLOCK_SIZE);
    }
}
