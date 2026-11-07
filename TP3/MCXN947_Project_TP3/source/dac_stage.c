/*
 * dac_stage.c
 *
 * Implementación de la etapa de salida analógica DAC.
 */

#include "dac_stage.h"  // declaraciones propias
#include "fsl_dac.h"    // driver del DAC0 (DAC_SetData, LPDAC_DATA_DATA)
#include "peripherals.h" // alias generado por MCUXpresso (DAC0_PERIPHERAL)

static volatile q15_t s_last_dac_sample = 0;  // última muestra Q15 escrita (telemetría/debug)
static volatile uint32_t s_last_dac_code = 0; // último código de 12 bits escrito al DAC (telemetría/debug)

// Reinicia el estado de telemetría de la etapa (no toca el hardware del DAC en sí).
void dac_stage_init(void) {
    s_last_dac_sample = 0;
    s_last_dac_code = 0;
}

// Convierte una muestra Q15 con signo a un código de 12 bits sin signo y lo escribe al DAC0.
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

// Escribe directamente un código de 12 bits al DAC0, sin pasar por la conversión desde Q15
// (usado, por ejemplo, para pruebas de bajo nivel del periférico).
void dac_stage_write_code(uint32_t code) {
    s_last_dac_code = code;
    DAC_SetData(DAC0_PERIPHERAL, code);
}

// Getter de la última muestra Q15 procesada enviada al DAC (telemetría/debug).
q15_t dac_stage_get_last_sample(void) {
    return s_last_dac_sample;
}

// Getter del último código de 12 bits escrito al registro del DAC (telemetría/debug).
uint32_t dac_stage_get_last_code(void) {
    return s_last_dac_code;
}

