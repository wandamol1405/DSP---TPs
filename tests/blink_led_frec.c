/*
 * Copyright 2016-2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file    MCXN947_Project.c
 * @brief   Application entry point.
 */
#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "peripherals.h"
#include "pin_mux.h"
#include <stdio.h>
/* TODO: insert other include files here. */

/* TODO: insert other definitions and declarations here. */

/*
 * @brief   Application entry point.
 */

/* Match values de CTIMER0_MATCH_0_CHANNEL para las 4 frecuencias de
 * blinkeo. Cada uno es la mitad del anterior (doble frecuencia). Ajustar
 * segun el clock real de CTIMER0 configurado en Peripherals Tool. */
static const uint32_t FREC_MATCH_VALUES[4] = {249999u, 124999u, 62499u,
                                               31249u};
static volatile uint8_t frec_index = 0;

static const ctimer_match_config_t CTIMER0_matchConfig = {
    .matchValue = 249999,
    .enableCounterReset = true,
    .enableCounterStop = false,
    .outControl = kCTIMER_Output_NoAction,
    .outPinInitState = false,
    .enableInterrupt = true};

/* Colores del LED RGB (LEDs activos por bajo: 0 = prendido, 1 = apagado).
 * Se ciclan con cada interrupcion de GPIO0_INT_1_IRQHANDLER. */
typedef struct {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
} led_color_t;

static const led_color_t LED_COLORS[3] = {
    {0U, 1U, 1U}, /* Rojo */
    {1U, 0U, 1U}, /* Verde */
    {1U, 1U, 0U}, /* Azul */
};
static volatile uint8_t color_index = 0;

int main(void) {

  /* Init board hardware. */
  BOARD_InitBootPins();
  BOARD_InitBootClocks();
  BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
  /* Init FSL debug console. */
  BOARD_InitDebugConsole();
#endif

  PRINTF("Hello World\r\n");

  /* Force the counter to be placed into memory. */
  volatile static int i = 0;
  /* Enter an infinite loop, just incrementing a counter. */
  while (1) {
    i++;
    /* 'Dummy' NOP to allow source level single stepping of
        tight while() loop */
    __asm volatile("nop");
  }
  return 0;
}

/*
 * @brief Callback de CTIMER0: togglea el LED RGB en cada match, generando
 * el blinkeo, en el color vigente (color_index). La frecuencia la
 * determina el matchValue vigente, cambiado desde GPIO0_INT_0_IRQHANDLER;
 * el color se cambia desde GPIO0_INT_1_IRQHANDLER.
 */
void CTIMER0_Callback(uint32_t flags) {
  static uint8_t led_on = 0;
  led_on ^= 1U;

  if (led_on) {
    GPIO_PinWrite(BOARD_INITLEDSPINS_LED_RED_GPIO,
                  BOARD_INITLEDSPINS_LED_RED_PIN, LED_COLORS[color_index].red);
    GPIO_PinWrite(BOARD_INITLEDSPINS_LED_GREEN_GPIO,
                  BOARD_INITLEDSPINS_LED_GREEN_PIN,
                  LED_COLORS[color_index].green);
    GPIO_PinWrite(BOARD_INITLEDSPINS_LED_BLUE_GPIO,
                  BOARD_INITLEDSPINS_LED_BLUE_PIN,
                  LED_COLORS[color_index].blue);
  } else {
    GPIO_PinWrite(BOARD_INITLEDSPINS_LED_RED_GPIO,
                  BOARD_INITLEDSPINS_LED_RED_PIN, 1U);
    GPIO_PinWrite(BOARD_INITLEDSPINS_LED_GREEN_GPIO,
                  BOARD_INITLEDSPINS_LED_GREEN_PIN, 1U);
    GPIO_PinWrite(BOARD_INITLEDSPINS_LED_BLUE_GPIO,
                  BOARD_INITLEDSPINS_LED_BLUE_PIN, 1U);
  }
}

// TODO: REVISAR POR QUE NO FUNCIONA EN LA PLACA
/* GPIO00_IRQn interrupt handler */
void GPIO0_INT_0_IRQHANDLER(void) {
  /* Get pin flags 0 */
  uint32_t pin_flags0 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0U);

  /* Place your interrupt code here */
  /* Cada pulsacion del boton avanza a la siguiente de las 4 frecuencias */
  frec_index = (frec_index + 1U) % 4U;

  ctimer_match_config_t new_config = CTIMER0_matchConfig;
  new_config.matchValue = FREC_MATCH_VALUES[frec_index];

  CTIMER_SetupMatch(CTIMER0_PERIPHERAL, CTIMER0_MATCH_0_CHANNEL,
                    &new_config);

  PRINTF("Frecuencia cambiada a indice %u (match=%u)\r\n", frec_index,
         FREC_MATCH_VALUES[frec_index]);

  /* Clear pin flags 0 */
  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags0, 0U);

/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
   Store immediate overlapping exception return operation might vector to
   incorrect interrupt. */
#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

/* GPIO00_IRQn interrupt handler (segundo boton, canal de interrupcion 1) */
void GPIO0_INT_1_IRQHANDLER(void) {
  /* Get pin flags 1 */
  uint32_t pin_flags1 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 1U);

  /* Place your interrupt code here */
  /* Cada pulsacion del boton avanza al siguiente color del LED RGB */
  color_index = (color_index + 1U) % 3U;

  /* Clear pin flags 1 */
  GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags1, 1U);

/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
   Store immediate overlapping exception return operation might vector to
   incorrect interrupt. */
#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}
