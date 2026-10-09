/*
 * pipeline.c
 *
 * Implementación del coordinador del pipeline de procesamiento de señal.
 */

#include "pipeline.h" // declaraciones propias y de todas las etapas que coordina

// Buffer circular de entrada (muestras del ADC)
static q15_t s_buffer_storage[PIPELINE_BUFFER_SIZE]; 
static circular_buffer_t s_circ_buffer;              

// Buffer circular de salida diferenciado (muestras procesadas antes del DAC)
static q15_t s_out_buffer_storage[PIPELINE_BUFFER_SIZE];
static circular_buffer_t s_out_circ_buffer;

// Inicializa el buffer circular y todas las etapas del pipeline en el orden correcto.
void pipeline_init(void) {
    circular_buffer_init(&s_circ_buffer, s_buffer_storage, PIPELINE_BUFFER_SIZE);
    circular_buffer_init(&s_out_circ_buffer, s_out_buffer_storage, PIPELINE_BUFFER_SIZE);
    
    adc_stage_init(&s_circ_buffer);
    processing_stage_init();
    
    // Notificamos a la etapa de procesamiento de la frecuencia inicial
    processing_stage_set_sample_rate(adc_stage_get_sample_rate());
    
    dac_stage_init();
    uart_stage_init();
}

// Ejecuta un ciclo completo del pipeline
void pipeline_step(void) {
    q15_t in_sample = 0;
    q15_t out_sample = 0;

    if (adc_stage_is_running()) {
        // Modo RUN: Adquisición en vivo del ADC
        if (adc_stage_read_sample(&in_sample)) {
            // Etapa 2: Procesamiento DSP
            out_sample = processing_stage_process_sample(in_sample);

            // Guardamos la muestra procesada en el buffer de salida diferenciado
            circular_buffer_push(&s_out_circ_buffer, out_sample);

            // Etapa 3: Salida Analógica DAC
            dac_stage_write_sample(out_sample);

            // Salida UART (para PC / Serial Plotter)
            uart_stage_feed_sample(in_sample, out_sample);
        }
    } else {
        // Modo STOP: Reproducción continua del buffer circular
        in_sample = circular_buffer_get_playback_sample(&s_circ_buffer);

        // Etapa 2: Procesamiento DSP (permite aplicar filtros sobre la señal congelada)
        out_sample = processing_stage_process_sample(in_sample);

        // Guardamos la muestra procesada en el buffer de salida diferenciado
        circular_buffer_push(&s_out_circ_buffer, out_sample);

        // Etapa 3: Salida Analógica DAC
        dac_stage_write_sample(out_sample);

        // Salida UART (para PC / Serial Plotter)
        uart_stage_feed_sample(in_sample, out_sample);
    }
}

// Delega a la etapa ADC el inicio de la adquisición (modo RUN) a la frecuencia dada.
void pipeline_start(sample_rate_t rate) {
    adc_stage_start(rate);
}

// Delega a la etapa ADC la detención de la adquisición (modo STOP).
void pipeline_stop(void) {
    adc_stage_stop();
}

// Alterna entre RUN y STOP según el estado actual de la etapa ADC.
bool pipeline_toggle_run_stop(void) {
    if (adc_stage_is_running()) {
        pipeline_stop();
    } else {
        pipeline_start(adc_stage_get_sample_rate());
    }
    return adc_stage_is_running();
}

// Indica si el pipeline está actualmente adquiriendo (RUN).
bool pipeline_is_running(void) {
    return adc_stage_is_running();
}

// Avanza a la siguiente frecuencia de muestreo; si está en STOP, difiere la reprogramación
// del timer hasta el próximo RUN (no tiene sentido cambiar el trigger de un ADC detenido).
sample_rate_t pipeline_next_sample_rate(void) {
    bool is_stopped = !adc_stage_is_running();
    sample_rate_t new_rate = adc_stage_next_sample_rate(is_stopped);
    // Sincronizar el filtro con la nueva frecuencia
    processing_stage_set_sample_rate(new_rate);
    return new_rate;
}

// Getter de la frecuencia de muestreo actual (delegado a la etapa ADC).
sample_rate_t pipeline_get_sample_rate(void) {
    return adc_stage_get_sample_rate();
}

// Getter del valor en Hz de la frecuencia de muestreo actual.
uint32_t pipeline_get_sample_rate_hz(void) {
    return adc_stage_get_sample_rate_hz(adc_stage_get_sample_rate());
}

// Aplica una frecuencia de muestreo específica, difiriendo la reprogramación del timer si
// el pipeline está en STOP.
void pipeline_set_sample_rate(sample_rate_t rate) {
    bool is_stopped = !adc_stage_is_running();
    adc_stage_apply_sample_rate(rate, is_stopped);
    // Sincronizar el filtro con la nueva frecuencia
    processing_stage_set_sample_rate(rate);
}

// Expone el buffer circular interno (por ejemplo, para que otra etapa lo consulte directamente).
circular_buffer_t* pipeline_get_circular_buffer(void) {
    return &s_circ_buffer;
}

// Expone el buffer circular de salida (para análisis, etc.).
circular_buffer_t* pipeline_get_output_circular_buffer(void) {
    return &s_out_circ_buffer;
}

// Solicita a la etapa UART que envíe el contenido completo del buffer circular por el puerto serie.
void pipeline_dump_to_uart(bool format_csv) {
    uart_stage_dump_buffer(&s_circ_buffer, format_csv);
}

// Funciones pasarela (proxy) hacia processing_stage
processing_mode_t pipeline_next_processing_mode(void) {
    return processing_stage_next_mode();
}

processing_mode_t pipeline_get_processing_mode(void) {
    return processing_stage_get_mode();
}

const char* pipeline_get_processing_mode_name(void) {
    return processing_stage_get_mode_name(processing_stage_get_mode());
}
