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

static bool s_streaming_enabled = false;      // true si el modo streaming de texto ('p') está activo
static bool s_binary_streaming_enabled = false; // true si el modo streaming binario ('c') está activo
static bool s_window_plot_enabled = false;      // EJEMPLO: ventana completa en texto para el plotter ('v')
static bool s_single_frame_requested = false;   // pedido de una unica ventana ('s'), se consume solo
static uint32_t s_frames_sent = 0;              // tramas binarias enviadas con exito desde el boot
static uint32_t s_frames_dropped = 0;           // tramas binarias descartadas (UART ocupada) desde el boot
static uart_stream_mode_t s_stream_mode = UART_STREAM_MODE_PLOTTER;
static uint32_t s_decimation = 32;            // enviar 1 de cada N muestras en el streaming
static uint32_t s_decimation_counter = 0;     // contador de muestras recibidas desde el último envío

static volatile bool s_sample_ready = false;  // true cuando hay un par entrada/salida pendiente de imprimir
static volatile q15_t s_stream_in = 0;        // última muestra de entrada retenida para streaming
static volatile q15_t s_stream_out = 0;       // última muestra de salida retenida para streaming

// ---------------------------------------------------------------------------
// Trama binaria (uart_stage_send_frame) -- ver TP3/README.md, "Formato de
// Trama UART". Protocolo minimo: sync + type + N + Fs + payload, nada mas
// (sin CRC/seq/peak_bin -- la consigna real solo pide transmitir el buffer
// y graficarlo, ver discusion en la PR/issue).
//
// Se arma a mano byte a byte (no con un struct + memcpy): un struct con
// un uint32_t despues de dos campos mas chicos le mete relleno de
// alineacion en el medio, y eso rompe el formato on-wire que espera la GUI.
// ---------------------------------------------------------------------------
#define UART_FRAME_SYNC0 0xA5U
#define UART_FRAME_SYNC1 0x5AU
// sync(2) + type(1) + n(2) + fs(4)
#define UART_FRAME_HEADER_SIZE 9U
// N mas grande de la consigna (512/1024/2048); dimensiona el buffer estatico
#define UART_FRAME_MAX_N 2048U
#define UART_FRAME_MAX_SIZE (UART_FRAME_HEADER_SIZE + 2U * UART_FRAME_MAX_N)

static lpuart_handle_t s_tx_handle;
static volatile bool s_tx_busy = false;
// true mientras el loop principal escribe texto (PRINTF bloqueante) por la misma UART: en
// ese lapso no se arranca ninguna trama binaria, para que no se mezclen los bytes.
static volatile bool s_text_busy = false;
static uint8_t s_tx_buf[UART_FRAME_MAX_SIZE];

// Se dispara por interrupcion (LP_FLEXCOMM4, ya enganchada automaticamente
// por LPUART_TransferCreateHandle) cuando termina de salir el ultimo byte.
static void uart_stage_tx_done(LPUART_Type *base, lpuart_handle_t *handle, status_t status, void *userData) {
    (void)base;
    (void)handle;
    (void)userData;
    if (status == kStatus_LPUART_TxIdle) {
        s_tx_busy = false;
    }
}

// Escribe `value` little-endian en buf[*offset] y avanza *offset -- evita
// depender del endianness/empaquetado que elija el compilador para un struct.
static void uart_frame_put_u16(uint8_t *buf, uint32_t *offset, uint16_t value) {
    buf[*offset]     = (uint8_t)(value & 0xFFU);
    buf[*offset + 1] = (uint8_t)((value >> 8) & 0xFFU);
    *offset += 2U;
}

static void uart_frame_put_u32(uint8_t *buf, uint32_t *offset, uint32_t value) {
    buf[*offset]     = (uint8_t)(value & 0xFFU);
    buf[*offset + 1] = (uint8_t)((value >> 8) & 0xFFU);
    buf[*offset + 2] = (uint8_t)((value >> 16) & 0xFFU);
    buf[*offset + 3] = (uint8_t)((value >> 24) & 0xFFU);
    *offset += 4U;
}

// Deja la etapa UART en su estado inicial: streaming desactivado y decimación por defecto.
void uart_stage_init(void) {
    s_streaming_enabled = false;
    s_binary_streaming_enabled = false;
    s_single_frame_requested = false;
    s_frames_sent = 0;
    s_frames_dropped = 0;
    s_stream_mode = UART_STREAM_MODE_PLOTTER;
    s_decimation = 32;
    s_decimation_counter = 0;
    s_sample_ready = false;
    s_tx_busy = false;

    // Registra el callback de "fin de transmision" para el modo no bloqueante
    // de uart_stage_send_frame(). LPUART4 ya quedo inicializado (baudrate,
    // 8N1) por BOARD_InitDebugConsole() -- esto solo engancha la interrupcion
    // de TX (LP_FLEXCOMM4, compartida con RX/I2C/SPI del mismo flexcomm) y
    // la habilita en el NVIC; no reconfigura el periferico.
    //
    // TODO(TP3): primera version por interrupcion a proposito (mas facil de
    // probar). Una vez validado en la placa con una ventana real, evaluar
    // pasar a EDMA si hace falta mas margen de CPU: lpuart_handle_t ->
    // lpuart_edma_handle_t, LPUART_TransferCreateHandle -> LPUART_TransferCreateHandleEDMA
    // (necesita ademas un edma_handle_t + EDMA_CreateHandle para el canal),
    // LPUART_TransferSendNonBlocking -> LPUART_SendEDMA. uart_stage_send_frame()
    // y todo lo que depende de s_tx_busy/uart_stage_tx_busy() no cambia.
    LPUART_TransferCreateHandle((LPUART_Type *)BOARD_DEBUG_UART_BASEADDR, &s_tx_handle, uart_stage_tx_done, NULL);
}

bool uart_stage_tx_busy(void) {
    return s_tx_busy;
}

bool uart_stage_can_send_frame(void) {
    return !s_tx_busy && !s_text_busy;
}

// Primero se toma el turno y después se mira si hay una trama saliendo: así, si PendSV
// interrumpe entre las dos líneas, ya ve s_text_busy y no arranca una trama nueva.
bool uart_stage_try_begin_text(void) {
    s_text_busy = true;
    if (s_tx_busy) {
        s_text_busy = false;
        return false;
    }
    return true;
}

void uart_stage_end_text(void) {
    s_text_busy = false;
}

// Activa o desactiva el streaming binario ('c'). Mutuamente excluyente con
// el de texto ('p'): si se prende uno, se apaga el otro aca mismo, no
// queda en manos del que llama acordarse de hacerlo.
void uart_stage_enable_binary_streaming(bool enable) {
    s_binary_streaming_enabled = enable;
    if (enable) {
        s_streaming_enabled = false;
        s_window_plot_enabled = false;
    }
}

bool uart_stage_is_binary_streaming_enabled(void) {
    return s_binary_streaming_enabled;
}

bool uart_stage_any_streaming_active(void) {
    return s_streaming_enabled || s_binary_streaming_enabled || s_window_plot_enabled;
}

// EJEMPLO: modo plotter por ventanas ('v'). Excluyente con 'p' y 'c', igual que esos dos
// entre sí.
void uart_stage_enable_window_plot(bool enable) {
    s_window_plot_enabled = enable;
    if (enable) {
        s_streaming_enabled = false;
        s_binary_streaming_enabled = false;
    }
}

bool uart_stage_is_window_plot_enabled(void) {
    return s_window_plot_enabled;
}

// EJEMPLO: imprime los len valores de una ventana, uno por línea, en el formato elegido
// con 'o'. Bloqueante: llamar solo desde el loop principal.
void uart_stage_plot_window(const int16_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (s_stream_mode == UART_STREAM_MODE_OSCILLOSCOPE) {
            PRINTF("%d,0,0\r\n", (int)data[i]);
        } else {
            // Mismo formato entrada,salida que 'p', para que lo lea la GUI del TP1
            PRINTF("%d,%d\r\n", (int)data[i], (int)data[i]);
        }
    }
}

void uart_stage_request_single_frame(void) {
    s_single_frame_requested = true;
}

bool uart_stage_consume_single_frame_request(void) {
    bool requested = s_single_frame_requested;
    s_single_frame_requested = false;
    return requested;
}

uint32_t uart_stage_get_frames_sent(void) {
    return s_frames_sent;
}

uint32_t uart_stage_get_frames_dropped(void) {
    return s_frames_dropped;
}

void uart_stage_send_frame(uart_frame_type_t type, uint16_t n, uint32_t fs_hz, const int16_t *payload) {
    if (payload == NULL || n == 0U || n > UART_FRAME_MAX_N) {
        return;  // argumentos invalidos: no cuenta como descarte real, es un error de uso
    }
    if (s_tx_busy || s_text_busy) {
        s_frames_dropped++;
        return;
    }

    uint16_t length = (type == UART_FRAME_TYPE_FFT) ? (n / 2U) : n;
    uint32_t offset = 0U;

    s_tx_buf[offset++] = UART_FRAME_SYNC0;
    s_tx_buf[offset++] = UART_FRAME_SYNC1;
    s_tx_buf[offset++] = (uint8_t)type;
    uart_frame_put_u16(s_tx_buf, &offset, n);
    uart_frame_put_u32(s_tx_buf, &offset, fs_hz);
    for (uint16_t i = 0; i < length; i++) {
        uart_frame_put_u16(s_tx_buf, &offset, (uint16_t)payload[i]);
    }

    // IMPORTANTE: marcar busy ANTES de pedir el envio. El callback de fin de
    // transmision puede llegar a correr (en otra interrupcion, ya con la
    // trama completa afuera) antes de que esta funcion termine su propio
    // return -- si busy se pusiera despues, una trama muy corta/rapida
    // podria "despejarse" antes de que el llamador llegue a verla.
    s_tx_busy = true;
    lpuart_transfer_t xfer = {
        .data = s_tx_buf,
        .dataSize = offset,
    };
    status_t status = LPUART_TransferSendNonBlocking((LPUART_Type *)BOARD_DEBUG_UART_BASEADDR, &s_tx_handle, &xfer);
    if (status != kStatus_Success) {
        s_tx_busy = false;  // no se pudo encolar (ver status_t): no dejar trabado el flag
        s_frames_dropped++;
        return;
    }
    s_frames_sent++;
}

// Imprime una ventana de salida (FFT o Bypass) como texto plano, una
// muestra por línea -- para mirar con minicom sin la GUI. Ver comentario
// largo en el .h: es bloqueante a propósito, el llamador decide cuándo
// conviene invocarla (nunca desde PendSV).
void uart_stage_print_samples(uart_frame_type_t type, uint16_t n, uint32_t fs_hz, const int16_t *payload) {
    if (payload == NULL || n == 0U || n > UART_FRAME_MAX_N) {
        return;
    }

    uint16_t length = (type == UART_FRAME_TYPE_FFT) ? (n / 2U) : n;
    const char *type_name = (type == UART_FRAME_TYPE_FFT) ? "FFT" : "BYPASS";

    PRINTF("\r\n--- %s  N=%u  Fs=%u Hz ---\r\n", type_name, (unsigned int)n, (unsigned int)fs_hz);

    for (uint16_t i = 0; i < length; i++) {
        if (type == UART_FRAME_TYPE_FFT) {
            // payload viene en dB x100 (mismo convenio que uart_stage_send_frame):
            // se separa a mano en parte entera/decimal porque PRINTF_FLOAT_ENABLE=0
            // en esta build (ver flags del proyecto) -- no hay "%f".
            int32_t db_x100 = (int32_t)payload[i];
            bool negative = db_x100 < 0;
            uint32_t abs_x100 = negative ? (uint32_t)(-db_x100) : (uint32_t)db_x100;
            PRINTF("%u: %s%u.%02u dB\r\n", (unsigned int)i, negative ? "-" : "",
                   (unsigned int)(abs_x100 / 100U), (unsigned int)(abs_x100 % 100U));
        } else {
            PRINTF("%u: %d\r\n", (unsigned int)i, (int)payload[i]);
        }
    }

    PRINTF("--- fin ---\r\n\r\n");
}

// Activa o desactiva el envío continuo de muestras (comando 'p'). Mutuamente
// excluyente con el streaming binario ('c'), ver uart_stage_enable_binary_streaming().
void uart_stage_enable_streaming(bool enable) {
    s_streaming_enabled = enable;
    if (enable) {
        s_binary_streaming_enabled = false;
        s_window_plot_enabled = false;
    }
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
    uint32_t flags = LPUART_GetStatusFlags(base);

    // EJEMPLO: si llegaron más bytes de los que entran en la FIFO de RX mientras el loop
    // principal estaba ocupado imprimiendo, el LPUART marca overrun y, mientras ese flag
    // siga puesto, no guarda ningún byte nuevo: la placa queda "sorda" a los comandos.
    // Leyendo por polling nadie lo limpiaba, así que se limpia acá.
    if ((flags & (uint32_t)kLPUART_RxOverrunFlag) != 0U) {
        (void)LPUART_ClearStatusFlags(base, (uint32_t)kLPUART_RxOverrunFlag);
    }

    if ((flags & (uint32_t)kLPUART_RxDataRegFullFlag) != 0U) {
        *ch = (char)LPUART_ReadByte(base);
        return true;
    }
    return false;
}

