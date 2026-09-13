/*
 * uart_stage.h
 *
 * Etapa de comunicación UART: transmisión de datos adquiridos y procesados hacia la PC.
 * Soporta:
 *  - Modo Serial Plotter (streaming en tiempo real para visualizadores en PC).
 *  - Modo Dump de Buffer (volcado de las 512 muestras para análisis en Python / MATLAB).
 */

#ifndef UART_STAGE_H_
#define UART_STAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"
#include "circular_buffer.h"

/**
 * @brief Inicializa la etapa UART.
 */
void uart_stage_init(void);

/**
 * @brief Alimenta una muestra del pipeline para streaming UART.
 *        Aplica decimación para no saturar el baudrate en frecuencias altas.
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
