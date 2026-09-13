/*
 * dac_stage.c
 *
 * Implementación de la etapa de salida analógica DAC.
 */

#include "dac_stage.h"
#include "fsl_dac.h"
#include "peripherals.h"

static volatile q15_t s_last_dac_sample = 0;
static volatile uint32_t s_last_dac_code = 0;

void dac_stage_init(void) {
    s_last_dac_sample = 0;
    s_last_dac_code = 0;
}

void dac_stage_write_sample(q15_t sample) {
    s_last_dac_sample = sample;

    // Conversión de escala:
    // sample está en [-32768, 32767].
    // Sumamos 32768 para llevarlo a [0, 65535].
    // Desplazamos 4 bits a la derecha para obtener los 12 bits [0, 4095] requeridos por el DAC.
    uint32_t code = LPDAC_DATA_DATA(((int32_t)sample + 32768) >> 4);
    s_last_dac_code = code;

    DAC_SetData(DAC0_PERIPHERAL, code);
}

void dac_stage_write_code(uint32_t code) {
    s_last_dac_code = code;
    DAC_SetData(DAC0_PERIPHERAL, code);
}

q15_t dac_stage_get_last_sample(void) {
    return s_last_dac_sample;
}

uint32_t dac_stage_get_last_code(void) {
    return s_last_dac_code;
}

