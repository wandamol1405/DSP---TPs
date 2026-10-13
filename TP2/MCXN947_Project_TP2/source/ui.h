/*
 * ui.h
 *
 * Interfaz gráfica del DSP: muestra Fs, tipo de filtro, Fc y BW en el OLED.
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
 * @brief Tipos de filtro que la UI sabe representar (nombre + gráfico de respuesta).
 */
typedef enum {
    UI_FILTER_BYPASS = 0, /**< Sin filtrado */
    UI_FILTER_LOWPASS,    /**< Pasa bajos */
    UI_FILTER_HIGHPASS,   /**< Pasa altos */
    UI_FILTER_BANDPASS,   /**< Pasa banda */
    UI_FILTER_BANDSTOP,   /**< Elimina banda */
    UI_FILTER_COUNT       /**< Cantidad de tipos (no es un tipo válido) */
} ui_filter_t;

/**
 * @brief Valor de frecuencia que indica "no aplica / desconocido" (se muestra como "--").
 */
#define UI_FREQUENCY_NONE 0xFFFFFFFFu

/**
 * @brief Inicializa el módulo y el display, y dibuja la pantalla completa con el estado por defecto
 *        (BYPASS, todas las frecuencias en "--").
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
 * @brief Actualiza la frecuencia de muestreo mostrada.
 * @param hz Frecuencia en Hz, o UI_FREQUENCY_NONE.
 */
void ui_set_sampling_frequency(uint32_t hz);

/**
 * @brief Actualiza el tipo de filtro (nombre y gráfico asociado).
 * @param type Tipo de filtro. Valores fuera de rango se ignoran.
 */
void ui_set_filter_type(ui_filter_t type);

/**
 * @brief Actualiza la frecuencia de corte mostrada.
 * @param hz Frecuencia en Hz, o UI_FREQUENCY_NONE si no aplica (p. ej. BYPASS).
 */
void ui_set_cutoff_frequency(uint32_t hz);

/**
 * @brief Actualiza el ancho de banda mostrado.
 * @param hz Ancho de banda en Hz, o UI_FREQUENCY_NONE si no aplica (p. ej. pasa bajos/altos).
 */
void ui_set_bandwidth(uint32_t hz);

/**
 * @brief Aplica al display los cambios pendientes, transmitiendo solo las regiones afectadas.
 *        Si no hay cambios retorna inmediatamente sin comunicar con el display.
 * @note Bloqueante mientras hay algo que transmitir. Solo contexto principal.
 */
void ui_process(void);

#endif /* UI_H_ */

