/*
 * uart_stage.h
 *
 * Etapa de comunicación UART con la PC (LPUART4, la consola de depuración de la placa).
 *
 * Hacia la PC salen dos cosas por el mismo puerto:
 *  - Tramas binarias con el buffer de salida (FFT o BYPASS): uart_stage_send_frame(), sin
 *    bloquear, desde PendSV. Es la única salida de datos. Formato en TP3/decisiones.md,
 *    "Formato de la trama".
 *  - Texto (PRINTF) con las respuestas a los comandos y los avisos, desde el loop principal.
 * Como comparten el puerto, se turnan: el texto sale solo entre dos tramas (ver
 * uart_stage_try_begin_text()).
 *
 * Desde la PC llegan comandos de una letra. Se reciben por interrupción y se guardan en un
 * buffer circular, así no se pierden aunque esté saliendo una trama o el loop principal
 * esté ocupado. uart_stage_try_getchar() los saca de ahí.
 */

#ifndef UART_STAGE_H_
#define UART_STAGE_H_

#include <stdint.h>   // tipo uint32_t
#include <stdbool.h>  // tipo bool

/**
 * @brief Tipo de trama binaria que arma uart_stage_send_frame().
 */
typedef enum {
    UART_FRAME_TYPE_FFT = 0,     /**< Magnitud en dBFS x100. Cantidad de valores = N/2 */
    UART_FRAME_TYPE_BYPASS = 1,  /**< Muestras q15. Cantidad de valores = N */
} uart_frame_type_t;

/**
 * @brief Inicializa la etapa UART: envío de tramas por interrupción y recepción de
 *        comandos por interrupción a un buffer circular.
 */
void uart_stage_init(void);

/**
 * @brief Arma y transmite (sin bloquear, por interrupción) una trama binaria con el buffer
 *        de salida (FFT o BYPASS). Si ya hay una trama saliendo, o el loop principal tiene
 *        el turno de texto, descarta la trama y la cuenta. Los datos se copian a un buffer
 *        propio antes de volver, así que el llamador puede reusar el suyo enseguida.
 * @param type    UART_FRAME_TYPE_FFT o UART_FRAME_TYPE_BYPASS.
 * @param n       Tamaño de ventana (512, 1024 o 2048).
 * @param fs_hz   Frecuencia de muestreo, en Hz.
 * @param payload Puntero a los datos: N/2 valores si type es FFT, N si es BYPASS.
 */
void uart_stage_send_frame(uart_frame_type_t type, uint16_t n, uint32_t fs_hz, const int16_t *payload);

/**
 * @brief Indica si una trama binaria podría salir ahora: no hay otra saliendo y el loop
 *        principal no tiene (ni está esperando) el turno de texto.
 */
bool uart_stage_can_send_frame(void);

/**
 * @brief Pide el turno de la UART para escribir texto (PRINTF) desde el loop principal.
 *
 *        Si devuelve true, hasta uart_stage_end_text() no arranca ninguna trama binaria.
 *
 *        Si devuelve false hay una trama saliendo y no hay que escribir nada, pero el
 *        pedido queda anotado: no arranca ninguna trama nueva y el próximo llamado
 *        devuelve true apenas termine la que está saliendo. Así el texto espera como
 *        mucho una trama, aunque el envío continuo tenga la UART siempre ocupada.
 *
 *        Llamar solo cuando hay texto pendiente, y volver a llamar hasta que devuelva
 *        true. Después de escribir, siempre uart_stage_end_text().
 * @note Solo desde el loop principal.
 */
bool uart_stage_try_begin_text(void);

/**
 * @brief Devuelve el turno tomado con uart_stage_try_begin_text().
 */
void uart_stage_end_text(void);

/**
 * @brief Activa o desactiva el envío continuo de tramas (comando 'c').
 */
void uart_stage_enable_binary_streaming(bool enable);

/**
 * @brief Consulta si el envío continuo de tramas está activo.
 */
bool uart_stage_is_binary_streaming_enabled(void);

/**
 * @brief Pide el envío de una única trama (comando 's'), sin activar el envío continuo.
 */
void uart_stage_request_single_frame(void);

/**
 * @brief Consulta si hay un pedido de trama única pendiente y lo limpia (se consume una
 *        sola vez). La llama el pipeline para decidir si manda la ventana que acaba de
 *        quedar lista.
 */
bool uart_stage_consume_single_frame_request(void);

/**
 * @brief Cantidad de tramas binarias enviadas desde el arranque.
 */
uint32_t uart_stage_get_frames_sent(void);

/**
 * @brief Cantidad de tramas binarias descartadas desde el arranque (ventana lista pero la
 *        UART estaba ocupada con la trama anterior o con texto).
 */
uint32_t uart_stage_get_frames_dropped(void);

/**
 * @brief Cantidad de bytes de comandos perdidos desde el arranque, porque se llenó el
 *        buffer circular de recepción o el del propio LPUART. Tiene que quedar en 0.
 */
uint32_t uart_stage_get_rx_lost(void);

/**
 * @brief Saca un byte del buffer circular de recepción, sin bloquear.
 * @param ch Puntero donde se guarda el carácter leído.
 * @return true si había un carácter, false si el buffer estaba vacío.
 * @note Solo desde el loop principal.
 */
bool uart_stage_try_getchar(char *ch);

#endif /* UART_STAGE_H_ */
