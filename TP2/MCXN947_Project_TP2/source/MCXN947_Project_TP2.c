/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    MCXN947_Project_TP2.c
 * @brief   Punto de entrada de la aplicación y control de usuario (TP2 DSP).
 */

#include <stdbool.h> // tipo bool
#include <stdio.h>   // declara printf(), a la que se mapea PRINTF en esta configuración
#include <stdlib.h>  // (reservado, no se usa directamente en este archivo)

#include "arm_math.h"      // tipo q15_t (CMSIS-DSP)
#include "board.h"         // funciones de la placa (LEDs, init de boot, debug console)
#include "clock_config.h"  // BOARD_InitBootClocks() y configuración de PLL/clocks generada por MCUXpresso
#include "fsl_debug_console.h" // macro PRINTF usada en todo este archivo
#include "peripherals.h"   // periféricos inicializados por el Config Tool (ADC1, CTIMER0, VREF0, GPIO0)
#include "pin_mux.h"       // BOARD_InitBootPins() (mux de pines generado por MCUXpresso)

#include "circular_buffer.h"  // tipo circular_buffer_t (usado indirectamente vía pipeline)
#include "adc_stage.h"        // sample_rate_t y control de la etapa de adquisición
#include "processing_stage.h" // processing_mode_t y control del modo de procesamiento DSP
#include "dac_stage.h"        // etapa de salida analógica (incluida por completitud del pipeline)
#include "uart_stage.h"       // comandos por UART: try_getchar, streaming, is_streaming_enabled
#include "pipeline.h"         // coordinador que conecta todas las etapas (init, step, control RUN/STOP/frecuencia)

/* Flags de notificación para impresión en el loop principal (se setean desde ISRs o
 * desde ProcessUartCommands, y se consumen y limpian en el while(1) de main) */
static volatile bool adc_print_flag = false;    // pide imprimir el estado RUN/STOP
static volatile bool freq_print_flag = false;   // pide imprimir la frecuencia de muestreo actual
static volatile bool buffer_dump_flag = false;  // pide volcar el buffer circular por UART

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
      // Alterna RUN/STOP y pide que se imprima el nuevo estado en el loop principal
      pipeline_toggle_run_stop();
      adc_print_flag = true;
      break;

    case 'f':
    case 'F': {
      // Avanza a la siguiente frecuencia, actualiza el LED y pide imprimir la nueva frecuencia
      sample_rate_t next = pipeline_next_sample_rate();
      SetLedForSampleRate(next);
      freq_print_flag = true;
      break;
    }

    case 'd':
    case 'D':
      // Solo levanta la bandera: el volcado real se hace en el loop principal, no en esta función
      buffer_dump_flag = true;
      break;

    case 'p':
    case 'P': {
      // El modo activo reserva el UART para líneas CSV compatibles con Serial-Oscilloscope.
      bool streaming = !uart_stage_is_streaming_enabled();
      uart_stage_enable_streaming(streaming);
      if (!streaming) {
        PRINTF("[UART] Streaming Serial Plotter: DESACTIVADO\r\n");
      }
      break;
    }

    case 'o':
    case 'O': {
      // Alterna el formato de streaming sin imprimir texto que contamine el CSV.
      uart_stream_mode_t current = uart_stage_get_stream_mode();
      uart_stream_mode_t next = (current == UART_STREAM_MODE_PLOTTER)
                                    ? UART_STREAM_MODE_OSCILLOSCOPE
                                    : UART_STREAM_MODE_PLOTTER;
      uart_stage_set_stream_mode(next);
      if (!uart_stage_is_streaming_enabled()) {
        PRINTF("[UART] Formato streaming: %s\r\n",
               next == UART_STREAM_MODE_OSCILLOSCOPE ? "SERIAL-OSCILLOSCOPE" : "PLOTTER");
      }
      break;
    }

    case 'm':
    case 'M': {
      // Avanza cíclicamente al siguiente modo de procesamiento DSP y lo confirma por consola
      processing_mode_t cur = processing_stage_get_mode();
      processing_mode_t next = (processing_mode_t)((cur + 1) % 4);
      processing_stage_set_mode(next);
      const char *mode_names[] = {"PASSTHROUGH", "FLOAT_CONV", "INVERT", "GAIN"};
      if (!uart_stage_is_streaming_enabled()) {
        PRINTF("[DSP] Modo de procesamiento: %s\r\n", mode_names[next]);
      }
      break;
    }

    case 'h':
    case 'H':
    case '?':
      // Imprime el menú de ayuda con todos los comandos disponibles
      if (!uart_stage_is_streaming_enabled()) {
        PRINTF("\r\n=== COMANDOS UART DISPONIBLES ===\r\n");
        PRINTF("  'r': Alternar RUN / STOP\r\n");
        PRINTF("  'f': Cambiar frecuencia de muestreo (8k, 16k, 22k, 44k, 48k)\r\n");
        PRINTF("  'd': Volcar las 512 muestras del buffer por UART (formato CSV)\r\n");
        PRINTF("  'p': Activar/Desactivar streaming continuo para Serial Plotter\r\n");
        PRINTF("  'o': Alternar formato Plotter / Serial-Oscilloscope\r\n");
        PRINTF("  'm': Alternar modo de procesamiento DSP\r\n");
        PRINTF("  'h': Mostrar esta ayuda\r\n");
        PRINTF("=================================\r\n\r\n");
      }
      break;

    default:
      // Carácter no reconocido: se ignora
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
      if (!uart_stage_is_streaming_enabled()) {
        if (pipeline_is_running()) {
          PRINTF("[RUN]: Adquisicion activada\r\n");
        } else {
          PRINTF("[STOP]: Adquisicion pausada (reproduciendo buffer en loop)\r\n");
        }
      }
      adc_print_flag = false;
    }

    /* 4. Notificación de frecuencia de muestreo */
    if (freq_print_flag) {
      if (!uart_stage_is_streaming_enabled()) {
        PRINTF("[INFO] Frecuencia de muestreo: %u Hz\r\n", (unsigned int)pipeline_get_sample_rate_hz());
      }
      freq_print_flag = false;
    }

    /* 5. Volcado de buffer solicitado */
    if (buffer_dump_flag) {
      if (!uart_stage_is_streaming_enabled()) {
        pipeline_dump_to_uart(true);
      }
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
