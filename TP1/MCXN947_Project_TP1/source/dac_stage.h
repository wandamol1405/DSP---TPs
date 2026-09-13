/*
 * dac_stage.h
 *
 * Etapa de salida analógica DAC: encapsula el periférico DAC0 de 12 bits,
 * la conversión de escala desde Q15 con signo a código sin signo [0..4095],
 * y la escritura al hardware.
 */

#ifndef DAC_STAGE_H_
#define DAC_STAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"

/**
 * @brief Inicializa la etapa de DAC.
 */
void dac_stage_init(void);

/**
 * @brief Convierte una muestra Q15 a 12 bits sin signo y la envía al DAC0.
 * @param sample Muestra en formato Q15 [-32768..32767].
 */
void dac_stage_write_sample(q15_t sample);

/**
 * @brief Envía directamente un código de 12 bits al DAC0.
 * @param code Valor numérico entre 0 y 4095.
 */
void dac_stage_write_code(uint32_t code);

/**
 * @brief Retorna la última muestra Q15 procesada enviada al DAC (para telemetría).
 */
q15_t dac_stage_get_last_sample(void);

/**
 * @brief Retorna el último código de 12 bits escrito al registro del DAC (para telemetría).
 */
uint32_t dac_stage_get_last_code(void);

#endif /* DAC_STAGE_H_ */

