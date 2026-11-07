/*
 * adc_stage.c
 *
 * Implementación de la etapa de adquisición ADC.
 */

#include "adc_stage.h"  // declaraciones propias (sample_rate_t y API pública)
#include "fsl_ctimer.h" // driver del CTIMER0, usado como fuente de trigger periódico del ADC
#include "fsl_lpadc.h"  // driver del LPADC (conversor A/D)
#include "peripherals.h" // alias/config generados por MCUXpresso (ADC1_PERIPHERAL, CTIMER0_PERIPHERAL, etc.)

static circular_buffer_t *s_circ_buffer = NULL;              // buffer circular donde se acumulan las muestras (inyectado por pipeline_init)
static volatile bool s_is_running = false;                   // true en modo RUN, false en modo STOP
static sample_rate_t s_current_sample_rate = SAMPLE_RATE_8K; // frecuencia de muestreo seleccionada actualmente

static lpadc_conv_result_t s_last_result; // último resultado crudo leído del LPADC (struct del driver)
static volatile q15_t s_last_sample = 0;  // última muestra ya normalizada a Q15 (telemetría/debug)
static volatile uint16_t s_last_raw = 0;  // último valor crudo de 16 bits sin signo del LPADC (telemetría/debug)

// Valor de match de CTIMER0 para cada frecuencia. Se calcula para el DOBLE de la frecuencia
// deseada porque el LPADC solo dispara con flanco de subida (fsl_lpadc.h) y el match está
// configurado en modo Toggle: la mitad de los matches generan flanco de bajada, no de subida.
static const uint32_t s_match_values[SAMPLE_RATE_COUNT] = {
    [SAMPLE_RATE_8K] = 9374,  // 8 kHz
    [SAMPLE_RATE_16K] = 4686, // 16 kHz
    [SAMPLE_RATE_22K] = 3408, // 22 kHz
    [SAMPLE_RATE_44K] = 1704, // 44 kHz
    [SAMPLE_RATE_48K] = 1562  // 48 kHz
};

// Valor en Hz de cada frecuencia soportada, indexado por sample_rate_t (para reportar/mostrar).
static const uint32_t s_sample_rate_hz[SAMPLE_RATE_COUNT] = {
    [SAMPLE_RATE_8K] = 8000,
    [SAMPLE_RATE_16K] = 16000,
    [SAMPLE_RATE_22K] = 22000,
    [SAMPLE_RATE_44K] = 44000,
    [SAMPLE_RATE_48K] = 48000};

// Guarda el puntero al buffer circular a usar y deja la etapa en su estado inicial (STOP, 8 kHz).
void adc_stage_init(circular_buffer_t *buffer) {
  s_circ_buffer = buffer;
  s_is_running = true;
  s_current_sample_rate = SAMPLE_RATE_8K;
  s_last_sample = 0;
  s_last_raw = 0;
}

// Reprograma el match de CTIMER0 para la frecuencia dada. Frena el timer, aplica el nuevo
// matchValue y lo reinicia desde 0 para que el cambio de período no deje "colgado" el ADC
// esperando un match que ya pasó (ver issue de reset de contador al cambiar frecuencia).
void adc_stage_set_sample_rate(sample_rate_t rate) {
  if (rate >= SAMPLE_RATE_COUNT) {
    return;
  }
  s_current_sample_rate = rate;

  // Frenar el timer antes de cambiar la configuración
  CTIMER_StopTimer(CTIMER0_PERIPHERAL);

  ctimer_match_config_t new_config = CTIMER0_Match_0_config;
  new_config.matchValue = s_match_values[rate];
  CTIMER_SetupMatch(CTIMER0_PERIPHERAL, CTIMER0_MATCH_0_CHANNEL, &new_config);

  // Reiniciar el contador a 0 y volver a arrancar
  CTIMER_Reset(CTIMER0_PERIPHERAL);
  CTIMER_StartTimer(CTIMER0_PERIPHERAL);
}

// Actualiza la frecuencia "lógica" actual y, salvo que deferred sea true, reprograma ya
// mismo el hardware (deferred se usa en STOP para no tocar el timer hasta el próximo RUN).
void adc_stage_apply_sample_rate(sample_rate_t rate, bool deferred) {
  s_current_sample_rate = rate;
  if (!deferred) {
    adc_stage_set_sample_rate(rate);
  }
}

// Calcula la siguiente frecuencia en el ciclo (8k→16k→22k→44k→48k→8k) y la aplica.
sample_rate_t adc_stage_next_sample_rate(bool deferred) {
  sample_rate_t next =
      (sample_rate_t)((s_current_sample_rate + 1) % SAMPLE_RATE_COUNT);
  adc_stage_apply_sample_rate(next, deferred);
  return next;
}

// Getter de la frecuencia de muestreo actualmente seleccionada.
sample_rate_t adc_stage_get_sample_rate(void) { return s_current_sample_rate; }

// Traduce un valor de sample_rate_t a su equivalente en Hz (0 si el valor es inválido).
uint32_t adc_stage_get_sample_rate_hz(sample_rate_t rate) {
  if (rate < SAMPLE_RATE_COUNT) {
    return s_sample_rate_hz[rate];
  }
  return 0;
}

// Pasa a modo RUN: limpia el buffer circular, arranca el timer a la frecuencia pedida
// y habilita la adquisición.
void adc_stage_start(sample_rate_t rate) {
  if (s_circ_buffer != NULL) {
    circular_buffer_reset(s_circ_buffer);
  }
  adc_stage_set_sample_rate(rate);
  s_is_running = true;
}

// Pasa a modo STOP: deja de adquirir y reubica el puntero de lectura al inicio del buffer
// para la reproducción en loop.
void adc_stage_stop(void) {
  s_is_running = false;
  if (s_circ_buffer != NULL) {
    circular_buffer_reset_read(s_circ_buffer);
  }
}

// Indica si la etapa está actualmente en modo RUN.
bool adc_stage_is_running(void) { return s_is_running; }

// Limpia los flags de trigger/estado del LPADC (debe llamarse al atender el ADC1_IRQHandler).
void adc_stage_clear_flags(void) {
  uint32_t trigger_status_flag = LPADC_GetTriggerStatusFlags(ADC1_PERIPHERAL);
  uint32_t status_flag = LPADC_GetStatusFlags(ADC1_PERIPHERAL);
  LPADC_ClearTriggerStatusFlags(ADC1_PERIPHERAL, trigger_status_flag);
  LPADC_ClearStatusFlags(ADC1_PERIPHERAL, status_flag);
}

// Lee el resultado de conversión disponible en el LPADC, lo normaliza de [0,65535] a Q15
// con signo [-32768,32767] y lo empuja al buffer circular. No hace nada si está en STOP.
bool adc_stage_read_sample(q15_t *sample_out) {
  if (!s_is_running) {
    return false;
  }

  if (!LPADC_GetConvResult(ADC1_PERIPHERAL, &s_last_result, 0U)) {
    return false;
  }

  s_last_raw = (uint16_t)s_last_result.convValue;

  // Normalización: LPADC de 16 bits sin signo [0..65535] a Q15 con signo
  // [-32768..32767]
  q15_t q15_val = (q15_t)((int32_t)s_last_raw - 32768);
  s_last_sample = q15_val;

  if (s_circ_buffer != NULL) {
    circular_buffer_push(s_circ_buffer, q15_val);
  }

  if (sample_out != NULL) {
    *sample_out = q15_val;
  }

  return true;
}

// Getter de la última muestra normalizada a Q15 (telemetría/debug).
q15_t adc_stage_get_last_sample(void) { return s_last_sample; }

// Getter del último valor crudo de 16 bits sin signo entregado por el LPADC (telemetría/debug).
uint16_t adc_stage_get_last_raw(void) { return s_last_raw; }
