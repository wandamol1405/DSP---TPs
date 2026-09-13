/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    MCXN947_Project_TP1.c
 * @brief   Punto de entrada de la aplicación y control de usuario (TP1 DSP).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "arm_math.h"
#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "peripherals.h"
#include "pin_mux.h"

#include "circular_buffer.h"
#include "adc_stage.h"
#include "processing_stage.h"
#include "dac_stage.h"
#include "uart_stage.h"
#include "pipeline.h"

/* Flags de notificación para impresión en el loop principal */
static volatile bool adc_print_flag = false;
static volatile bool freq_print_flag = false;
static volatile bool buffer_dump_flag = false;

/**
 * @brief Configura el color del LED según la frecuencia de muestreo.
 *
 * 8 kHz  → R (Rojo)
 * 16 kHz → G (Verde)
 * 22 kHz → B (Azul)
 * 44 kHz → R + G (Amarillo)
 * 48 kHz → R + B (Magenta)
 */
void SetLedForSampleRate(sample_rate_t rate) {
  switch (rate) {
  case SAMPLE_RATE_8K:
    LED_RED_ON();
    LED_GREEN_OFF();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_16K:
    LED_RED_OFF();
    LED_GREEN_ON();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_22K:
    LED_RED_OFF();
    LED_GREEN_OFF();
    LED_BLUE_ON();
    break;

  case SAMPLE_RATE_44K:
    LED_RED_ON();
    LED_GREEN_ON();
    LED_BLUE_OFF();
    break;

  case SAMPLE_RATE_48K:
    LED_RED_ON();
    LED_GREEN_OFF();
    LED_BLUE_ON();
    break;

  default:
    break;
  }
}

/**
 * @brief Procesa comandos recibidos por el puerto serie (no bloqueante).
 */
static void ProcessUartCommands(void) {
  char ch = 0;
  if (uart_stage_try_getchar(&ch)) {
    switch (ch) {
    case 'r':
    case 'R':
      pipeline_toggle_run_stop();
      adc_print_flag = true;
      break;

    case 'f':
    case 'F': {
      sample_rate_t next = pipeline_next_sample_rate();
      SetLedForSampleRate(next);
      freq_print_flag = true;
      break;
    }

    case 'd':
    case 'D':
      buffer_dump_flag = true;
      break;

    case 'p':
    case 'P': {
      bool streaming = !uart_stage_is_streaming_enabled();
      uart_stage_enable_streaming(streaming);
      PRINTF("[UART] Streaming Serial Plotter: %s\r\n", streaming ? "ACTIVADO" : "DESACTIVADO");
      break;
    }

    case 'm':
    case 'M': {
      processing_mode_t cur = processing_stage_get_mode();
      processing_mode_t next = (processing_mode_t)((cur + 1) % 4);
      processing_stage_set_mode(next);
      const char *mode_names[] = {"PASSTHROUGH", "FLOAT_CONV", "INVERT", "GAIN"};
      PRINTF("[DSP] Modo de procesamiento: %s\r\n", mode_names[next]);
      break;
    }

    case 'h':
    case 'H':
    case '?':
      PRINTF("\r\n=== COMANDOS UART DISPONIBLES ===\r\n");
      PRINTF("  'r': Alternar RUN / STOP\r\n");
      PRINTF("  'f': Cambiar frecuencia de muestreo (8k, 16k, 22k, 44k, 48k)\r\n");
      PRINTF("  'd': Volcar las 512 muestras del buffer por UART (formato CSV)\r\n");
      PRINTF("  'p': Activar/Desactivar streaming continuo para Serial Plotter\r\n");
      PRINTF("  'm': Alternar modo de procesamiento DSP\r\n");
      PRINTF("  'h': Mostrar esta ayuda\r\n");
      PRINTF("=================================\r\n\r\n");
      break;

    default:
      break;
    }
  }
}

/**
 * @brief Application entry point.
 */
int main(void) {
  /* Inicialización de hardware de la placa */
  BOARD_InitBootPins();
  BOARD_InitBootClocks();
  BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
  BOARD_InitDebugConsole();
#endif

  PRINTF("\r\n========================================\r\n");
  PRINTF("  TP1 DSP - FRDM-MCXN947 Modularizado\r\n");
  PRINTF("  Etapas: ADC -> Buffer -> DSP -> DAC/UART\r\n");
  PRINTF("  Envie 'h' para ver comandos por consola\r\n");
  PRINTF("========================================\r\n\r\n");

  /* Inicialización del pipeline y sus etapas */
  pipeline_init();

  /* Configuración inicial: frecuencia 8 kHz, modo STOP diferido */
  sample_rate_t initial_rate = pipeline_get_sample_rate();
  SetLedForSampleRate(initial_rate);
  pipeline_set_sample_rate(initial_rate);

  freq_print_flag = true;

  while (1) {
    /* 1. Tarea periódica de transmisión UART (Serial Plotter) */
    uart_stage_task();

    /* 2. Procesamiento de comandos de entrada por UART */
    ProcessUartCommands();

    /* 3. Notificación de estado RUN / STOP */
    if (adc_print_flag) {
      if (pipeline_is_running()) {
        PRINTF("[RUN]: Adquisicion activada\r\n");
      } else {
        PRINTF("[STOP]: Adquisicion pausada (reproduciendo buffer en loop)\r\n");
      }
      adc_print_flag = false;
    }

    /* 4. Notificación de frecuencia de muestreo */
    if (freq_print_flag) {
      PRINTF("[INFO] Frecuencia de muestreo: %u Hz\r\n", (unsigned int)pipeline_get_sample_rate_hz());
      freq_print_flag = false;
    }

    /* 5. Volcado de buffer solicitado */
    if (buffer_dump_flag) {
      pipeline_dump_to_uart(true);
      buffer_dump_flag = false;
    }
  }

  return 0;
}

/**
 * @brief Manejador de interrupción GPIO00_IRQn (Pulsador SW3).
 *        Cambia a la siguiente frecuencia de muestreo.
 */
void GPIO0_INT_0_IRQHANDLER(void) {
  uint32_t pin_flags0 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0U);

  sample_rate_t new_rate = pipeline_next_sample_rate();
  SetLedForSampleRate(new_rate);
  freq_print_flag = true;

  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags0, 0U);

#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

/**
 * @brief Manejador de interrupción GPIO01_IRQn (Pulsador SW2).
 *        Alterna el estado entre RUN y STOP.
 */
void GPIO0_INT_1_IRQHANDLER(void) {
  uint32_t pin_flags1 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 1U);

  pipeline_toggle_run_stop();
  adc_print_flag = true;

  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags1, 1U);

#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

/**
 * @brief Manejador de interrupción ADC1_IRQn.
 *        Dispara una vez por período de muestreo (8k, 16k, 22k, 44k, 48k).
 *        Ejecuta el paso del pipeline de manera desacoplada:
 *        ADC -> Buffer Circular -> Procesamiento DSP -> Salidas DAC / UART.
 */
void ADC1_IRQHANDLER(void) {
  /* Limpia las banderas de hardware del LPADC */
  adc_stage_clear_flags();

  /* Ejecuta un paso del pipeline */
  pipeline_step();

#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}
