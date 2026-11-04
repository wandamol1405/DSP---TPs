/*
 * processing_stage.c
 *
 * Implementación de la etapa de procesamiento digital de señales.
 * Controla el ruteo de las muestras hacia el bypass o hacia el filtro FIR activo.
 */

#include "processing_stage.h"
#include "filter_coeffs.h"
#include "filter_coeffs_multirate.h"
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

// Instancia de filtro FIR y buffer de estado para CMSIS-DSP (LOWPASS/HIGHPASS/BANDPASS)
static arm_fir_instance_q15 s_fir_instance;
// Con ARM_MATH_DSP (Cortex-M33), arm_fir_init_q15 limpia numTaps + blockSize elementos
static q15_t s_fir_state[MAX_FIR_TAPS + PROCESSING_BLOCK_SIZE];

// ---------------------------------------------------------------------------
// Elimina banda: camino multirate (decimar -> banda de Fr -> interpolar).
//
// Bw=15 Hz es una fraccion minuscula de Nyquist a 44-48 kHz: un notch de una
// sola etapa (arm_fir_q15, como el resto de los filtros) no entra en tiempo
// real ahi. Procesando a Fs/M, el mismo ancho absoluto es una fraccion mucho
// mas grande de esa Nyquist reducida -> filtro mucho mas corto. Ver
// TP2/filtros/disenio_filtros_tp2_equiripple.ipynb seccion 7.
//
//   y[n] = x[n-D] - ( Interp( Comp( Decim(x) ) ) << log2(M) )
//
// - Decim : anti-alias + diezmado por M (arm_fir_decimate_q15).
// - Comp  : complemento del notch, delta[n-K] - notch[n], a Fs/M: deja pasar
//           SOLO la banda de Fr, ya alineada en el tiempo (se genera en
//           disenio_filtros_tp2.py, complemento_q15).
// - Interp: relleno de ceros + el mismo anti-alias (arm_fir_interpolate_q15).
//           CMSIS no multiplica por M: la salida queda con ganancia 1/M y se
//           compensa con un shift de log2(M) (M es potencia de 2).
// - D     : retardo de la rama de abajo, para que la resta cancele exacto la
//           banda de Fr. Lo calcula el generador con las convenciones de
//           CMSIS (coeficientes invertidos, fase del decimador).
//
// El resto del espectro no pasa por el diezmado: llega a la salida por la
// linea de retardo con todo su ancho de banda.
// ---------------------------------------------------------------------------
static arm_fir_decimate_instance_q15    s_bs_dec_inst;
static arm_fir_instance_q15             s_bs_comp_inst;
static arm_fir_interpolate_instance_q15 s_bs_interp_inst;

// Tamaños de estado: lo que pide (y limpia con memset) cada init de CMSIS.
// arm_fir_init_q15 con ARM_MATH_DSP limpia numTaps + blockSize (no -1).
static q15_t s_bs_dec_state[BS_MR_MAX_AA_TAPS + PROCESSING_BLOCK_SIZE - 1];
static q15_t s_bs_comp_state[BS_MR_MAX_COMP_TAPS + (PROCESSING_BLOCK_SIZE / BS_MR_MIN_M)];
// numTapsAA/M es a lo sumo BS_MR_MAX_AA_TAPS/BS_MR_MIN_M: cota holgada, no ajustada
// exacto por config (ver generar_multirate_bandstop en disenio_filtros_tp2.py)
static q15_t s_bs_interp_state[(BS_MR_MAX_AA_TAPS / BS_MR_MIN_M) + (PROCESSING_BLOCK_SIZE / BS_MR_MIN_M) - 1];

// Buffers intermedios, dimensionados al peor caso (M minimo)
static q15_t s_bs_xdec[PROCESSING_BLOCK_SIZE / BS_MR_MIN_M];   // x diezmada
static q15_t s_bs_banda[PROCESSING_BLOCK_SIZE / BS_MR_MIN_M];  // banda de Fr, a Fs/M
static q15_t s_bs_banda_up[PROCESSING_BLOCK_SIZE];             // banda de Fr, a Fs

// Linea de retardo para x[n-D] (D = delaySamples de la config activa), implementada como
// buffer circular de tamaño potencia de 2 (mascara en vez de modulo). Tiene que ser
// mayor que BS_MR_MAX_DELAY (el mayor D de las 5 Fs, ver filter_coeffs_multirate.h).
#define BS_MR_DELAY_BUF_SIZE 1024U
#if BS_MR_MAX_DELAY >= BS_MR_DELAY_BUF_SIZE
#error "BS_MR_DELAY_BUF_SIZE tiene que ser mayor que BS_MR_MAX_DELAY"
#endif
static q15_t s_bs_delay_buf[BS_MR_DELAY_BUF_SIZE];
static uint32_t s_bs_delay_write = 0;

// Config activa del camino multirate (fijada en reconfigure_bandstop_multirate, se
// mantiene estable durante todo processing_stage_process_block como el resto del estado)
static const bandstop_multirate_config_t *s_bs_active_cfg = NULL;

/**
 * @brief (Re)inicializa el camino multirate de elimina banda para la Fs dada.
 */
static void reconfigure_bandstop_multirate(sample_rate_t rate) {
    const bandstop_multirate_config_t *cfg = &bs_mr_config[rate];
    s_bs_active_cfg = cfg;

    arm_fir_decimate_init_q15(&s_bs_dec_inst, cfg->numTapsAA, (uint8_t)cfg->M,
                               (q15_t *)cfg->pCoeffsAA, s_bs_dec_state, PROCESSING_BLOCK_SIZE);
    arm_fir_init_q15(&s_bs_comp_inst, cfg->numTapsComp, (q15_t *)cfg->pCoeffsComp,
                      s_bs_comp_state, PROCESSING_BLOCK_SIZE / cfg->M);
    arm_fir_interpolate_init_q15(&s_bs_interp_inst, (uint8_t)cfg->M, cfg->numTapsAA,
                                  (q15_t *)cfg->pCoeffsAA, s_bs_interp_state, PROCESSING_BLOCK_SIZE / cfg->M);

    // Limpia la linea de retardo: si no, al cambiar de Fs/filtro queda mezclada con
    // historia vieja de otra configuracion (su D es distinto).
    for (uint32_t i = 0; i < BS_MR_DELAY_BUF_SIZE; i++) {
        s_bs_delay_buf[i] = 0;
    }
    s_bs_delay_write = 0;
}

/**
 * @brief Filtra un bloque de PROCESSING_BLOCK_SIZE muestras por el camino multirate.
 *        Requiere un bloque completo: el decimador se inicializó con blockSize fijo
 *        (ver reconfigure_bandstop_multirate). El pipeline siempre llama con
 *        n == PROCESSING_BLOCK_SIZE; si no lo fuera, se deja pasar sin filtrar antes
 *        que desalinear el decimador/interpolador.
 */
static void process_bandstop_multirate(const q15_t *in, q15_t *out, uint32_t n) {
    if (n != PROCESSING_BLOCK_SIZE || s_bs_active_cfg == NULL) {
        for (uint32_t i = 0; i < n; i++) {
            out[i] = in[i];
        }
        return;
    }

    const bandstop_multirate_config_t *cfg = s_bs_active_cfg;
    uint32_t n_dec = PROCESSING_BLOCK_SIZE / cfg->M;

    // 1) Decimar (incluye el anti-alias)
    arm_fir_decimate_q15(&s_bs_dec_inst, (q15_t *)in, s_bs_xdec, PROCESSING_BLOCK_SIZE);

    // 2) Quedarse solo con la banda de Fr (complemento del notch), a Fs/M
    arm_fir_q15(&s_bs_comp_inst, s_bs_xdec, s_bs_banda, n_dec);

    // 3) Interpolar de vuelta a Fs y compensar la ganancia 1/M del interpolador
    arm_fir_interpolate_q15(&s_bs_interp_inst, s_bs_banda, s_bs_banda_up, n_dec);
    arm_shift_q15(s_bs_banda_up, cfg->log2M, s_bs_banda_up, PROCESSING_BLOCK_SIZE);

    // 4) y[n] = x[n-D] - banda de Fr (D = retardo de la rama de abajo)
    for (uint32_t i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        s_bs_delay_buf[s_bs_delay_write] = in[i];
        uint32_t read_idx = (s_bs_delay_write - cfg->delaySamples) & (BS_MR_DELAY_BUF_SIZE - 1U);
        q15_t x_delayed = s_bs_delay_buf[read_idx];
        s_bs_delay_write = (s_bs_delay_write + 1U) & (BS_MR_DELAY_BUF_SIZE - 1U);
        out[i] = (q15_t)__SSAT((q31_t)x_delayed - (q31_t)s_bs_banda_up[i], 16);
    }
}

/**
 * @brief Reconfigura la instancia del filtro FIR de acuerdo al modo y frecuencia actuales.
 */
static void reconfigure_filter(processing_mode_t mode, sample_rate_t rate) {
    if (mode == PROCESSING_MODE_BYPASS) {
        return; // No requiere inicializar CMSIS-DSP en bypass
    }

    if (mode == PROCESSING_MODE_BANDSTOP) {
        // Elimina banda no usa fir_config_bs (una sola etapa): usa el camino
        // multirate de arriba, mucho mas liviano en 44-48 kHz.
        reconfigure_bandstop_multirate(rate);
        return;
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

    if (s_active_mode == PROCESSING_MODE_BANDSTOP) {
        // Elimina banda: camino multirate (ver process_bandstop_multirate arriba),
        // no el arm_fir_q15 de una sola etapa que usan los otros 3 modos.
        while (length > 0) {
            uint32_t n = (length < PROCESSING_BLOCK_SIZE) ? length : PROCESSING_BLOCK_SIZE;
            process_bandstop_multirate(in, out, n);
            in += n;
            out += n;
            length -= n;
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
