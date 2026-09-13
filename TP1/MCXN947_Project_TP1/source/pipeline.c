/*
 * pipeline.c
 *
 * Implementación del coordinador del pipeline de procesamiento de señal.
 */

#include "pipeline.h"

static q15_t s_buffer_storage[PIPELINE_BUFFER_SIZE];
static circular_buffer_t s_circ_buffer;

void pipeline_init(void) {
    circular_buffer_init(&s_circ_buffer, s_buffer_storage, PIPELINE_BUFFER_SIZE);
    adc_stage_init(&s_circ_buffer);
    processing_stage_init();
    dac_stage_init();
    uart_stage_init();
}

void pipeline_step(void) {
    q15_t in_sample = 0;
    q15_t out_sample = 0;

    if (adc_stage_is_running()) {
        // Modo RUN: Adquisición en vivo del ADC
        if (adc_stage_read_sample(&in_sample)) {
            // Etapa 2: Procesamiento DSP
            out_sample = processing_stage_process_sample(in_sample);

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

        // Etapa 3: Salida Analógica DAC
        dac_stage_write_sample(out_sample);

        // Salida UART (para PC / Serial Plotter)
        uart_stage_feed_sample(in_sample, out_sample);
    }
}

void pipeline_start(sample_rate_t rate) {
    adc_stage_start(rate);
}

void pipeline_stop(void) {
    adc_stage_stop();
}

bool pipeline_toggle_run_stop(void) {
    if (adc_stage_is_running()) {
        pipeline_stop();
    } else {
        pipeline_start(adc_stage_get_sample_rate());
    }
    return adc_stage_is_running();
}

bool pipeline_is_running(void) {
    return adc_stage_is_running();
}

sample_rate_t pipeline_next_sample_rate(void) {
    bool is_stopped = !adc_stage_is_running();
    return adc_stage_next_sample_rate(is_stopped);
}

sample_rate_t pipeline_get_sample_rate(void) {
    return adc_stage_get_sample_rate();
}

uint32_t pipeline_get_sample_rate_hz(void) {
    return adc_stage_get_sample_rate_hz(adc_stage_get_sample_rate());
}

void pipeline_set_sample_rate(sample_rate_t rate) {
    bool is_stopped = !adc_stage_is_running();
    adc_stage_apply_sample_rate(rate, is_stopped);
}

circular_buffer_t* pipeline_get_circular_buffer(void) {
    return &s_circ_buffer;
}

void pipeline_dump_to_uart(bool format_csv) {
    uart_stage_dump_buffer(&s_circ_buffer, format_csv);
}

