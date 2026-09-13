/*
 * adc_stage.h
 *
 * Etapa de adquisición ADC: encapsula el LPADC, control de frecuencia de muestreo con CTIMER,
 * y almacenamiento de muestras normalizadas en formato Q15 dentro del buffer circular.
 */

#ifndef ADC_STAGE_H_
#define ADC_STAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"
#include "circular_buffer.h"

/**
 * @brief Frecuencias de muestreo soportadas por la aplicación.
 */
typedef enum {
    SAMPLE_RATE_8K = 0,
    SAMPLE_RATE_16K,
    SAMPLE_RATE_22K,
    SAMPLE_RATE_44K,
    SAMPLE_RATE_48K,
    SAMPLE_RATE_COUNT
} sample_rate_t;

/**
 * @brief Inicializa la etapa de ADC y asocia el buffer circular.
 * @param buffer Puntero al buffer circular donde se almacenarán las muestras adquiridas.
 */
void adc_stage_init(circular_buffer_t *buffer);

/**
 * @brief Inicia la adquisición a la frecuencia de muestreo especificada (Modo RUN).
 * @param rate Frecuencia de muestreo inicial.
 */
void adc_stage_start(sample_rate_t rate);

/**
 * @brief Detiene la adquisición de nuevas muestras (Modo STOP).
 */
void adc_stage_stop(void);

/**
 * @brief Consulta si la adquisición está activa (RUN).
 * @return true si está en RUN, false si está en STOP.
 */
bool adc_stage_is_running(void);

/**
 * @brief Configura la frecuencia de muestreo en el hardware (CTIMER0).
 * @param rate Frecuencia de muestreo a establecer.
 */
void adc_stage_set_sample_rate(sample_rate_t rate);

/**
 * @brief Aplica la frecuencia de muestreo actual. Si está en STOP, la reprogramación del timer puede diferirse.
 * @param rate Frecuencia a aplicar.
 * @param deferred Si es true, no reprograma el timer de inmediato (espera al próximo RUN).
 */
void adc_stage_apply_sample_rate(sample_rate_t rate, bool deferred);

/**
 * @brief Avanza cíclicamente a la siguiente frecuencia de muestreo.
 * @param deferred Si es true, difiere la actualización del timer de hardware.
 * @return La nueva frecuencia seleccionada.
 */
sample_rate_t adc_stage_next_sample_rate(bool deferred);

/**
 * @brief Obtiene la frecuencia de muestreo actualmente seleccionada.
 */
sample_rate_t adc_stage_get_sample_rate(void);

/**
 * @brief Obtiene el valor numérico en Hz de una frecuencia de muestreo.
 */
uint32_t adc_stage_get_sample_rate_hz(sample_rate_t rate);

/**
 * @brief Limpia las banderas de interrupción del periférico LPADC.
 */
void adc_stage_clear_flags(void);

/**
 * @brief Lee el resultado de la conversión del hardware LPADC, lo convierte a Q15
 *        y lo guarda en el buffer circular (si la adquisición está en RUN).
 * @param sample_out Puntero donde se almacenará la muestra convertida a Q15 (puede ser NULL).
 * @return true si la lectura fue exitosa (adquisición en RUN), false si no había conversión o está en STOP.
 */
bool adc_stage_read_sample(q15_t *sample_out);

/**
 * @brief Retorna la última muestra Q15 adquirida (útil para telemetría/debug).
 */
q15_t adc_stage_get_last_sample(void);

/**
 * @brief Retorna el último valor crudo (16 bits sin signo) del ADC.
 */
uint16_t adc_stage_get_last_raw(void);

#endif /* ADC_STAGE_H_ */

