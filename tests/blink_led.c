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
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
/* TODO: insert other include files here. */

/* TODO: insert other definitions and declarations here. */

/*
 * @brief   Callback function for CTIMER0 interrupt.
 * Blink the LED on the board every time the interrupt is called.
 */

void CTIMER0_Callback(uint32_t flags){
	static uint8_t i = 0;
	i++;
	if(i%2){
		GPIO_PinWrite(BOARD_INITLEDSPINS_LED_BLUE_GPIO, BOARD_INITLEDSPINS_LED_BLUE_PIN, 0);
	}else {
		GPIO_PinWrite(BOARD_INITLEDSPINS_LED_BLUE_GPIO, BOARD_INITLEDSPINS_LED_BLUE_PIN, 1);
	}
}

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
    volatile static int i = 0 ;
    /* Enter an infinite loop, just incrementing a counter. */
    while(1) {
        i++ ;
        /* 'Dummy' NOP to allow source level single stepping of
            tight while() loop */
        __asm volatile ("nop");
    }
    return 0 ;
}
