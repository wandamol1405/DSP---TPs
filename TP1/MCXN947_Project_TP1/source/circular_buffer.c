/*
 * circular_buffer.c
 *
 * Implementación del buffer circular para muestras de audio/DSP.
 */

#include "circular_buffer.h"

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

q15_t circular_buffer_get_playback_sample(circular_buffer_t *cb) {
    if (cb == NULL || cb->buffer == NULL) {
        return 0;
    }
    q15_t sample = cb->buffer[cb->read_idx];
    cb->read_idx = (cb->read_idx + 1) % cb->capacity;
    return sample;
}

q15_t circular_buffer_peek_last_written(const circular_buffer_t *cb) {
    if (cb == NULL || cb->buffer == NULL) {
        return 0;
    }
    uint32_t last_idx = (cb->write_idx == 0) ? (cb->capacity - 1) : (cb->write_idx - 1);
    return cb->buffer[last_idx];
}

q15_t circular_buffer_peek_at(const circular_buffer_t *cb, uint32_t index) {
    if (cb == NULL || cb->buffer == NULL || index >= cb->capacity) {
        return 0;
    }
    return cb->buffer[index];
}

void circular_buffer_reset_read(circular_buffer_t *cb) {
    if (cb != NULL) {
        cb->read_idx = 0;
    }
}

void circular_buffer_reset(circular_buffer_t *cb) {
    if (cb != NULL) {
        cb->write_idx = 0;
        cb->read_idx = 0;
        cb->count = 0;
    }
}

void circular_buffer_sync_read_to_write(circular_buffer_t *cb) {
    if (cb != NULL) {
        cb->read_idx = (cb->write_idx == 0) ? (cb->capacity - 1) : (cb->write_idx - 1);
    }
}

uint32_t circular_buffer_get_write_index(const circular_buffer_t *cb) {
    return (cb != NULL) ? cb->write_idx : 0;
}

uint32_t circular_buffer_get_read_index(const circular_buffer_t *cb) {
    return (cb != NULL) ? cb->read_idx : 0;
}

