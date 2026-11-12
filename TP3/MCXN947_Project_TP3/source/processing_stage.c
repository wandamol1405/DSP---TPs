/*
 * processing_stage.c
 *
 * Implementación de la etapa de procesamiento digital de señales.
 *
 * FFT de N puntos (512, 1024 o 2048) con CMSIS-DSP:
 *  1. La ventana real se arma como complejo intercalado [re0, im0, re1, im1, ...] con
 *     parte imaginaria 0 (arm_cfft_q15 trabaja en el lugar y pisa su buffer, así que la
 *     ventana de entrada no se toca).
 *  2. arm_cfft_q15 con la instancia de N puntos. Para no saturar, divide por 2 en cada
 *     etapa: la salida queda escalada por 1/N (en q15, X[k] / N).
 *  3. arm_cmplx_mag_q15 sobre los primeros N/2 bins (de 0 a Fs/2; la otra mitad es el
 *     espejo, porque la entrada es real). Devuelve |X[k]| / N en formato 2.14.
 *  4. Conversión a dBFS x 100 en float (el M33 tiene FPU).
 *
 * Escala de dBFS: un tono A*cos(2*pi*f*n/Fs) que cae justo en el bin k da
 * |X[k]| = A*N/2, o sea |X[k]|/N = A/2. Por eso la amplitud del tono es 2*|X[k]|/N y, en
 * 2.14, A = mag / 8192. Así 0 dB es un tono de amplitud máxima (A = 1.0 en q15) para
 * cualquier N. El bin 0 (continua) no se reparte en dos mitades: A = mag / 16384.
 */

#include "processing_stage.h"
#include "arm_const_structs.h" // instancias arm_cfft_sR_q15_len512/1024/2048
#include <math.h>              // log10f
#include <stddef.h>            // NULL

// Buffer de trabajo de la FFT: N complejos intercalados (2 x 2048 x 2 bytes = 8 KB)
static q15_t s_cfft_buf[2U * PROCESSING_MAX_FRAME];

// Magnitud de los N/2 bins, en formato 2.14
static q15_t s_mag[PROCESSING_MAX_FRAME / 2U];

static volatile processing_mode_t s_current_mode = PROCESSING_MODE_BYPASS;

void processing_stage_init(void) {
    s_current_mode = PROCESSING_MODE_BYPASS;
}

void processing_stage_set_mode(processing_mode_t mode) {
    if (mode < PROCESSING_MODE_COUNT) {
        s_current_mode = mode;
    }
}

processing_mode_t processing_stage_get_mode(void) {
    return s_current_mode;
}

const char* processing_stage_get_mode_name(processing_mode_t mode) {
    switch (mode) {
        case PROCESSING_MODE_BYPASS: return "BYPASS";
        case PROCESSING_MODE_FFT:    return "FFT";
        default:                     return "UNKNOWN";
    }
}

uint16_t processing_stage_get_num_taps(void) {
    return 0;
}

// Instancia de CMSIS para cada N soportado (NULL si N no es válido)
static const arm_cfft_instance_q15 *cfft_instance_for(uint32_t n) {
    switch (n) {
        case 512U:  return &arm_cfft_sR_q15_len512;
        case 1024U: return &arm_cfft_sR_q15_len1024;
        case 2048U: return &arm_cfft_sR_q15_len2048;
        default:    return NULL;
    }
}

// Pasa una magnitud en 2.14 a dBFS x 100 (ver la escala en el encabezado del archivo)
static int16_t mag_to_db_x100(q15_t mag, bool is_dc) {
    if (mag <= 0) {
        return (int16_t)PROCESSING_DB_FLOOR_X100;
    }

    float amplitude = (float)mag / (is_dc ? 16384.0f : 8192.0f);
    float db_x100 = 2000.0f * log10f(amplitude); // 100 x 20 log10(A)

    // Redondeo al entero más cercano. El rango posible es de -7827 (mag = 1) a +1204
    // (mag máxima), así que entra en int16 sin recortar.
    return (int16_t)(db_x100 + ((db_x100 >= 0.0f) ? 0.5f : -0.5f));
}

static uint32_t process_fft(const q15_t *in, uint32_t n, int16_t *out) {
    const arm_cfft_instance_q15 *inst = cfft_instance_for(n);
    if (inst == NULL) {
        return 0;
    }

    // 1. Ventana real -> complejo intercalado con parte imaginaria 0
    for (uint32_t i = 0; i < n; i++) {
        s_cfft_buf[2U * i]      = in[i];
        s_cfft_buf[2U * i + 1U] = 0;
    }

    // 2. FFT directa (ifftFlag = 0) con la salida en orden natural (bitReverseFlag = 1)
    arm_cfft_q15(inst, s_cfft_buf, 0U, 1U);

    // 3. Magnitud de los bins 0 .. N/2 - 1
    uint32_t bins = n / 2U;
    arm_cmplx_mag_q15(s_cfft_buf, s_mag, bins);

    // 4. dBFS x 100
    for (uint32_t k = 0; k < bins; k++) {
        out[k] = mag_to_db_x100(s_mag[k], k == 0U);
    }

    return bins;
}

uint32_t processing_stage_process_frame(processing_mode_t mode, const q15_t *in, uint32_t n, int16_t *out) {
    if (in == NULL || out == NULL || n == 0U || n > PROCESSING_MAX_FRAME) {
        return 0;
    }

    switch (mode) {
        case PROCESSING_MODE_FFT:
            return process_fft(in, n, out);

        case PROCESSING_MODE_BYPASS:
            // La ventana se copia tal cual al buffer de salida
            arm_copy_q15(in, out, n);
            return n;

        default:
            return 0;
    }
}
