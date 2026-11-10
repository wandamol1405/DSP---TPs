/*
 * uart_stage.h
 *
 * Etapa de comunicación UART: transmisión de datos adquiridos y procesados hacia la PC.
 *
 * Dos caminos de salida para la ventana de FFT/Bypass (ver TP3/README.md,
 * "Formato de Trama UART"):
 *  - Binario ('c', uart_stage_send_frame()): no bloqueante, pensado para
 *    que lo decodifique la GUI (TP3/tools/fft_gui.py, en la rama de GUI).
 *  - Texto ('p', uart_stage_print_samples()): bloqueante, un valor por
 *    línea, para mirarlo con un terminal común (minicom) sin la GUI.
 * Mutuamente excluyentes -- no tiene que haber un envío binario en curso
 * mientras se imprime texto (comparten el mismo LPUART4, un PRINTF en el
 * medio de una transferencia no bloqueante podría entreverar bytes).
 *
 * 'p' todavía no quedó re-enganchado a uart_stage_print_samples(): sigue
 * con el streaming muestra-a-muestra heredado de TP2
 * (uart_stage_feed_sample()/uart_stage_task()) porque todavía no existe
 * una ventana de salida FFT/Bypass real del pipeline (issues #14/#15,
 * en curso en paralelo). Cuando esa salida exista, 'p' pasa a llamar a
 * uart_stage_print_samples() y el camino viejo se puede sacar.
 *
 * El volcado de buffer ('d', uart_stage_dump_buffer()) queda igual que en
 * TP2 por ahora (vuelca el buffer circular crudo, no la salida FFT/Bypass).
 */

#ifndef UART_STAGE_H_
#define UART_STAGE_H_

#include <stdint.h>   // tipo uint32_t
#include <stdbool.h>  // tipo bool
#include "arm_math.h" // tipo q15_t (CMSIS-DSP)
#include "circular_buffer.h" // circular_buffer_t, para el volcado por UART

/**
 * @brief Formato de las muestras enviadas durante el streaming.
 */
typedef enum {
	UART_STREAM_MODE_PLOTTER = 0,      /**< Entrada y salida: entrada,salida */
	UART_STREAM_MODE_OSCILLOSCOPE      /**< Tres canales: salida,0,0 */
} uart_stream_mode_t;

/**
 * @brief Tipo de trama binaria que arma uart_stage_send_frame() (ver
 *        TP3/README.md, "Formato de Trama UART" -- protocolo minimo: solo
 *        lo que pide la consigna, transmitir el buffer de salida y poder
 *        graficarlo en la PC).
 */
typedef enum {
    UART_FRAME_TYPE_FFT = 0,     /**< Magnitud en dBFS x100. Length = N/2 */
    UART_FRAME_TYPE_BYPASS = 1,  /**< Muestras q15. Length = N */
} uart_frame_type_t;

/**
 * @brief Inicializa la etapa UART.
 */
void uart_stage_init(void);

/**
 * @brief Arma y transmite (no bloqueante, por interrupcion) una trama
 *        binaria con el buffer de salida (FFT o Bypass). Si ya hay un envio
 *        en curso, o el loop principal esta escribiendo texto, descarta la
 *        trama y la cuenta (ver uart_stage_can_send_frame()). El payload se
 *        copia a un buffer propio antes de volver, asi que el llamador puede
 *        reusar el suyo enseguida.
 * @param type    UART_FRAME_TYPE_FFT o UART_FRAME_TYPE_BYPASS.
 * @param n       Tamano de ventana (512, 1024 o 2048).
 * @param fs_hz   Frecuencia de muestreo, en Hz.
 * @param payload Puntero a los datos: N/2 valores si type es FFT, N si es
 *                BYPASS (se deriva de type y n, no hace falta pasarlo).
 */
void uart_stage_send_frame(uart_frame_type_t type, uint16_t n, uint32_t fs_hz, const int16_t *payload);

/**
 * @brief Indica si hay una trama binaria en curso de transmision.
 */
bool uart_stage_tx_busy(void);

/**
 * @brief Indica si una trama binaria podría salir ahora: no hay otra saliendo y el loop
 *        principal no está escribiendo texto.
 */
bool uart_stage_can_send_frame(void);

/**
 * @brief Pide el turno de la UART para escribir texto (PRINTF) desde el loop principal.
 *        Si hay una trama binaria saliendo devuelve false y no hay que escribir nada; si
 *        devuelve true, hasta uart_stage_end_text() no arranca ninguna trama binaria
 *        (uart_stage_send_frame() la descarta y la cuenta). Texto y trama comparten
 *        LPUART4: si se escriben a la vez se mezclan los bytes.
 * @note Solo desde el loop principal.
 */
bool uart_stage_try_begin_text(void);

/**
 * @brief Devuelve el turno tomado con uart_stage_try_begin_text().
 */
void uart_stage_end_text(void);

/**
 * @brief Imprime por UART (bloqueante, vía PRINTF) los valores de una
 *        ventana de salida (FFT o Bypass), uno por línea, para poder
 *        mirarlos con un terminal de texto común (minicom) sin la GUI.
 *        Es el equivalente en texto de uart_stage_send_frame() -- mismos
 *        parámetros, mismo origen de datos, pero plano y legible en vez
 *        de binario.
 *
 *        Llamar SOLO desde el loop principal, nunca desde PendSV: al ser
 *        bloqueante (son hasta 2048 líneas), haría exactamente lo que esta
 *        issue busca evitar -- frenar el refresco del OLED. El llamador
 *        decide cuándo invocarla (por ejemplo, una ventana sí y varias no),
 *        no hay un "busy" que lo decida solo como en el modo binario.
 * @param type    UART_FRAME_TYPE_FFT o UART_FRAME_TYPE_BYPASS.
 * @param n       Tamaño de ventana (512, 1024 o 2048).
 * @param fs_hz   Frecuencia de muestreo, en Hz.
 * @param payload Puntero a los datos: N/2 valores si type es FFT, N si es
 *                BYPASS (se deriva de type y n, no hace falta pasarlo).
 */
void uart_stage_print_samples(uart_frame_type_t type, uint16_t n, uint32_t fs_hz, const int16_t *payload);

/**
 * @brief Activa o desactiva el streaming binario continuo (comando 'c').
 *        Mutuamente excluyente con el streaming de texto ('p'): activar
 *        uno desactiva el otro (lo hace esta misma función, no hace falta
 *        que el llamador se ocupe), no pueden competir por el mismo LPUART.
 */
void uart_stage_enable_binary_streaming(bool enable);

/**
 * @brief Consulta si el streaming binario continuo está activo.
 */
bool uart_stage_is_binary_streaming_enabled(void);

/**
 * @brief Indica si hay CUALQUIER modo de streaming activo (texto 'p' o
 *        binario 'c') -- para las notificaciones de estado que no deben
 *        contaminar ninguno de los dos. Reemplaza los usos sueltos de
 *        uart_stage_is_streaming_enabled() en los que antes solo
 *        importaba el streaming de texto.
 */
bool uart_stage_any_streaming_active(void);

/**
 * @brief Pide el envío/impresión de una única ventana de salida (comando
 *        's'), sin activar el modo continuo.
 */
void uart_stage_request_single_frame(void);

/**
 * @brief Consulta si hay un pedido de ventana única pendiente y lo limpia
 *        (se consume una sola vez). Pensada para que la llame el
 *        productor de la ventana (pipeline) junto con
 *        uart_stage_is_binary_streaming_enabled() para decidir si
 *        corresponde mandar la ventana que acaba de quedar lista.
 */
bool uart_stage_consume_single_frame_request(void);

/**
 * @brief Cantidad de tramas binarias enviadas con éxito desde el boot.
 */
uint32_t uart_stage_get_frames_sent(void);

/**
 * @brief Cantidad de tramas binarias descartadas desde el boot (ventana
 *        lista pero la UART todavía estaba ocupada con la anterior).
 */
uint32_t uart_stage_get_frames_dropped(void);

/**
 * @brief Alimenta una muestra del pipeline para streaming UART.
 *        Aplica decimación para no saturar el baudrate en frecuencias altas.
 *
 *        Camino heredado de TP2 (muestra a muestra, no por ventana) -- hoy
 *        'p' lo sigue usando porque todavía no hay una salida FFT/Bypass
 *        real del pipeline (issues #14/#15). El día que eso exista, 'p'
 *        pasa a llamar a uart_stage_print_samples() en su lugar y esto
 *        debería poder sacarse.
 * @param in_sample Muestra cruda/adquirida (Q15).
 * @param out_sample Muestra procesada (Q15).
 */
void uart_stage_feed_sample(q15_t in_sample, q15_t out_sample);

/**
 * @brief Función periódica para el loop principal (`while(1)`).
 *        Transmite los datos pendientes si el streaming está activo.
 */
void uart_stage_task(void);

/**
 * @brief Vuelca las muestras del buffer circular por UART en formato CSV para análisis en PC.
 * @param cb Puntero al buffer circular.
 * @param format_csv Si es true, imprime valores separados por comas; si es false, un valor por línea con índice.
 */
void uart_stage_dump_buffer(circular_buffer_t *cb, bool format_csv);

/**
 * @brief Habilita o deshabilita el streaming continuo hacia el Serial Plotter de la PC.
 * @param enable true para habilitar, false para pausar.
 */
void uart_stage_enable_streaming(bool enable);

/**
 * @brief Consulta si el streaming continuo está activo.
 */
bool uart_stage_is_streaming_enabled(void);

/**
 * @brief Configura el formato de salida del streaming.
 * @param mode Formato compatible con el visualizador seleccionado.
 */
void uart_stage_set_stream_mode(uart_stream_mode_t mode);

/**
 * @brief Obtiene el formato de salida actual del streaming.
 */
uart_stream_mode_t uart_stage_get_stream_mode(void);

/**
 * @brief Configura el factor de decimación del streaming.
 * @param decimation Enviar 1 de cada N muestras (por defecto 32 para 48 kHz).
 */
void uart_stage_set_decimation(uint32_t decimation);

/**
 * @brief Intenta leer un caracter recibido por UART sin bloquear la ejecución.
 * @param ch Puntero donde se guardará el caracter recibido.
 * @return true si había un caracter disponible, false en caso contrario.
 */
bool uart_stage_try_getchar(char *ch);

#endif /* UART_STAGE_H_ */
