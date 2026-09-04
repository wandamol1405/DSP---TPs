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
#include <stdio.h>
#include <stdbool.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
/* TODO: insert other include files here. */

/* TODO: insert other definitions and declarations here. */

typedef enum
{
    SAMPLE_RATE_8K = 0,
    SAMPLE_RATE_16K,
    SAMPLE_RATE_22K,
    SAMPLE_RATE_44K,
    SAMPLE_RATE_48K
} sample_rate_t;

static sample_rate_t current_sample_rate = SAMPLE_RATE_8K;

static volatile bool is_conversion_running = false; // flag para la IRQ del ADC
static volatile bool adc_print_flag = false; // flag para imprimir estado del ADC

/**
 * 8 kHz  → R
 * 16 kHz → G
 * 22 kHz → B
 * 44 kHz → R + G
 * 48 kHz → R + B
 */
void SetLedForSampleRate(sample_rate_t rate)
{
    switch (rate)
    {
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

void ApplySampleRate(sample_rate_t rate)
{
    SetLedForSampleRate(rate);
//    Timer_SetSampleRate(rate);
}

void NextSampleRate(void)
{
    if (current_sample_rate == SAMPLE_RATE_48K)
    {
        current_sample_rate = SAMPLE_RATE_8K;
    }
    else
    {
        current_sample_rate++;
    }

    ApplySampleRate(current_sample_rate);
}

/*
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
    /* Force the counter to be placed into memory. */
    volatile static int i = 0 ;
    /* Enter an infinite loop, just incrementing a counter. */
    while(1) {
    	if(adc_print_flag){
    		if (is_conversion_running) PRINTF("[RUN]: adquisición activada\r\n");
    		else PRINTF("[STOP]: adquisición desactivada\r\n");
    		adc_print_flag = false;
    	}
        i++ ;
        /* 'Dummy' NOP to allow source level single stepping of
            tight while() loop */
        __asm volatile ("nop");
    }
    return 0 ;
}

/* GPIO00_IRQn interrupt handler */
void GPIO0_INT_0_IRQHANDLER(void) {
	/* Get pin flags 0 */
	uint32_t pin_flags0 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0U);

	/* Place your interrupt code here */
	NextSampleRate();

	/* Clear pin flags 0 */
	GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags0, 0U);

	/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
	 Store immediate overlapping exception return operation might vector to incorrect interrupt. */
	#if defined __CORTEX_M && (__CORTEX_M == 4U)
	__DSB();
	#endif
}

/* GPIO01_IRQn interrupt handler */
void GPIO0_INT_1_IRQHANDLER(void) {
	/* Get pin flags 1 */
	uint32_t pin_flags1 = GPIO_GpioGetInterruptChannelFlags(GPIO0, 1U);

	/* Place your interrupt code here */
	is_conversion_running = !is_conversion_running;
	adc_print_flag = true;

	/* Clear pin flags 1 */
	GPIO_GpioClearInterruptChannelFlags(GPIO0, pin_flags1, 1U);

	/* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F
	 Store immediate overlapping exception return operation might vector to incorrect interrupt. */
	#if defined __CORTEX_M && (__CORTEX_M == 4U)
	__DSB();
	#endif
}

