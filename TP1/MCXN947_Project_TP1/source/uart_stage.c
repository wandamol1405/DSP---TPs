/*
 * uart_stage.c
 *
 * Implementación de la etapa de comunicación UART.
 */

#include "uart_stage.h"
#include "fsl_debug_console.h"
#include "board.h"
#include "fsl_lpuart.h"
#include <stdio.h>

static bool s_streaming_enabled = false;
static uint32_t s_decimation = 32;
static uint32_t s_decimation_counter = 0;

static volatile bool s_sample_ready = false;
static volatile q15_t s_stream_in = 0;
static volatile q15_t s_stream_out = 0;

void uart_stage_init(void) {
    s_streaming_enabled = false;
    s_decimation = 32;
    s_decimation_counter = 0;
    s_sample_ready = false;
}

void uart_stage_enable_streaming(bool enable) {
    s_streaming_enabled = enable;
}

bool uart_stage_is_streaming_enabled(void) {
    return s_streaming_enabled;
}

void uart_stage_set_decimation(uint32_t decimation) {
    if (decimation > 0) {
        s_decimation = decimation;
    }
}

void uart_stage_feed_sample(q15_t in_sample, q15_t out_sample) {
    if (!s_streaming_enabled) {
        return;
    }

    s_decimation_counter++;
    if (s_decimation_counter >= s_decimation) {
        s_decimation_counter = 0;
        s_stream_in = in_sample;
        s_stream_out = out_sample;
        s_sample_ready = true;
    }
}

void uart_stage_task(void) {
    if (s_streaming_enabled && s_sample_ready) {
        s_sample_ready = false;
        // Formato para Serial Plotter: Entrada,Salida
        PRINTF("%d,%d\r\n", (int)s_stream_in, (int)s_stream_out);
    }
}

void uart_stage_dump_buffer(circular_buffer_t *cb, bool format_csv) {
    if (cb == NULL || cb->buffer == NULL) {
        PRINTF("[UART] Buffer nulo\r\n");
        return;
    }

    PRINTF("\r\n--- INICIO BUFFER DUMP (%u muestras) ---\r\n", (unsigned int)cb->capacity);

    if (format_csv) {
        for (uint32_t i = 0; i < cb->capacity; i++) {
            PRINTF("%d", (int)cb->buffer[i]);
            if (i < cb->capacity - 1) {
                PRINTF(",");
            }
            // Retorno de carro cada 16 valores para facilitar lectura
            if ((i + 1) % 16 == 0) {
                PRINTF("\r\n");
            }
        }
        PRINTF("\r\n");
    } else {
        for (uint32_t i = 0; i < cb->capacity; i++) {
            PRINTF("[%u]: %d\r\n", (unsigned int)i, (int)cb->buffer[i]);
        }
    }

    PRINTF("--- FIN BUFFER DUMP ---\r\n\r\n");
}

bool uart_stage_try_getchar(char *ch) {
    if (ch == NULL) {
        return false;
    }
    LPUART_Type *base = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;
    if ((LPUART_GetStatusFlags(base) & (uint32_t)kLPUART_RxDataRegFullFlag) != 0U) {
        *ch = (char)LPUART_ReadByte(base);
        return true;
    }
    return false;
}

