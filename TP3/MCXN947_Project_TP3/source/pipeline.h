/*
 * pipeline.h
 *
 * Coordinador del pipeline de procesamiento de señal.
 * Conecta las etapas: ADC -> Buffer Circular -> Procesamiento DSP -> Salida (DAC y UART).
 */

#ifndef PIPELINE_H_
#define PIPELINE_H_

#include <stdint.h>   // tipo uint32_t
#include <stdbool.h>  // tipo bool
#include "arm_math.h" // tipo q15_t (CMSIS-DSP)
#include "circular_buffer.h"  // circular_buffer_t, buffer interno del pipeline
#include "adc_stage.h"        // etapa de adquisición (ADC + CTIMER) y sample_rate_t
#include "processing_stage.h" // etapa de procesamiento DSP
#include "dac_stage.h"        // etapa de salida analógica (DAC0)
#include "uart_stage.h"       // etapa de comunicación por UART (tramas binarias y comandos)

// Tamaño del buffer circular interno del pipeline, en muestras (según consigna del TP1).
#define PIPELINE_BUFFER_SIZE 512

// Tamaño máximo de ventana (N), en muestras. Dimensiona los dos buffers del ping-pong y el
// de salida; es el mismo máximo que soporta la etapa de procesamiento.
#define PIPELINE_MAX_FRAME PROCESSING_MAX_FRAME

/**
 * @brief Tamaños de ventana (N) disponibles para la FFT.
 */
typedef enum {
    FFT_SIZE_512 = 0,
    FFT_SIZE_1024,
    FFT_SIZE_2048,
    FFT_SIZE_COUNT
} fft_size_t;

/**
 * @brief Descriptor de una ventana: con qué configuración se tomó. Se captura cuando
 *        arranca la ventana, así no cambia aunque se toque Fs, N o el modo mientras se llena.
 */
typedef struct {
    uint32_t fs_hz;   /**< Frecuencia de muestreo, en Hz */
    uint16_t n;       /**< Cantidad de muestras de la ventana */
    uint8_t mode;     /**< processing_mode_t con el que se procesa (BYPASS o FFT) */
} frame_info_t;

/**
 * @brief Inicializa el pipeline y todas sus etapas asociadas.
 */
void pipeline_init(void);

/**
 * @brief Ejecuta un paso del pipeline. Se invoca al ritmo de la tasa de muestreo (ISR del ADC).
 *        Guarda la muestra en la ventana que se está llenando y la repite por el DAC; al
 *        completar N muestras dispara PendSV.
 */
void pipeline_step(void);

/**
 * @brief Procesa la ventana de N muestras que quedó lista en el pipeline.
 *        Se invoca desde PendSV_Handler fuera de la ISR del ADC.
 */
void pipeline_process_block(void);

/**
 * @brief Obtiene el tamaño de ventana seleccionado, en muestras (512, 1024 o 2048).
 *        Puede no haberse aplicado todavía a la ventana que se está llenando.
 */
uint32_t pipeline_get_fft_size_n(void);

/**
 * @brief Consulta si ocurrió una sobrecarga de procesamiento (overrun)
 *        desde la última llamada y limpia la bandera.
 * @return true si ocurrió sobrecarga, false en caso contrario.
 */
bool pipeline_take_overrun(void);

/**
 * @brief Obtiene la cantidad acumulada de ventanas no procesadas a tiempo.
 */
uint32_t pipeline_get_overrun_count(void);

/**
 * @brief Inicia la adquisición y procesamiento continuo (Modo RUN).
 * @param rate Frecuencia de muestreo deseada.
 */
void pipeline_start(sample_rate_t rate);

/**
 * @brief Detiene la adquisición y pasa a reproducción en loop del buffer (Modo STOP).
 */
void pipeline_stop(void);

/**
 * @brief Alterna el estado entre RUN y STOP.
 * @return El nuevo estado (true = RUN, false = STOP).
 */
bool pipeline_toggle_run_stop(void);

/**
 * @brief Consulta si el pipeline está en adquisición (RUN).
 */
bool pipeline_is_running(void);

/**
 * @brief Avanza a la siguiente frecuencia de muestreo disponible.
 * @return La nueva frecuencia seleccionada.
 */
sample_rate_t pipeline_next_sample_rate(void);

/**
 * @brief Obtiene la frecuencia de muestreo actual.
 */
sample_rate_t pipeline_get_sample_rate(void);

/**
 * @brief Obtiene el valor en Hz de la frecuencia actual.
 */
uint32_t pipeline_get_sample_rate_hz(void);

/**
 * @brief Aplica una frecuencia de muestreo específica.
 */
void pipeline_set_sample_rate(sample_rate_t rate);

/**
 * @brief Obtiene el puntero al buffer circular de entrada del pipeline.
 */
circular_buffer_t* pipeline_get_circular_buffer(void);


/**
 * @brief Avanza al siguiente modo del ciclo BYPASS -> FFT 512 -> FFT 1024 -> FFT 2048 -> BYPASS
 *        (SW3 y comando 'm'). En BYPASS se mantiene el último N. El cambio es diferido: se
 *        aplica recién cuando arranca la próxima ventana.
 * @return El nuevo modo de procesamiento (el N se consulta con pipeline_get_fft_size_n()).
 */
processing_mode_t pipeline_next_processing_mode(void);

/**
 * @brief Obtiene el modo de procesamiento actual.
 */
processing_mode_t pipeline_get_processing_mode(void);

/**
 * @brief Obtiene el nombre del modo actual en formato de texto.
 */
const char* pipeline_get_processing_mode_name(void);

/**
 * @brief Obtiene la cantidad de coeficientes (taps) del filtro actual (0 en BYPASS).
 */
uint16_t pipeline_get_num_taps(void);

#endif /* PIPELINE_H_ */
