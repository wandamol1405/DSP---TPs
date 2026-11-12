/*
 * uart_stage.c
 *
 * Implementación de la etapa de comunicación UART.
 */

#include "uart_stage.h"        // declaraciones propias
#include "board.h"             // BOARD_DEBUG_UART_BASEADDR (instancia física de LPUART usada)
#include "fsl_lpuart.h"        // driver del LPUART: envío de tramas y recepción de comandos por interrupción
#include <stddef.h>            // NULL, size_t

static bool s_binary_streaming_enabled = false; // true si el envío continuo ('c') está activo
static bool s_single_frame_requested = false;   // pedido de una única trama ('s'), se consume solo
static uint32_t s_frames_sent = 0;              // tramas binarias enviadas desde el arranque
static uint32_t s_frames_dropped = 0;           // tramas binarias descartadas (UART ocupada) desde el arranque
static volatile uint32_t s_rx_lost = 0;         // bytes de comandos perdidos desde el arranque

// ---------------------------------------------------------------------------
// Trama binaria (uart_stage_send_frame) -- ver TP3/decisiones.md, "Formato de
// la trama". Protocolo minimo: sync + type + N + Fs + payload, nada mas.
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

// Un solo handle del driver para las dos direcciones: envío de tramas y recepción de
// comandos. Los dos usan la misma interrupción (LP_FLEXCOMM4).
static lpuart_handle_t s_uart_handle;
static volatile bool s_tx_busy = false;
// true mientras el loop principal tiene el turno de texto (PRINTF bloqueante) o lo está
// esperando: en ese lapso no se arranca ninguna trama binaria, para que no se mezclen los
// bytes y para que el texto no espere más de una trama.
static volatile bool s_text_busy = false;
static uint8_t s_tx_buf[UART_FRAME_MAX_SIZE];

// Buffer circular de recepción: lo llena la interrupción del LPUART y lo vacía el loop
// principal con uart_stage_try_getchar(). Los comandos son de una letra, así que 64 bytes
// alcanzan para todo lo que pueda llegar mientras el loop principal imprime la ayuda.
#define UART_RX_RING_SIZE 64U
static uint8_t s_rx_ring[UART_RX_RING_SIZE];

// Lo llama el driver desde la interrupción del LPUART (LP_FLEXCOMM4): cuando termina de
// salir el último byte de una trama, y cuando se pierde un byte recibido.
static void uart_stage_event(LPUART_Type *base, lpuart_handle_t *handle, status_t status, void *userData) {
    (void)base;
    (void)handle;
    (void)userData;
    if (status == kStatus_LPUART_TxIdle) {
        s_tx_busy = false;
    } else if ((status == kStatus_LPUART_RxRingBufferOverrun) || (status == kStatus_LPUART_RxHardwareOverrun)) {
        s_rx_lost++;
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

// Deja la etapa UART en su estado inicial y engancha las interrupciones del LPUART.
void uart_stage_init(void) {
    s_binary_streaming_enabled = false;
    s_single_frame_requested = false;
    s_frames_sent = 0;
    s_frames_dropped = 0;
    s_rx_lost = 0;
    s_tx_busy = false;
    s_text_busy = false;

    LPUART_Type *base = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;

    // LPUART4 ya quedó inicializado (velocidad, 8N1) por BOARD_InitDebugConsole(). Esto
    // solo registra el handle del driver, que engancha la interrupción LP_FLEXCOMM4 y la
    // habilita en el NVIC; no reconfigura el periférico.
    LPUART_TransferCreateHandle(base, &s_uart_handle, uart_stage_event, NULL);

    // Recepción por interrupción: desde acá cada byte que llega lo guarda el driver en
    // s_rx_ring, sin depender de que el loop principal lo lea a tiempo. El driver también
    // limpia la marca de desborde del LPUART, que si queda puesta frena la recepción.
    LPUART_TransferStartRingBuffer(base, &s_uart_handle, s_rx_ring, sizeof(s_rx_ring));
}

bool uart_stage_can_send_frame(void) {
    return !s_tx_busy && !s_text_busy;
}

// Primero se toma el turno y después se mira si hay una trama saliendo: así, si PendSV
// interrumpe entre las dos líneas, ya ve s_text_busy y no arranca una trama nueva.
// Si hay una trama saliendo el turno NO se suelta: queda reservado para que no arranque
// otra trama detrás, y el próximo llamado lo consigue apenas termine la actual.
bool uart_stage_try_begin_text(void) {
    s_text_busy = true;
    return !s_tx_busy;
}

void uart_stage_end_text(void) {
    s_text_busy = false;
}

// Activa o desactiva el envío continuo de tramas ('c').
void uart_stage_enable_binary_streaming(bool enable) {
    s_binary_streaming_enabled = enable;
}

bool uart_stage_is_binary_streaming_enabled(void) {
    return s_binary_streaming_enabled;
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

uint32_t uart_stage_get_rx_lost(void) {
    return s_rx_lost;
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
    status_t status = LPUART_TransferSendNonBlocking((LPUART_Type *)BOARD_DEBUG_UART_BASEADDR, &s_uart_handle, &xfer);
    if (status != kStatus_Success) {
        s_tx_busy = false;  // no se pudo encolar (ver status_t): no dejar trabado el flag
        s_frames_dropped++;
        return;
    }
    s_frames_sent++;
}

// Saca un byte del buffer circular de recepción. LPUART_TransferReceiveNonBlocking() con
// el buffer circular activo copia lo que ya llegó; se la llama solo si hay al menos un
// byte, así nunca deja una recepción pendiente.
bool uart_stage_try_getchar(char *ch) {
    if (ch == NULL) {
        return false;
    }
    LPUART_Type *base = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;
    if (LPUART_TransferGetRxRingBufferLength(base, &s_uart_handle) == 0U) {
        return false;
    }

    uint8_t byte = 0U;
    size_t received = 0U;
    lpuart_transfer_t xfer = {
        .rxData = &byte,
        .dataSize = 1U,
    };
    if ((LPUART_TransferReceiveNonBlocking(base, &s_uart_handle, &xfer, &received) != kStatus_Success) ||
        (received != 1U)) {
        return false;
    }
    *ch = (char)byte;
    return true;
}
