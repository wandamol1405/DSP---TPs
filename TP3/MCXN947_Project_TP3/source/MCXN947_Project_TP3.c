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
#include "uart_stage.h"       // comandos por UART: try_getchar, streaming, is_streaming_enabled
#include "pipeline.h"         // coordinador que conecta todas las etapas (init, step, control RUN/STOP/frecuencia)
#include "ui.h"               // interfaz gráfica en el display OLED (independiente del hardware)

/* Flags de notificación para impresión en el loop principal (se setean desde ISRs o
 * desde ProcessUartCommands, y se consumen y limpian en el while(1) de main) */
static volatile bool adc_print_flag = false;      // pide imprimir el estado RUN/STOP
static volatile bool freq_print_flag = false;     // pide imprimir la frecuencia de muestreo actual
static volatile bool buffer_dump_flag = false;    // pide volcar el buffer circular por UART
static volatile bool dsp_mode_print_flag = false; // pide imprimir el modo de procesamiento DSP actual
static volatile bool ui_update_flag = false;      // pide sincronizar el estado del DSP hacia la UI

/**
 * @brief Parámetros nominales de visualización asociados a cada modo del DSP.
 *        Sin los filtros FIR del TP2, solo queda BYPASS (sin Fc ni BW).
 */
typedef struct {
  ui_filter_t ui_type;
  uint32_t fc_hz;
  uint32_t bw_hz;
} dsp_mode_ui_info_t;

static const dsp_mode_ui_info_t s_mode_ui_info[PROCESSING_MODE_COUNT] = {
    [PROCESSING_MODE_BYPASS] = {UI_FILTER_BYPASS, UI_FREQUENCY_NONE, UI_FREQUENCY_NONE},
};

/**
 * @brief Publica en la UI el estado actual del pipeline DSP (Fs, filtro, Fc, BW y taps).
 *        La UI decide internamente qué regiones del display cambiaron realmente.
 */
static void UpdateUiFromDsp(void) {
  processing_mode_t mode = pipeline_get_processing_mode();
  if ((unsigned int)mode >= (unsigned int)PROCESSING_MODE_COUNT) {
    mode = PROCESSING_MODE_BYPASS;
  }

  ui_set_sampling_frequency(pipeline_get_sample_rate_hz());
  ui_set_filter_type(s_mode_ui_info[mode].ui_type);
  ui_set_cutoff_frequency(s_mode_ui_info[mode].fc_hz);
  ui_set_bandwidth(s_mode_ui_info[mode].bw_hz);
  ui_set_filter_taps(pipeline_get_num_taps());
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
      ui_update_flag = true;
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
      // TODO(TP3): cuando exista una ventana de salida FFT/Bypass real del pipeline
      // (issues #14/#15), este 'p' pasa a imprimir esa ventana con
      // uart_stage_print_samples() en vez del streaming muestra-a-muestra de TP2
      // -- ver el comentario grande en uart_stage.h.
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
      if (!uart_stage_any_streaming_active()) {
        PRINTF("[UART] Formato streaming: %s\r\n",
               next == UART_STREAM_MODE_OSCILLOSCOPE ? "SERIAL-OSCILLOSCOPE" : "PLOTTER");
      }
      break;
    }

    case 'c':
    case 'C': {
      // Streaming binario continuo para la GUI (TP3/tools/fft_gui.py, rama de GUI).
      // Mutuamente excluyente con 'p': uart_stage_enable_binary_streaming() ya se
      // encarga de apagar el de texto si estaba activo.
      bool enable = !uart_stage_is_binary_streaming_enabled();
      uart_stage_enable_binary_streaming(enable);
      if (!enable) {
        PRINTF("[UART] Streaming binario: DESACTIVADO\r\n");
      }
      break;
    }

    case 's':
    case 'S':
      // Pide una unica ventana de salida (FFT o Bypass) sin activar el modo continuo.
      // La consume quien produzca la ventana (todavia no existe, ver issues #14/#15)
      // junto con uart_stage_is_binary_streaming_enabled() para decidir si enviarla.
      uart_stage_request_single_frame();
      break;

    case 't':
    case 'T':
      if (!uart_stage_any_streaming_active()) {
        PRINTF("[UART] Tramas binarias: %u enviadas, %u descartadas\r\n",
               (unsigned int)uart_stage_get_frames_sent(), (unsigned int)uart_stage_get_frames_dropped());
      }
      break;

    case 'm':
    case 'M': {
      // Avanza ciclicamente al siguiente modo de procesamiento DSP.
      // TODO(TP3): el equipo decidio que este comando/SW3 va a recorrer Bypass y
      // los 3 tamanos de ventana de FFT (512/1024/2048) en un solo ciclo -- eso lo
      // implementa quien se ocupe de las issues #14/#15 (ventanas N + FFT/Bypass),
      // no esta issue. Una vez mergeado ese cambio, actualizar el texto de ayuda de
      // 'm' mas abajo y el comentario del handler de SW3 para reflejar los modos reales.
      pipeline_next_processing_mode();
      dsp_mode_print_flag = true;
      ui_update_flag = true;
      break;
    }

    case 'h':
    case 'H':
    case '?':
      // Imprime el menú de ayuda con todos los comandos disponibles
      if (!uart_stage_any_streaming_active()) {
        PRINTF("\r\n=== COMANDOS UART DISPONIBLES ===\r\n");
        PRINTF("  'r': Alternar RUN / STOP\r\n");
        PRINTF("  'f': Cambiar frecuencia de muestreo (8k, 16k, 22k, 44k, 48k) [SW2]\r\n");
        PRINTF("  'd': Volcar las 512 muestras del buffer por UART (formato CSV)\r\n");
        PRINTF("  'p': Activar/Desactivar streaming de TEXTO (excluyente con 'c')\r\n");
        PRINTF("  'o': Alternar formato Plotter / Serial-Oscilloscope\r\n");
        PRINTF("  'm': Alternar modo de procesamiento DSP [SW3]\r\n");
        PRINTF("  'c': Activar/Desactivar streaming BINARIO para la GUI (excluyente con 'p')\r\n");
        PRINTF("  's': Enviar/imprimir una unica ventana de salida\r\n");
        PRINTF("  't': Mostrar tramas binarias enviadas/descartadas\r\n");
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

  // TODO(TP3, issues #14/#15): "Etapas: ADC -> Ventana N -> FFT/Bypass -> UART/OLED"
  // describe el plan, no el pipeline actual (sigue siendo el bloque de 32 heredado
  // de TP2, sin ventana N ni FFT real) -- revisar que siga siendo cierto una vez
  // implementadas esas issues, y a esa altura ya no hace falta este TODO.
  //
  // Este banner (y el resto de este bloque) no esta guardado por
  // uart_stage_any_streaming_active() porque corre antes de que exista cualquier
  // estado de streaming (arranca en false). El unico caso en que molestaria es un
  // reset de la placa con la GUI ya conectada en modo 'c' -- improbable pero real:
  // si llega a pasar, el parser de la GUI lo loguea como texto sin romper nada (no
  // hay forma de que este texto ASCII genere por accidente el sync 0xA5 0x5A), asi
  // que no es grave, solo ensucia el log hasta la proxima trama binaria.
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
    /* 1. Tarea periódica de transmisión UART (Serial Plotter) */
    uart_stage_task();

    /* 2. Procesamiento de comandos de entrada por UART */
    ProcessUartCommands();

    /* 3. Notificación de estado RUN / STOP */
    if (adc_print_flag) {
      if (!uart_stage_any_streaming_active()) {
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
      if (!uart_stage_any_streaming_active()) {
        PRINTF("[INFO] Frecuencia de muestreo: %u Hz\r\n", (unsigned int)pipeline_get_sample_rate_hz());
      }
      freq_print_flag = false;
    }

    /* 5. Volcado de buffer solicitado */
    if (buffer_dump_flag) {
      if (!uart_stage_any_streaming_active()) {
        pipeline_dump_to_uart(true);
      }
      buffer_dump_flag = false;
    }

    /* 6. Aviso de sobrecarga: el filtro no llegó a procesar un bloque a tiempo y se pasó a BYPASS */
    if (pipeline_take_overrun()) {
      if (!uart_stage_any_streaming_active()) {
        PRINTF("[DSP] Sobrecarga: el filtro no entra en tiempo real a %u Hz -> BYPASS (bloques perdidos: %u)\r\n",
               (unsigned int)pipeline_get_sample_rate_hz(), (unsigned int)pipeline_get_overrun_count());
      }
      dsp_mode_print_flag = true;
      ui_update_flag = true;
    }

    /* 7. Notificación de modo DSP */
    if (dsp_mode_print_flag) {
      if (!uart_stage_any_streaming_active()) {
        PRINTF("[DSP] Modo/Filtro activo: %s\r\n", pipeline_get_processing_mode_name());
      }
      dsp_mode_print_flag = false;
    }

    /* 8. Actualización de la interfaz gráfica (OLED): solo fuera de ISR */
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
 *        Lo dispara pipeline_step cada vez que se completa un bloque de muestras:
 *        el filtrado FIR se hace acá, fuera de la ISR del ADC.
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
 *        Alterna el modo de procesamiento DSP (ver TODO en el case 'm' de
 *        ProcessUartCommands para el ciclo de modos que falta implementar).
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
