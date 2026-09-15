/*
 * adc_stage.c
 *
 * Implementación de la etapa de adquisición ADC.
 */

#include "adc_stage.h"
#include "fsl_ctimer.h"
#include "fsl_lpadc.h"
#include "peripherals.h"

static circular_buffer_t *s_circ_buffer = NULL;
static volatile bool s_is_running = false;
static sample_rate_t s_current_sample_rate = SAMPLE_RATE_8K;

static lpadc_conv_result_t s_last_result;
static volatile q15_t s_last_sample = 0;
static volatile uint16_t s_last_raw = 0;

static const uint32_t s_match_values[SAMPLE_RATE_COUNT] = {
    [SAMPLE_RATE_8K] = 9374,  // 8 kHz
    [SAMPLE_RATE_16K] = 4686, // 16 kHz
    [SAMPLE_RATE_22K] = 3408, // 22 kHz
    [SAMPLE_RATE_44K] = 1704, // 44 kHz
    [SAMPLE_RATE_48K] = 1562  // 48 kHz
};

static const uint32_t s_sample_rate_hz[SAMPLE_RATE_COUNT] = {
    [SAMPLE_RATE_8K] = 8000,
    [SAMPLE_RATE_16K] = 16000,
    [SAMPLE_RATE_22K] = 22000,
    [SAMPLE_RATE_44K] = 44000,
    [SAMPLE_RATE_48K] = 48000};

void adc_stage_init(circular_buffer_t *buffer) {
  s_circ_buffer = buffer;
  s_is_running = false;
  s_current_sample_rate = SAMPLE_RATE_8K;
  s_last_sample = 0;
  s_last_raw = 0;
}

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

void adc_stage_apply_sample_rate(sample_rate_t rate, bool deferred) {
  s_current_sample_rate = rate;
  if (!deferred) {
    adc_stage_set_sample_rate(rate);
  }
}

sample_rate_t adc_stage_next_sample_rate(bool deferred) {
  sample_rate_t next =
      (sample_rate_t)((s_current_sample_rate + 1) % SAMPLE_RATE_COUNT);
  adc_stage_apply_sample_rate(next, deferred);
  return next;
}

sample_rate_t adc_stage_get_sample_rate(void) { return s_current_sample_rate; }

uint32_t adc_stage_get_sample_rate_hz(sample_rate_t rate) {
  if (rate < SAMPLE_RATE_COUNT) {
    return s_sample_rate_hz[rate];
  }
  return 0;
}

void adc_stage_start(sample_rate_t rate) {
  if (s_circ_buffer != NULL) {
    circular_buffer_reset(s_circ_buffer);
  }
  adc_stage_set_sample_rate(rate);
  s_is_running = true;
}

void adc_stage_stop(void) {
  s_is_running = false;
  if (s_circ_buffer != NULL) {
    circular_buffer_reset_read(s_circ_buffer);
  }
}

bool adc_stage_is_running(void) { return s_is_running; }

void adc_stage_clear_flags(void) {
  uint32_t trigger_status_flag = LPADC_GetTriggerStatusFlags(ADC1_PERIPHERAL);
  uint32_t status_flag = LPADC_GetStatusFlags(ADC1_PERIPHERAL);
  LPADC_ClearTriggerStatusFlags(ADC1_PERIPHERAL, trigger_status_flag);
  LPADC_ClearStatusFlags(ADC1_PERIPHERAL, status_flag);
}

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

q15_t adc_stage_get_last_sample(void) { return s_last_sample; }

uint16_t adc_stage_get_last_raw(void) { return s_last_raw; }
