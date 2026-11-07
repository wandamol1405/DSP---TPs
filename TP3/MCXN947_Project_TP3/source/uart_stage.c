/*
 * uart_stage.c
 *
 * Implementación de la etapa de comunicación UART.
 */

#include "uart_stage.h"        // declaraciones propias
#include "fsl_debug_console.h" // macro PRINTF, usada para toda la salida por UART
#include "board.h"             // BOARD_DEBUG_UART_BASEADDR (instancia física de LPUART usada)
#include "fsl_lpuart.h"        // acceso de bajo nivel al LPUART (lectura directa de RX, sin pasar por PRINTF/GETCHAR)
#include <stdio.h>             // declara printf(), a la que se mapea la macro PRINTF en esta configuración (SDK_DEBUGCONSOLE=0)

static bool s_streaming_enabled = false;      // true si el modo streaming ('p') está activo
static uart_stream_mode_t s_stream_mode = UART_STREAM_MODE_PLOTTER;
static uint32_t s_decimation = 32;            // enviar 1 de cada N muestras en el streaming
static uint32_t s_decimation_counter = 0;     // contador de muestras recibidas desde el último envío

static volatile bool s_sample_ready = false;  // true cuando hay un par entrada/salida pendiente de imprimir
static volatile q15_t s_stream_in = 0;        // última muestra de entrada retenida para streaming
static volatile q15_t s_stream_out = 0;       // última muestra de salida retenida para streaming

// Deja la etapa UART en su estado inicial: streaming desactivado y decimación por defecto.
void uart_stage_init(void) {
    s_streaming_enabled = false;
    s_stream_mode = UART_STREAM_MODE_PLOTTER;
    s_decimation = 32;
    s_decimation_counter = 0;
    s_sample_ready = false;
}

// Activa o desactiva el envío continuo de muestras (comando 'p').
void uart_stage_enable_streaming(bool enable) {
    s_streaming_enabled = enable;
}

// Getter de si el streaming continuo está activo.
bool uart_stage_is_streaming_enabled(void) {
    return s_streaming_enabled;
}

// Cambia entre el formato del plotter del repositorio y Serial-Oscilloscope.
void uart_stage_set_stream_mode(uart_stream_mode_t mode) {
    if (mode <= UART_STREAM_MODE_OSCILLOSCOPE) {
        s_stream_mode = mode;
    }
}

// Getter del formato de salida actual.
uart_stream_mode_t uart_stage_get_stream_mode(void) {
    return s_stream_mode;
}

// Cambia cada cuántas muestras se envía un dato por streaming (ignora valores no positivos).
void uart_stage_set_decimation(uint32_t decimation) {
    if (decimation > 0) {
        s_decimation = decimation;
    }
}

// Recibe un par entrada/salida del pipeline; si el streaming está activo, cuenta hasta
// completar el factor de decimación y entonces deja el par listo para que uart_stage_task()
// lo imprima (evita bloquear el pipeline con un PRINTF en cada muestra).
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

// Tarea periódica para el loop principal: si hay una muestra pendiente del streaming,
// la imprime (esto es lo que efectivamente saca los datos por el puerto, fuera de la ISR).
void uart_stage_task(void) {
    if (s_streaming_enabled && s_sample_ready) {
        s_sample_ready = false;
        if (s_stream_mode == UART_STREAM_MODE_OSCILLOSCOPE) {
            // Serial-Oscilloscope requiere tres valores separados por dos comas.
            PRINTF("%d,0,0\r\n", (int)s_stream_out);
        } else {
            // Formato usado por la GUI del repositorio: entrada,salida.
            PRINTF("%d,%d\r\n", (int)s_stream_in, (int)s_stream_out);
        }
    }
}

// Imprime todas las muestras del buffer circular por UART, entre marcadores de
// inicio/fin, en CSV (16 valores por línea) o una por línea con índice.
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

// Lee un byte recibido por UART sin bloquear, accediendo directamente al registro del
// LPUART (no usa GETCHAR/DbgConsole, así que funciona sin depender de esa configuración).
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

