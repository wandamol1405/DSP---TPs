/*
 * ui.h
 *
 * Interfaz gráfica del DSP (TP3 - FFT): muestra en el OLED el modo activo (BYPASS o FFT),
 * el tamaño de ventana (N), la frecuencia de muestreo (Fs), la resolución en frecuencia
 * (df = Fs / N) y la frecuencia del pico del espectro.
 *
 * API independiente del hardware de visualización: no expone U8g2, el controlador,
 * el bus ni coordenadas. La aplicación solo informa QUÉ cambió; este módulo decide
 * qué regiones de pantalla actualizar.
 *
 * Modelo de concurrencia:
 *   - ui_set_*() y ui_show_screen() NO tocan el display: solo registran el valor pedido y
 *     marcan qué quedó pendiente (sección crítica de pocas instrucciones). Por eso pueden
 *     llamarse desde cualquier contexto, incluidas ISRs.
 *   - ui_init() y ui_process() SÍ comunican con el display (I2C por polling, bloqueante)
 *     y deben llamarse únicamente desde el contexto principal (loop de main), nunca desde ISR.
 */

#ifndef UI_H_
#define UI_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Modos de operación que la UI sabe representar.
 */
typedef enum {
    UI_MODE_BYPASS = 0, /**< Copia directa de la ventana */
    UI_MODE_FFT,        /**< Espectro de magnitud por FFT */
    UI_MODE_COUNT       /**< Cantidad de modos (no es un modo válido) */
} ui_mode_t;

/**
 * @brief Valor de frecuencia que indica "no aplica / desconocido" (se muestra como "--").
 */
#define UI_FREQUENCY_NONE 0xFFFFFFFFu

/**
 * @brief Valor de tamaño de ventana que indica "desconocido" (se muestra como "--").
 */
#define UI_WINDOW_SIZE_NONE 0u

/**
 * @brief Inicializa el módulo y el display, y dibuja la pantalla completa con el estado por defecto
 *        (BYPASS, N y todas las frecuencias en "--").
 * @return true si el display respondió. Si devuelve false la UI queda deshabilitada y el resto de
 *         las funciones pasan a ser inofensivas (no intentan comunicar con el display).
 * @note Bloqueante. Solo contexto principal.
 */
bool ui_init(void);

/**
 * @brief Solicita redibujar la pantalla completa a partir del estado actual de la UI
 *        (se aplica en la próxima llamada a ui_process()).
 */
void ui_show_screen(void);

/**
 * @brief Actualiza el modo mostrado en el encabezado (BYPASS o FFT).
 * @param mode Modo activo. Valores fuera de rango se ignoran.
 */
void ui_set_mode(ui_mode_t mode);

/**
 * @brief Actualiza el tamaño de la ventana N mostrado (512, 1024, 2048) y recalcula df.
 * @param n Cantidad de muestras de la ventana, o UI_WINDOW_SIZE_NONE (0).
 */
void ui_set_window_size(uint16_t n);

/**
 * @brief Actualiza la frecuencia de muestreo mostrada y recalcula df.
 * @param hz Frecuencia en Hz, o UI_FREQUENCY_NONE.
 */
void ui_set_sampling_frequency(uint32_t hz);

/**
 * @brief Actualiza la frecuencia del pico de la FFT mostrada.
 * @param hz Frecuencia del pico en Hz, o UI_FREQUENCY_NONE si no aplica (en BYPASS).
 */
void ui_set_peak_frequency(uint32_t hz);

/**
 * @brief Aplica al display los cambios pendientes, transmitiendo solo las regiones afectadas.
 *        Si no hay cambios retorna inmediatamente sin comunicar con el display.
 * @note Bloqueante mientras hay algo que transmitir. Solo contexto principal.
 */
void ui_process(void);

#endif /* UI_H_ */
