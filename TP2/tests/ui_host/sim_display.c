/*
 * sim_display.c (solo test en PC)
 *
 * Implementa display_port_init() / display_port_get_last_error() del backend usando un
 * simulador de SSD1306 en lugar del LPI2C. Interpreta el tráfico que genera U8g2:
 *   transacción = [control, bytes...]; control 0x00 -> comando, 0x40 -> datos de GDDRAM.
 * Comandos interpretados: 0x00-0x0F (columna baja), 0x10-0x1F (columna alta), 0xB0-0xB7 (página).
 */
#include <assert.h>
#include <string.h>
#include "display_port.h"
#include "sim_display.h"

uint8_t sim_gddram[8][128];
uint8_t sim_touched[8][16];
sim_stats_t sim_stats;
bool sim_fail_init = false;

static unsigned s_page, s_col_lo, s_col_hi, s_col;

void sim_reset_stats(void) {
    memset(&sim_stats, 0, sizeof(sim_stats));
    memset(sim_touched, 0, sizeof(sim_touched));
}

bool sim_pixel(int x, int y) {
    return (sim_gddram[y / 8][x] >> (y % 8)) & 1u;
}

static void process_transfer(const uint8_t *b, unsigned n) {
    if (n == 0) {
        return;
    }
    sim_stats.transfers++;
    if (b[0] == 0x00) { // comando (+ argumentos que se ignoran)
        if (n < 2) {
            return;
        }
        // U8g2 envía columna y página como bytes consecutivos de una misma transacción de comando.
        // Los argumentos de comandos de init también pasan por acá, pero columna/página siempre se
        // fijan justo antes de cada escritura de datos, así que no afectan.
        for (unsigned i = 1; i < n; i++) {
            uint8_t cmd = b[i];
            if ((cmd & 0xF0u) == 0xB0u) {
                s_page = cmd & 0x07u;
            } else if ((cmd & 0xF0u) == 0x10u) {
                s_col_hi = cmd & 0x0Fu;
            } else if ((cmd & 0xF0u) == 0x00u) {
                s_col_lo = cmd & 0x0Fu;
            }
            s_col = (s_col_hi << 4) | s_col_lo;
        }
    } else if (b[0] == 0x40) { // datos
        sim_stats.data_transfers++;
        for (unsigned i = 1; i < n; i++) {
            assert(s_col < 128u);
            sim_gddram[s_page][s_col] = b[i];
            sim_touched[s_page][s_col / 8u] = 1;
            sim_stats.data_bytes++;
            s_col++;
        }
    } else {
        assert(!"byte de control I2C inesperado");
    }
}

static uint8_t sim_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static uint8_t buf[256];
    static unsigned n;
    (void)u8x8;
    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_SET_DC:
        break;
    case U8X8_MSG_BYTE_START_TRANSFER:
        n = 0;
        break;
    case U8X8_MSG_BYTE_SEND:
        assert(n + arg_int <= sizeof(buf));
        memcpy(&buf[n], arg_ptr, arg_int);
        n += arg_int;
        break;
    case U8X8_MSG_BYTE_END_TRANSFER:
        process_transfer(buf, n);
        break;
    default:
        return 0;
    }
    return 1;
}

static uint8_t sim_gpio_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    (void)arg_int;
    (void)arg_ptr;
    if (msg != U8X8_MSG_GPIO_AND_DELAY_INIT && msg != U8X8_MSG_DELAY_NANO && msg != U8X8_MSG_DELAY_10MICRO &&
        msg != U8X8_MSG_DELAY_100NANO && msg != U8X8_MSG_DELAY_MILLI && msg != U8X8_MSG_DELAY_I2C) {
        u8x8_SetGPIOResult(u8x8, 1);
    }
    return 1;
}

bool display_port_init(u8g2_t *u8g2) {
    if (sim_fail_init) {
        return false;
    }
    memset(sim_gddram, 0xA5, sizeof(sim_gddram)); // la GDDRAM real arranca con basura
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(u8g2, U8G2_R0, sim_byte_cb, sim_gpio_cb); // mismo setup que el backend real
    u8x8_SetI2CAddress(&u8g2->u8x8, (uint8_t)(DISPLAY_I2C_ADDR_7BIT << 1));
    u8g2_InitDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0U);
    return true;
}

status_t display_port_get_last_error(void) {
    return kStatus_Success;
}

