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
#include <stdlib.h>
/* TODO: insert other include files here. */

/* TODO: insert other definitions and declarations here. */

#define ADC_BUFFER_SIZE 512

volatile q15_t adc_buffer[ADC_BUFFER_SIZE];

volatile uint32_t write_index = 0;
volatile uint32_t read_index = 0;

// Definición de las frecuencias de muestreo
typedef enum {
  SAMPLE_RATE_8K = 0,
  SAMPLE_RATE_16K,
  SAMPLE_RATE_22K,
  SAMPLE_RATE_44K,
  SAMPLE_RATE_48K
} sample_rate_t;

static sample_rate_t current_sample_rate = SAMPLE_RATE_8K;  // frecuencia de muestreo actual del ADC

static lpadc_conv_result_t result;							// último resultado de conversión del ADC (solo se actualiza en RUN)

// Copias para los PRINTF de debug del loop principal, desacopladas del
// camino real del ADC/DAC en la ISR: un print lento (semihost) nunca
// bloquea ni marca el ritmo de ADC1_IRQHANDLER.
static volatile q15_t last_sample;			// última muestra nueva del ADC (solo tiene sentido en RUN)
static volatile q15_t last_dac_sample;		// Q15 que alimentó al DAC en el último tick (RUN o STOP)
static volatile uint32_t last_dac_value;	// código de 12 bits que se mandó al DAC en el último tick

static volatile bool is_conversion_running = false;	// true = RUN (adquiriendo), false = STOP (reproduciendo en loop)
static volatile bool adc_print_flag = false; 		 		// flag para imprimir el estado RUN/STOP
static volatile bool freq_print_flag = false;		 		// flag para imprimir la frecuencia de muestreo actual
static volatile bool adc_data_ready = false;		 		// flag que indica una muestra ADC nueva (solo se prende en RUN)
static volatile bool dac_output_ready = false;		 		// flag que indica una salida nueva del DAC (se prende en RUN y en STOP)

/**
 * 8 kHz  → R (Rojo)
 * 16 kHz → G (Verde)
 * 22 kHz → B (Azul)
 * 44 kHz → R + G (Amarillo)
 * 48 kHz → R + B (Magenta)
 */

/**
 * @brief Configura el color del LED según la frecuencia de muestreo.
 * @param rate La frecuencia de muestreo para la cual configurar el LED.
 */
void SetLedForSampleRate(sample_rate_t rate) {
  switch (rate) {
  case SAMPLE_RATE_8K:
    // LED para 8 kHz
    LED_RED_ON();
    LED_GREEN_OFF();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_16K:
    // LED para 16 kHz
    LED_RED_OFF();
    LED_GREEN_ON();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_22K:
    // LED para 22 kHz
    LED_RED_OFF();
    LED_GREEN_OFF();
    LED_BLUE_ON();
    break;

  case SAMPLE_RATE_44K:
    // LED para 44 kHz
    LED_RED_ON();
    LED_GREEN_ON();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_48K:
    // LED para 48 kHz
    LED_RED_ON();
    LED_GREEN_OFF();
    LED_BLUE_ON();
    break;
  }
}

/**
 * @brief Configura la frecuencia de muestreo del timer.
 * @param rate La frecuencia de muestreo a configurar.
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
 * @brief Aplica una frecuencia de muestreo: siempre actualiza el LED, pero
 * solo reprograma el timer cuando @p conversion_stop es false. Esto es lo
 * que hace que un cambio de frecuencia en STOP quede "diferido" — el
 * LED/selección se actualiza instantáneamente, mientras que el hardware sigue con
 * la frecuencia vieja hasta el próximo RUN, para no interrumpir la
 * reproducción del buffer en curso.
 * @param rate La frecuencia de muestreo a aplicar.
 * @param conversion_stop True si la adquisición está detenida actualmente.
 */
void ApplySampleRate(sample_rate_t rate, bool conversion_stop) {
  SetLedForSampleRate(rate);
  if (!conversion_stop){
    Timer_SetSampleRate(rate);
  }
}

/**
 * @brief Pasa a la siguiente frecuencia de muestreo y la aplica.
 * @param conversion_stop True si la adquisición está detenida actualmente.
 */
void NextSampleRate(bool conversion_stop) {
  if (current_sample_rate == SAMPLE_RATE_48K) {
    current_sample_rate = SAMPLE_RATE_8K;
  } else {
    current_sample_rate++;
  }

  ApplySampleRate(current_sample_rate, conversion_stop);
}

/**
 * @brief Arranca una adquisición nueva: graba en el buffer desde el
 * principio, a la frecuencia de muestreo indicada.
 *
 * El estado se resetea ANTES de setear is_conversion_running, a propósito:
 * si la ISR del ADC dispara en el medio, todavía ve el estado STOP viejo y
 * simplemente reproduce adc_buffer[0], que el primer tick real de RUN pisa
 * igual (siempre hace read_index = write_index sin condición). No cambiar
 * el orden.
 *
 * @param rate La frecuencia de muestreo a la que grabar.
 */
void ADC_StartConversion(sample_rate_t rate) {
  current_sample_rate = rate;
  write_index = 0;
  read_index = 0;
  Timer_SetSampleRate(rate);
  is_conversion_running = true;
}

/**
 * @brief Detiene la adquisición: el DAC pasa a reproducir el buffer en
 * loop, empezando desde el principio.
 *
 * A diferencia de ADC_StartConversion, el flag se baja ANTES de resetear
 * read_index: si la ISR del ADC dispara en el medio, tiene que ver STOP ya
 * puesto (así toma la rama de reproducción y nunca toca write_index) — si
 * no, un tick de RUN podría pisar read_index justo después de resetearlo,
 * y la reproducción arrancaría desde la posición incorrecta. No cambiar
 * el orden.
 */
void ADC_StopConversion(void) {
  is_conversion_running = false;
  read_index = 0;
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
  ApplySampleRate(current_sample_rate, false);

  while (1) {

    if (adc_print_flag) {
      if (is_conversion_running)
        PRINTF("[RUN]: adquisición activada\r\n");
      else
        PRINTF("[STOP]: adquisición desactivada\r\n");
      adc_print_flag = false;
    }

    if (freq_print_flag){
    	PRINTF("[INFO] Frecuencia de muestreo configurada a %d Hz\r\n",
    	         (current_sample_rate == SAMPLE_RATE_8K)    ? 8000
    	         : (current_sample_rate == SAMPLE_RATE_16K) ? 16000
    	         : (current_sample_rate == SAMPLE_RATE_22K) ? 22000
    	         : (current_sample_rate == SAMPLE_RATE_44K) ? 44000
    	         : (current_sample_rate == SAMPLE_RATE_48K) ? 48000
				 : 0);
    	freq_print_flag = false;
    }

    if (adc_data_ready){
    // Copia solo para debug; se prende solo en RUN, cuando hay una conversión nueva de verdad.
    q15_t sample = last_sample;

		float adcValue;
		arm_q15_to_float(&sample, &adcValue, 1U);	// Debería ser un float entre -1 y 1
//    Ya no se usa, el valor del DAC ahora se calcula en la ISR del ADC:
//		uint32_t dacValue = LPDAC_DATA_DATA(4095.0F * (0.5F + 0.5F * adcValue));
//		uint32_t dacValue = LPDAC_DATA_DATA(((int32_t)adc_buffer[read_index] + 32768) >> 4);

//		PRINTF("[ADC] Q15: %d | Float: %f\r\n", adc_buffer[read_index], adcValue);	// no funciona %f

		int32_t adc_milli = (int32_t)(adcValue * 1000.0f);

		PRINTF("[ADC] uint16: %u | Q15: %d | Decimal: %s%d.%03d\r\n",
				result.convValue,
				sample,
				adc_milli < 0 ? "-" : "\0",
				adc_milli / 1000,
				abs(adc_milli % 1000));

		adc_data_ready = false;
	}

    if (dac_output_ready){
    // Copia solo para debug; se prende en RUN y en STOP, siempre que el DAC sacó un valor nuevo.
    q15_t dac_sample = last_dac_sample;
    uint32_t dacValue = last_dac_value;

    PRINTF("[DAC] Q15: %d | DAC: %u | %s\r\n",
    	dac_sample,
			dacValue,
			is_conversion_running ? "RUN" : "STOP");

    dac_output_ready = false;
	}

  }
  return 0;
}

/**
 * @brief   Manejador de interrupción GPIO00_IRQn.
 * @details Esta función cambia a la siguiente frecuencia de muestreo de la lista.
 */
void GPIO0_INT_0_IRQHANDLER(void) {
  /* Get pin flags 0 */
  uint32_t pin_flags0 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0U);

  /* Place your interrupt code here */

  // Cambia a la siguiente frecuencia de muestreo
  if (is_conversion_running) {
    // RUN: aplica la nueva frecuencia al timer de inmediato.
    NextSampleRate(false);
  } else {
    // STOP: solo actualiza la selección/LED; aplicarla al timer queda
    // diferido hasta el próximo RUN para no interrumpir la reproducción en curso.
    NextSampleRate(true);
  }

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
 * @brief   Manejador de interrupción GPIO01_IRQn.
 * @details Esta función alterna el estado de la adquisición (RUN/STOP).
 */
void GPIO0_INT_1_IRQHANDLER(void) {
  /* Get pin flags 1 */
  uint32_t pin_flags1 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 1U);

  /* Place your interrupt code here */

  // Alterna el estado de la adquisición
  if (is_conversion_running) {
    ADC_StopConversion();
  } else {
    ADC_StartConversion(current_sample_rate);
  }

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

/**
 * @brief Manejador de interrupción ADC1_IRQn. Dispara una vez por período
 * de muestreo, sea cual sea la frecuencia actual, y maneja el DAC en
 * ambos casos:
 *  - RUN: guarda la muestra nueva y la saca de inmediato (monitor en vivo).
 *  - STOP: reproduce el contenido del buffer en loop, sin tocar write_index.
 * En ambos casos el DAC se alimenta de adc_buffer[read_index] a través del
 * mismo código de abajo, así que hay un solo lugar que escribe al DAC.
 */
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

	  adc_buffer[write_index] = (q15_t)((int32_t)result.convValue - 32768);	// -0x8000

    // Apunta read_index a la muestra recién escrita
    read_index = write_index;

	  write_index = (write_index + 1) % ADC_BUFFER_SIZE;

    // Copia para el PRINTF 
    last_sample = adc_buffer[read_index];
    adc_data_ready = true;
  }

  // adc_buffer guarda Q15 con signo (~-32768..32767); el DAC necesita un
  // código sin signo de 12 bits, de ahí el offset +32768 antes del shift.
  uint32_t dacValue = LPDAC_DATA_DATA(((int32_t)adc_buffer[read_index] + 32768) >> 4);
  DAC_SetData(DAC0, dacValue);

  // Copia para el PRINTF
  last_dac_sample = adc_buffer[read_index];
  last_dac_value = dacValue;
  dac_output_ready = true;

  if(!is_conversion_running) {
    read_index = (read_index + 1) % ADC_BUFFER_SIZE;
  }

  /* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
     Store immediate overlapping exception return operation might vector to incorrect interrupt. */
  #if defined __CORTEX_M && (__CORTEX_M == 4U)
    __DSB();
  #endif
}
