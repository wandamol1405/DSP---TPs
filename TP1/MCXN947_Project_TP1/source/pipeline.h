/*
 * pipeline.h
 *
 * Coordinador del pipeline de procesamiento de señal.
 * Conecta las etapas: ADC -> Buffer Circular -> Procesamiento DSP -> Salida (DAC y UART).
 */

#ifndef PIPELINE_H_
#define PIPELINE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"
#include "circular_buffer.h"
#include "adc_stage.h"
#include "processing_stage.h"
#include "dac_stage.h"
#include "uart_stage.h"

#define PIPELINE_BUFFER_SIZE 512

/**
 * @brief Inicializa el pipeline y todas sus etapas asociadas.
 */
void pipeline_init(void);

/**
 * @brief Ejecuta un paso del pipeline. Se invoca al ritmo de la tasa de muestreo.
 *        En RUN: Adquiere del ADC -> guarda en buffer -> procesa con DSP -> envía a DAC y UART.
 *        En STOP: Lee del buffer circular (loop) -> procesa con DSP -> envía a DAC y UART.
 */
void pipeline_step(void);

/**
 * @brief Inicia la adquisición y procesamiento continuo (Modo RUN).
 * @param rate Frecuencia de muestreo deseada.
 */
void pipeline_start(sample_rate_t rate);

/**
 * @brief Detiene la adquisición y pasa a reproducción en loop del buffer (Modo STOP).
 */
void pipeline_stop(void);

/**
 * @brief Alterna el estado entre RUN y STOP.
 * @return El nuevo estado (true = RUN, false = STOP).
 */
bool pipeline_toggle_run_stop(void);

/**
 * @brief Consulta si el pipeline está en adquisición (RUN).
 */
bool pipeline_is_running(void);

/**
 * @brief Avanza a la siguiente frecuencia de muestreo disponible.
 * @return La nueva frecuencia seleccionada.
 */
sample_rate_t pipeline_next_sample_rate(void);

/**
 * @brief Obtiene la frecuencia de muestreo actual.
 */
sample_rate_t pipeline_get_sample_rate(void);

/**
 * @brief Obtiene el valor en Hz de la frecuencia actual.
 */
uint32_t pipeline_get_sample_rate_hz(void);

/**
 * @brief Aplica una frecuencia de muestreo específica.
 */
void pipeline_set_sample_rate(sample_rate_t rate);

/**
 * @brief Obtiene el puntero al buffer circular interno del pipeline.
 */
circular_buffer_t* pipeline_get_circular_buffer(void);

/**
 * @brief Vuelca el buffer circular por UART para análisis en PC.
 * @param format_csv true para formato CSV, false con índices por línea.
 */
void pipeline_dump_to_uart(bool format_csv);

#endif /* PIPELINE_H_ */

