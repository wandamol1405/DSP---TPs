/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    MCXN947_Project_TP3.c
 * @brief   Punto de entrada de la aplicación y control de usuario (TP3 DSP - FFT).
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
#include "uart_stage.h"       // comandos por UART, turno de texto y envío de tramas
#include "pipeline.h"         // coordinador que conecta todas las etapas (init, step, control RUN/STOP/frecuencia)
#include "ui.h"               // interfaz gráfica en el display OLED (independiente del hardware)

/* Avisos pendientes de imprimir. Los levantan las ISRs de los pulsadores y
 * ProcessUartCommands(), que nunca escriben por la UART: el texto sale después, desde
 * PrintPendingStatus(), cuando el loop principal consigue el turno entre dos tramas. */
static volatile bool adc_print_flag = false;      // pide imprimir el estado RUN/STOP
static volatile bool freq_print_flag = false;     // pide imprimir la frecuencia de muestreo actual
static volatile bool dsp_mode_print_flag = false; // pide imprimir el modo de procesamiento DSP actual
static volatile bool stream_print_flag = false;   // pide imprimir si el envío continuo quedó activo
static volatile bool stats_print_flag = false;    // pide imprimir los contadores de la UART ('t')
static volatile bool help_print_flag = false;     // pide imprimir la ayuda ('h')
static volatile bool overrun_print_flag = false;  // pide avisar que se descartó una ventana
static volatile bool ui_update_flag = false;      // pide sincronizar el estado del DSP hacia la UI

/**
 * @brief Publica en la UI el estado actual del pipeline DSP (modo, N, Fs y pico).
 *        La UI decide internamente qué regiones del display cambiaron realmente.
 */
static void UpdateUiFromDsp(void) {
  processing_mode_t mode = pipeline_get_processing_mode();
  ui_mode_t ui_mode = (mode == PROCESSING_MODE_FFT) ? UI_MODE_FFT : UI_MODE_BYPASS;

  ui_set_mode(ui_mode);
  ui_set_window_size((uint16_t)pipeline_get_fft_size_n());
  ui_set_sampling_frequency(pipeline_get_sample_rate_hz());
  ui_set_peak_frequency(pipeline_get_peak_frequency_hz());
}

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
 * @brief Procesa todos los comandos que llegaron por el puerto serie (no bloqueante).
 *        Aplica el cambio en el momento y deja el aviso pendiente; no escribe por la UART,
 *        así que se puede llamar siempre, aunque esté saliendo una trama.
 */
static void ProcessUartCommands(void) {
  char ch = 0;
  while (uart_stage_try_getchar(&ch)) {
    switch (ch) {
    case 'r':
    case 'R':
      // Alterna RUN/STOP
      pipeline_toggle_run_stop();
      adc_print_flag = true;
      break;

    case 'f':
    case 'F': {
      // Avanza a la siguiente frecuencia de muestreo y actualiza el LED (igual que SW2)
      sample_rate_t next = pipeline_next_sample_rate();
      SetLedForSampleRate(next);
      freq_print_flag = true;
      ui_update_flag = true;
      break;
    }

    case 'm':
    case 'M':
      // Mismo ciclo que SW3: BYPASS -> FFT 512 -> FFT 1024 -> FFT 2048 -> BYPASS
      pipeline_next_processing_mode();
      dsp_mode_print_flag = true;
      ui_update_flag = true;
      break;

    case 'c':
    case 'C':
      // Envío continuo de tramas: cada ventana que encuentre la UART libre
      uart_stage_enable_binary_streaming(!uart_stage_is_binary_streaming_enabled());
      stream_print_flag = true;
      break;

    case 's':
    case 'S':
      // Pide una única trama, sin activar el envío continuo. La manda PendSV con la
      // próxima ventana que encuentre la UART libre.
      uart_stage_request_single_frame();
      break;

    case 't':
    case 'T':
      stats_print_flag = true;
      break;

    case 'h':
    case 'H':
    case '?':
      help_print_flag = true;
      break;

    default:
      // Carácter no reconocido: se ignora
      break;
    }
  }
}

/**
 * @brief Indica si hay algún aviso pendiente de imprimir.
 */
static bool HasPendingText(void) {
  return adc_print_flag || freq_print_flag || dsp_mode_print_flag || stream_print_flag ||
         stats_print_flag || help_print_flag || overrun_print_flag;
}

/**
 * @brief Imprime los avisos pendientes que dejaron las ISRs y los comandos. Llamar solo
 *        con el turno de texto de la UART tomado. Cada bandera se baja antes de imprimir:
 *        si una ISR la vuelve a levantar mientras tanto, el aviso sale en la vuelta
 *        siguiente en lugar de perderse.
 */
static void PrintPendingStatus(void) {
  /* Ayuda */
  if (help_print_flag) {
    help_print_flag = false;
    PRINTF("\r\n=== COMANDOS UART DISPONIBLES ===\r\n");
    PRINTF("  'm': Cambiar modo: BYPASS -> FFT 512 -> FFT 1024 -> FFT 2048 [SW3]\r\n");
    PRINTF("  'f': Cambiar frecuencia de muestreo (8k, 16k, 22k, 44k, 48k) [SW2]\r\n");
    PRINTF("  'r': Alternar RUN / STOP\r\n");
    PRINTF("  'c': Activar/Desactivar el envio continuo de tramas\r\n");
    PRINTF("  's': Enviar una unica trama\r\n");
    PRINTF("  't': Mostrar el estado y los contadores de la UART\r\n");
    PRINTF("  'h': Mostrar esta ayuda\r\n");
    PRINTF("=================================\r\n\r\n");
  }

  /* Estado RUN / STOP */
  if (adc_print_flag) {
    adc_print_flag = false;
    if (pipeline_is_running()) {
      PRINTF("[RUN]: Adquisicion activada\r\n");
    } else {
      PRINTF("[STOP]: Adquisicion pausada (reproduciendo buffer en loop)\r\n");
    }
  }

  /* Frecuencia de muestreo */
  if (freq_print_flag) {
    freq_print_flag = false;
    PRINTF("[INFO] Frecuencia de muestreo: %u Hz\r\n", (unsigned int)pipeline_get_sample_rate_hz());
  }

  /* Modo DSP */
  if (dsp_mode_print_flag) {
    dsp_mode_print_flag = false;
    PRINTF("[DSP] Modo: %s  N=%u\r\n", pipeline_get_processing_mode_name(),
           (unsigned int)pipeline_get_fft_size_n());
  }

  /* Envío continuo */
  if (stream_print_flag) {
    stream_print_flag = false;
    PRINTF("[UART] Envio continuo: %s\r\n",
           uart_stage_is_binary_streaming_enabled() ? "ACTIVADO" : "DESACTIVADO");
  }

  /* Sobrecarga: se completó una ventana antes de que PendSV terminara con la anterior, y
   * se descartó (el modo no cambia) */
  if (overrun_print_flag) {
    overrun_print_flag = false;
    PRINTF("[DSP] Sobrecarga a %u Hz: ventana descartada (total descartadas: %u)\r\n",
           (unsigned int)pipeline_get_sample_rate_hz(), (unsigned int)pipeline_get_overrun_count());
  }

  /* Estado y contadores ('t') */
  if (stats_print_flag) {
    stats_print_flag = false;
    PRINTF("[ESTADO] %s  Fs=%u Hz  Modo: %s  N=%u  Envio continuo: %s\r\n",
           pipeline_is_running() ? "RUN" : "STOP", (unsigned int)pipeline_get_sample_rate_hz(),
           pipeline_get_processing_mode_name(), (unsigned int)pipeline_get_fft_size_n(),
           uart_stage_is_binary_streaming_enabled() ? "SI" : "NO");
    PRINTF("[UART] Tramas: %u enviadas, %u descartadas. Comandos perdidos: %u\r\n",
           (unsigned int)uart_stage_get_frames_sent(), (unsigned int)uart_stage_get_frames_dropped(),
           (unsigned int)uart_stage_get_rx_lost());
    PRINTF("[DSP] Ventanas descartadas por sobrecarga: %u\r\n", (unsigned int)pipeline_get_overrun_count());
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

  // Este cartel se imprime sin pedir el turno de texto porque corre antes de
  // pipeline_init(): todavía no hay ninguna trama que pueda salir.
  PRINTF("\r\n========================================\r\n");
  PRINTF("  TP3 DSP - FFT (FRDM-MCXN947)\r\n");
#if defined(DEBUG)
  PRINTF("  [BUILD: DEBUG (-O0, sin optimizar)]\r\n");
#else
  PRINTF("  [BUILD: RELEASE (optimizado, SIMD/FPU activo)]\r\n");
#endif
  PRINTF("  Etapas: ADC -> Ventana N -> FFT/Bypass -> UART/OLED\r\n");
  PRINTF("  Envie 'h' para ver comandos por consola\r\n");
  PRINTF("========================================\r\n\r\n");

  /* Inicialización del pipeline y sus etapas */
  pipeline_init();

  /* Configuración inicial: frecuencia 8 kHz, modo STOP diferido */
  sample_rate_t initial_rate = pipeline_get_sample_rate();
  SetLedForSampleRate(initial_rate);
  pipeline_set_sample_rate(initial_rate);

  /* Inicialización de la interfaz gráfica (OLED): dibuja la pantalla completa y queda lista
   * para aplicar actualizaciones parciales en el loop principal */
  ui_init();

  freq_print_flag = true;
  ui_update_flag = true;

  while (1) {
    /* 1. Comandos: se atienden en cada vuelta, también mientras sale una trama. Solo
     *    cambian el estado y dejan avisos pendientes; no escriben por la UART. */
    ProcessUartCommands();
    if (pipeline_take_overrun()) {
      overrun_print_flag = true;
    }

    /* 2. Texto: sale solo entre dos tramas, porque texto y trama comparten LPUART4 y se
     *    mezclarían los bytes. Si hay una trama saliendo, el turno queda pedido: no
     *    arranca otra trama y el texto sale en cuanto termine la actual. */
    if (HasPendingText() && uart_stage_try_begin_text()) {
      PrintPendingStatus();
      uart_stage_end_text();
    }

    /* 3. Actualización de la interfaz gráfica (OLED): solo fuera de ISR */
    if (pipeline_take_peak_updated()) {
      ui_set_peak_frequency(pipeline_get_peak_frequency_hz());
    }
    if (ui_update_flag) {
      ui_update_flag = false;
      UpdateUiFromDsp();
    }
    ui_process();
  }

  return 0;
}

/**
 * @brief Manejador de PendSV (prioridad mínima, configurada en pipeline_init).
 *        Lo dispara pipeline_step cada vez que se completa una ventana de N muestras:
 *        la FFT (o el bypass) y el envío por UART se hacen acá, fuera de la ISR del ADC.
 */
void PendSV_Handler(void) {
  pipeline_process_block();
}

/**
 * @brief Manejador de interrupción GPIO00_IRQn (Pulsador SW2).
 *        Cambia a la siguiente frecuencia de muestreo.
 */
void GPIO0_INT_0_IRQHANDLER(void) {
  uint32_t pin_flags0 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0U);

  sample_rate_t new_rate = pipeline_next_sample_rate();
  SetLedForSampleRate(new_rate);
  freq_print_flag = true;
  ui_update_flag = true;

  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags0, 0U);

#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

/**
 * @brief Manejador de interrupción GPIO01_IRQn (Pulsador SW3).
 *        Avanza en el ciclo BYPASS -> FFT 512 -> FFT 1024 -> FFT 2048 -> BYPASS.
 */
void GPIO0_INT_1_IRQHANDLER(void) {
  uint32_t pin_flags1 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 1U);

  pipeline_next_processing_mode();
  dsp_mode_print_flag = true;
  ui_update_flag = true;

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
