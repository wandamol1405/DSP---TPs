/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    MCXN947_Project_TP1.c
 * @brief   Application entry point.
 */
#include "arm_math.h"
#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "peripherals.h"
#include "pin_mux.h"
#include <stdbool.h>
#include <stdio.h>
/* TODO: insert other include files here. */

/* TODO: insert other definitions and declarations here. */

#define ADC_BUFFER_SIZE 512

volatile q15_t adc_buffer[ADC_BUFFER_SIZE];
volatile uint32_t write_index = 0;
volatile uint32_t read_index = 0;

// Definition of the sample rates
typedef enum {
  SAMPLE_RATE_8K = 0,
  SAMPLE_RATE_16K,
  SAMPLE_RATE_22K,
  SAMPLE_RATE_44K,
  SAMPLE_RATE_48K
} sample_rate_t;

static sample_rate_t current_sample_rate = SAMPLE_RATE_8K;
static lpadc_conv_result_t result;

static volatile bool is_conversion_running = false; // flag para la IRQ del ADC
static volatile bool adc_print_flag = false; // flag para imprimir estado del ADC
static volatile bool freq_print_flag = false;		// flag para imprimir la frecuencia actual
static volatile bool adc_data_ready = false;

/**
 * 8 kHz  → R (Rojo)
 * 16 kHz → G (Verde)
 * 22 kHz → B (Azul)
 * 44 kHz → R + G (Amarillo)
 * 48 kHz → R + B (Magenta)
 */

/**
 * @brief Set the LED color based on the sample rate.
 * @param rate The sample rate to set the LED for.
 */
void SetLedForSampleRate(sample_rate_t rate) {
  switch (rate) {
  case SAMPLE_RATE_8K:
    // LED correspondiente a 8 kHz
    LED_RED_ON();
    LED_GREEN_OFF();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_16K:
    // LED correspondiente a 16 kHz
    LED_RED_OFF();
    LED_GREEN_ON();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_22K:
    // LED correspondiente a 22 kHz
    LED_RED_OFF();
    LED_GREEN_OFF();
    LED_BLUE_ON();
    break;

  case SAMPLE_RATE_44K:
    // LED correspondiente a 44 kHz
    LED_RED_ON();
    LED_GREEN_ON();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_48K:
    // LED correspondiente a 48 kHz
    LED_RED_ON();
    LED_GREEN_OFF();
    LED_BLUE_ON();
    break;
  }
}

/**
 * @brief Set the sample rate for the timer.
 * @param rate The sample rate to set.
 */
void Timer_SetSampleRate(sample_rate_t rate) {
  uint32_t match_value = 0;

  switch (rate) {
  case SAMPLE_RATE_8K:
    match_value = 18749; // 8 kHz
    break;
  case SAMPLE_RATE_16K:
    match_value = 9374; // 16 kHz
    break;
  case SAMPLE_RATE_22K:
    match_value = 6817; // 22 kHz
    break;
  case SAMPLE_RATE_44K:
    match_value = 3408; // 44 kHz
    break;
  case SAMPLE_RATE_48K:
    match_value = 3124; // 48 kHz
    break;
  }

  ctimer_match_config_t new_config = CTIMER0_Match_0_config;
  new_config.matchValue = match_value;

  CTIMER_SetupMatch(CTIMER0_PERIPHERAL, CTIMER0_MATCH_0_CHANNEL, &new_config);

  freq_print_flag = true;
}

/**
 * @brief Apply a sample rate.
 * @param rate The sample rate to apply.
 */
void ApplySampleRate(sample_rate_t rate) {
  SetLedForSampleRate(rate);
  Timer_SetSampleRate(rate);
}

void NextSampleRate(void) {
  if (current_sample_rate == SAMPLE_RATE_48K) {
    current_sample_rate = SAMPLE_RATE_8K;
  } else {
    current_sample_rate++;
  }

  ApplySampleRate(current_sample_rate);
}

/**
 * @brief   Application entry point.
 */
int main(void) {

  /* Init board hardware. */
  BOARD_InitBootPins();
  BOARD_InitBootClocks();
  BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
  /* Init FSL debug console. */
  BOARD_InitDebugConsole();
#endif

  PRINTF("Hello TP1\r\n");
  //    PRINTF("Initial sample rate: %d\r\n", current_sample_rate);
  ApplySampleRate(current_sample_rate);

  /* Enter an infinite loop, just incrementing a counter. */
  while (1) {

    if (adc_print_flag) {
      if (is_conversion_running)
        PRINTF("[RUN]: adquisición activada\r\n");
      else
        PRINTF("[STOP]: adquisición desactivada\r\n");
      adc_print_flag = false;
    }

    if (freq_print_flag){
    	PRINTF("[INFO] Sample rate set to %d Hz\r\n",
    	         (current_sample_rate == SAMPLE_RATE_8K)    ? 8000
    	         : (current_sample_rate == SAMPLE_RATE_16K) ? 16000
    	         : (current_sample_rate == SAMPLE_RATE_22K) ? 22000
    	         : (current_sample_rate == SAMPLE_RATE_44K) ? 44000
    	         : (current_sample_rate == SAMPLE_RATE_48K) ? 48000
				 : 0);
    	freq_print_flag = false;
    }

    if (adc_data_ready){
		PRINTF("[ADC] Conversion: %u\r\n", result.convValue);
		adc_data_ready = false;
	}

  }
  return 0;
}

/**
 * @brief   GPIO00_IRQn interrupt handler.
 * @details This function changes the sample rate to the next one in the list.
 */
void GPIO0_INT_0_IRQHANDLER(void) {
  /* Get pin flags 0 */
  uint32_t pin_flags0 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0U);

  /* Place your interrupt code here */

  // Change to the next sample rate
  NextSampleRate();

  /* Clear pin flags 0 */
  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags0, 0U);

/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
 Store immediate overlapping exception return operation might vector to
 incorrect interrupt. */
#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

/**
 * @brief   GPIO01_IRQn interrupt handler.
 * @details This function toggles the conversion running state.
 */
void GPIO0_INT_1_IRQHANDLER(void) {
  /* Get pin flags 1 */
  uint32_t pin_flags1 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 1U);

  /* Place your interrupt code here */

  // Toggle the conversion running state
  is_conversion_running = !is_conversion_running;
  adc_print_flag = true;

  /* Clear pin flags 1 */
  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags1, 1U);

/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
 Store immediate overlapping exception return operation might vector to
 incorrect interrupt. */
#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

/* ADC1_IRQn interrupt handler */
void ADC1_IRQHANDLER(void) {
  uint32_t trigger_status_flag;
  uint32_t status_flag;
  /* Trigger interrupt flags */
  trigger_status_flag = LPADC_GetTriggerStatusFlags(ADC1_PERIPHERAL);
  /* Interrupt flags */
  status_flag = LPADC_GetStatusFlags(ADC1_PERIPHERAL);
  /* Clears trigger interrupt flags */
  LPADC_ClearTriggerStatusFlags(ADC1_PERIPHERAL, trigger_status_flag);
  /* Clears interrupt flags */
  LPADC_ClearStatusFlags(ADC1_PERIPHERAL, status_flag);

  /* Place your code here */
  if (is_conversion_running){
	  LPADC_GetConvResult(ADC1, &result, 0U);

	  adc_buffer[write_index] = (q15_t)result.convValue;

	  write_index++;
	  if (write_index >= ADC_BUFFER_SIZE){
		  write_index = 0;
	  }

	  adc_data_ready = true;
  }

  /* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
     Store immediate overlapping exception return operation might vector to incorrect interrupt. */
  #if defined __CORTEX_M && (__CORTEX_M == 4U)
    __DSB();
  #endif
}
