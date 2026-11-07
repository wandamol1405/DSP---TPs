/*
 * display_port.c
 *
 * Implementación del backend de visualización (U8g2 sobre LPI2C3 / LP_FLEXCOMM3).
 * Los callbacks u8x8_* provienen del ejemplo MCXN947_U8g2_OLED sin cambios funcionales.
 */

#include "display_port.h"
#include "peripherals.h"       // LP_FLEXCOMM3_PERIPHERAL (LPI2C3) generado por el Config Tools
#include "fsl_lpi2c.h"         // LPI2C_MasterStart / Send / Stop
#include "fsl_debug_console.h" // macro PRINTF
#include <stdio.h>             // declara printf(), a la que se mapea PRINTF en esta configuración

static volatile status_t s_last_lpi2c_status = kStatus_Success; // último error del bus (solo contexto principal)

status_t display_port_get_last_error(void) {
    return s_last_lpi2c_status;
}

// Callback de bytes de U8x8: traduce los mensajes de transferencia a llamadas del driver LPI2C.
uint8_t u8x8_byte_lpi2c_mcxn947(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    uint8_t slave_address;
    status_t status = kStatus_Success;

    switch (msg) {
    case U8X8_MSG_BYTE_SEND:
        // Envía arg_int bytes por el bus
        if (arg_int > 0 && arg_ptr != NULL) {
            status = LPI2C_MasterSend(LP_FLEXCOMM3_PERIPHERAL, (uint8_t *)arg_ptr, (size_t)arg_int);
            if (status != kStatus_Success) {
                s_last_lpi2c_status = status;
                // Ante NACK o error de bus se libera el bus
                (void)LPI2C_MasterStop(LP_FLEXCOMM3_PERIPHERAL);
                return 0U;
            }
        }
        break;

    case U8X8_MSG_BYTE_INIT:
        // Pines, reloj y LPI2C3 ya fueron inicializados en el arranque (BOARD_Init*); solo se limpia el estado.
        s_last_lpi2c_status = kStatus_Success;
        break;

    case U8X8_MSG_BYTE_SET_DC:
        // En I2C el SSD1306 distingue comando/dato con el byte de control (0x00 / 0x40), no con un GPIO
        break;

    case U8X8_MSG_BYTE_START_TRANSFER:
        // U8x8 guarda la dirección de 8 bits (0x78 por defecto); LPI2C_MasterStart espera la de 7 bits
        slave_address = (uint8_t)(u8x8_GetI2CAddress(u8x8) >> 1);
        status = LPI2C_MasterStart(LP_FLEXCOMM3_PERIPHERAL, slave_address, kLPI2C_Write);
        if (status != kStatus_Success) {
            s_last_lpi2c_status = status;
            return 0U;
        }
        break;

    case U8X8_MSG_BYTE_END_TRANSFER:
        status = LPI2C_MasterStop(LP_FLEXCOMM3_PERIPHERAL);
        if (status != kStatus_Success) {
            s_last_lpi2c_status = status;
            return 0U;
        }
        break;

    default:
        return 0U;
    }

    return 1U;
}

// Callback de demoras de U8x8 usando las rutinas de retardo del SDK.
uint8_t u8x8_gpio_and_delay_mcxn947(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    (void)arg_ptr;

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        // Los pines se configuran en BOARD_InitBootPins()
        break;

    case U8X8_MSG_DELAY_NANO:
        __NOP();
        break;

    case U8X8_MSG_DELAY_10MICRO:
        SDK_DelayAtLeastUs(10UL, SystemCoreClock);
        break;

    case U8X8_MSG_DELAY_100NANO:
        SDK_DelayAtLeastUs(1UL, SystemCoreClock);
        break;

    case U8X8_MSG_DELAY_MILLI:
        SDK_DelayAtLeastUs((uint32_t)arg_int * 1000UL, SystemCoreClock);
        break;

    case U8X8_MSG_DELAY_I2C:
        // arg_int vale 1 (100 kHz, ~5 us) o 4 (400 kHz, ~1.25 us)
        SDK_DelayAtLeastUs((arg_int <= 2U ? 5UL : 2UL), SystemCoreClock);
        break;

    default:
        u8x8_SetGPIOResult(u8x8, 1U);
        break;
    }

    return 1U;
}

bool display_port_init(u8g2_t *u8g2) {
    if (u8g2 == NULL) {
        return false;
    }

    // SSD1306 128x64 por I2C en modo full buffer (1024 B de RAM): necesario para poder
    // transmitir solo regiones parciales con u8g2_UpdateDisplayArea()
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(u8g2, U8G2_R0, u8x8_byte_lpi2c_mcxn947, u8x8_gpio_and_delay_mcxn947);

    // Dirección de 8 bits (0x3C << 1 = 0x78)
    u8x8_SetI2CAddress(&u8g2->u8x8, (uint8_t)(DISPLAY_I2C_ADDR_7BIT << 1));

    // Envía la secuencia de configuración del controlador
    u8g2_InitDisplay(u8g2);

    // Saca al display del modo de ahorro de energía
    u8g2_SetPowerSave(u8g2, 0U);

    if (s_last_lpi2c_status != kStatus_Success) {
        PRINTF("[DISPLAY] Aviso: LPI2C reporto estado %d durante la inicializacion del display.\r\n",
               (int)s_last_lpi2c_status);
        return false;
    }

    return true;
}

