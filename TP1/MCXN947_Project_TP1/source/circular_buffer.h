/*
 * circular_buffer.h
 *
 * Buffer circular optimizado para muestras de audio/DSP en formato Q15.
 */

#ifndef CIRCULAR_BUFFER_H_
#define CIRCULAR_BUFFER_H_

#include <stdint.h>  // tipos de ancho fijo (uint32_t)
#include <stdbool.h> // tipo bool
#include <stddef.h>  // NULL
#include "arm_math.h" // tipo q15_t (Q15 con signo de 16 bits) del CMSIS-DSP

// Capacidad por defecto del buffer circular, en muestras (según consigna del TP1)
#define CIRCULAR_BUFFER_DEFAULT_CAPACITY 512

typedef struct {
    q15_t *buffer;              /**< Puntero al arreglo de almacenamiento */
    uint32_t capacity;          /**< Capacidad total del buffer */
    volatile uint32_t write_idx;/**< Índice de escritura */
    volatile uint32_t read_idx; /**< Índice de lectura/reproducción */
    volatile uint32_t count;    /**< Cantidad de muestras válidas almacenadas */
} circular_buffer_t;

/**
 * @brief Inicializa el buffer circular.
 * @param cb Puntero a la estructura del buffer.
 * @param storage Puntero a la memoria asignada para el buffer.
 * @param capacity Capacidad máxima en muestras.
 */
void circular_buffer_init(circular_buffer_t *cb, q15_t *storage, uint32_t capacity);

/**
 * @brief Inserta una muestra en el buffer circular (avance circular automático).
 * @param cb Puntero al buffer.
 * @param sample Muestra en formato Q15 a guardar.
 */
void circular_buffer_push(circular_buffer_t *cb, q15_t sample);

/**
 * @brief Extrae una muestra del buffer circular (modo FIFO).
 * @param cb Puntero al buffer.
 * @param sample Puntero donde se almacenará la muestra leída.
 * @return true si se extrajo una muestra, false si estaba vacío.
 */
bool circular_buffer_pop(circular_buffer_t *cb, q15_t *sample);

/**
 * @brief Obtiene la siguiente muestra para reproducción en loop (modo STOP) y avanza el índice de lectura.
 * @param cb Puntero al buffer.
 * @return Muestra leída en formato Q15.
 */
q15_t circular_buffer_get_playback_sample(circular_buffer_t *cb);

/**
 * @brief Obtiene la última muestra escrita sin avanzar ningún índice.
 * @param cb Puntero al buffer.
 * @return Última muestra escrita en Q15.
 */
q15_t circular_buffer_peek_last_written(const circular_buffer_t *cb);

/**
 * @brief Obtiene una muestra en una posición específica del buffer.
 * @param cb Puntero al buffer.
 * @param index Índice a consultar (0 .. capacity-1).
 * @return Muestra en la posición indicada.
 */
q15_t circular_buffer_peek_at(const circular_buffer_t *cb, uint32_t index);

/**
 * @brief Reinicia el puntero de lectura a 0 (utilizado al pasar a STOP).
 * @param cb Puntero al buffer.
 */
void circular_buffer_reset_read(circular_buffer_t *cb);

/**
 * @brief Reinicia todos los índices del buffer a 0.
 * @param cb Puntero al buffer.
 */
void circular_buffer_reset(circular_buffer_t *cb);

/**
 * @brief Sincroniza el índice de lectura con la última muestra escrita.
 * @param cb Puntero al buffer.
 */
void circular_buffer_sync_read_to_write(circular_buffer_t *cb);

/**
 * @brief Retorna el índice actual de escritura.
 */
uint32_t circular_buffer_get_write_index(const circular_buffer_t *cb);

/**
 * @brief Retorna el índice actual de lectura.
 */
uint32_t circular_buffer_get_read_index(const circular_buffer_t *cb);

#endif /* CIRCULAR_BUFFER_H_ */

