/*
 * processing_stage.c
 *
 * Implementación de la etapa de procesamiento digital de señales.
 * Controla el ruteo de las muestras hacia el bypass o hacia el filtro FIR activo.
 */

#include "processing_stage.h"
#include "filter_coeffs.h"
#include <stddef.h>

static volatile processing_mode_t s_current_mode = PROCESSING_MODE_BYPASS;
static volatile sample_rate_t s_current_rate = SAMPLE_RATE_8K;

// Los cambios de modo/frecuencia llegan desde ISRs (botones, ADC) o desde main, pero la
// instancia FIR solo se toca en processing_stage_process_block(): ahí se reinicializa entre
// bloques, nunca mientras arm_fir_q15 está filtrando.
static volatile bool s_reconfig_pending = true;

// Modo con el que está configurada la instancia FIR (solo lo usa process_block). Puede
// diferir de s_current_mode hasta el próximo bloque.
static processing_mode_t s_active_mode = PROCESSING_MODE_BYPASS;

// Instancia de filtro FIR y buffer de estado para CMSIS-DSP
static arm_fir_instance_q15 s_fir_instance;
// Con ARM_MATH_DSP (Cortex-M33), arm_fir_init_q15 limpia numTaps + blockSize elementos
static q15_t s_fir_state[MAX_FIR_TAPS + PROCESSING_BLOCK_SIZE];

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
        arm_fir_init_q15(&s_fir_instance, config->numTaps, (q15_t *)config->pCoeffs, s_fir_state, PROCESSING_BLOCK_SIZE);
    }
}

void processing_stage_init(void) {
    s_current_mode = PROCESSING_MODE_BYPASS;
    s_current_rate = SAMPLE_RATE_8K;
    s_reconfig_pending = true;
}

void processing_stage_set_mode(processing_mode_t mode) {
    if (mode < PROCESSING_MODE_COUNT) {
        s_current_mode = mode;
        s_reconfig_pending = true;
    }
}

processing_mode_t processing_stage_next_mode(void) {
    processing_mode_t next = (processing_mode_t)((s_current_mode + 1) % PROCESSING_MODE_COUNT);
    s_current_mode = next;
    s_reconfig_pending = true;
    return next;
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
    s_reconfig_pending = true;
}

void processing_stage_process_block(const q15_t *in, q15_t *out, uint32_t length) {
    if (in == NULL || out == NULL || length == 0) {
        return;
    }

    // Aplicar el cambio de modo/frecuencia pendiente antes de filtrar el bloque. Se limpia
    // el aviso antes de leer modo y frecuencia: si cambian después, queda para el próximo.
    if (s_reconfig_pending) {
        s_reconfig_pending = false;
        s_active_mode = s_current_mode;
        reconfigure_filter(s_active_mode, s_current_rate);
    }

    if (s_active_mode == PROCESSING_MODE_BYPASS) {
        for (uint32_t i = 0; i < length; i++) {
            out[i] = in[i];
        }
        return;
    }

    // La instancia se inicializó con PROCESSING_BLOCK_SIZE: se filtra de a bloques de ese
    // tamaño como máximo (el estado del filtro se conserva entre llamadas)
    while (length > 0) {
        uint32_t n = (length < PROCESSING_BLOCK_SIZE) ? length : PROCESSING_BLOCK_SIZE;
        arm_fir_q15(&s_fir_instance, (q15_t *)in, out, n);
        in += n;
        out += n;
        length -= n;
    }
}
