/*
 * circular_buffer.c
 *
 * Implementación del buffer circular para muestras de audio/DSP.
 */

#include "circular_buffer.h" // declaraciones propias (circular_buffer_t y API pública)

// Inicializa la estructura: asocia el almacenamiento externo y pone todos los índices en 0.
void circular_buffer_init(circular_buffer_t *cb, q15_t *storage, uint32_t capacity) {
    if (cb == NULL || storage == NULL || capacity == 0) {
        return;
    }
    cb->buffer = storage;
    cb->capacity = capacity;
    cb->write_idx = 0;
    cb->read_idx = 0;
    cb->count = 0;
}

// Escribe una muestra en write_idx y avanza el índice de forma circular (módulo capacity).
void circular_buffer_push(circular_buffer_t *cb, q15_t sample) {
    if (cb == NULL || cb->buffer == NULL) {
        return;
    }
    cb->buffer[cb->write_idx] = sample;
    cb->write_idx = (cb->write_idx + 1) % cb->capacity;

    if (cb->count < cb->capacity) {
        cb->count++;
    }
}

// Extrae (consume) la muestra en read_idx si hay al menos una disponible (FIFO).
bool circular_buffer_pop(circular_buffer_t *cb, q15_t *sample) {
    if (cb == NULL || cb->buffer == NULL || cb->count == 0) {
        return false;
    }
    if (sample != NULL) {
        *sample = cb->buffer[cb->read_idx];
    }
    cb->read_idx = (cb->read_idx + 1) % cb->capacity;
    cb->count--;
    return true;
}

// Lee la muestra en read_idx y avanza el índice, sin verificar cuenta (para reproducción en loop continuo).
q15_t circular_buffer_get_playback_sample(circular_buffer_t *cb) {
    if (cb == NULL || cb->buffer == NULL) {
        return 0;
    }
    q15_t sample = cb->buffer[cb->read_idx];
    cb->read_idx = (cb->read_idx + 1) % cb->capacity;
    return sample;
}

// Devuelve la última muestra escrita (posición write_idx - 1) sin modificar ningún índice.
q15_t circular_buffer_peek_last_written(const circular_buffer_t *cb) {
    if (cb == NULL || cb->buffer == NULL) {
        return 0;
    }
    uint32_t last_idx = (cb->write_idx == 0) ? (cb->capacity - 1) : (cb->write_idx - 1);
    return cb->buffer[last_idx];
}

// Devuelve la muestra almacenada en una posición arbitraria del buffer, sin modificar índices.
q15_t circular_buffer_peek_at(const circular_buffer_t *cb, uint32_t index) {
    if (cb == NULL || cb->buffer == NULL || index >= cb->capacity) {
        return 0;
    }
    return cb->buffer[index];
}

// Vuelve a empezar la lectura desde el principio del buffer (write_idx queda intacto).
void circular_buffer_reset_read(circular_buffer_t *cb) {
    if (cb != NULL) {
        cb->read_idx = 0;
    }
}

// Reinicia por completo el buffer: escritura, lectura y cantidad de muestras válidas a 0.
void circular_buffer_reset(circular_buffer_t *cb) {
    if (cb != NULL) {
        cb->write_idx = 0;
        cb->read_idx = 0;
        cb->count = 0;
    }
}

// Ubica el índice de lectura justo en la última muestra escrita (para retomar reproducción al detener el RUN).
void circular_buffer_sync_read_to_write(circular_buffer_t *cb) {
    if (cb != NULL) {
        cb->read_idx = (cb->write_idx == 0) ? (cb->capacity - 1) : (cb->write_idx - 1);
    }
}

// Getter del índice de escritura actual (0 si cb es NULL).
uint32_t circular_buffer_get_write_index(const circular_buffer_t *cb) {
    return (cb != NULL) ? cb->write_idx : 0;
}

// Getter del índice de lectura actual (0 si cb es NULL).
uint32_t circular_buffer_get_read_index(const circular_buffer_t *cb) {
    return (cb != NULL) ? cb->read_idx : 0;
}

