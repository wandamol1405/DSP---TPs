/*
 * pipeline.c
 *
 * Implementación del coordinador del pipeline de procesamiento de señal.
 */

#include "pipeline.h" // declaraciones propias y de todas las etapas que coordina
#include "fsl_device_registers.h" // SCB y NVIC (PendSV y su prioridad)

// Buffer circular de entrada (muestras del ADC)
static q15_t s_buffer_storage[PIPELINE_BUFFER_SIZE]; 
static circular_buffer_t s_circ_buffer;              

// Buffer circular de salida diferenciado (muestras procesadas antes del DAC)
static q15_t s_out_buffer_storage[PIPELINE_BUFFER_SIZE];
static circular_buffer_t s_out_circ_buffer;

// Bloques ping-pong del procesamiento: mientras la ISR del ADC llena s_blk_in[cur] y saca
// por el DAC s_blk_out[cur], PendSV filtra el otro par (s_blk_ready). Latencia: 2 bloques.
static q15_t s_blk_in[2][PROCESSING_BLOCK_SIZE];
static q15_t s_blk_out[2][PROCESSING_BLOCK_SIZE];
static volatile uint32_t s_blk_cur = 0;       // par de buffers que usa la ISR
static volatile uint32_t s_blk_idx = 0;       // posición dentro del bloque actual
static volatile uint32_t s_blk_ready = 0;     // par de buffers que filtra PendSV
static volatile bool s_blk_busy = false;      // true desde que se pide filtrar hasta que termina

static volatile uint32_t s_overrun_count = 0;   // bloques que no se filtraron a tiempo
static volatile bool s_overrun_pending = false; // aviso para el loop principal

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

    // El filtrado corre en PendSV con la menor prioridad: la ISR del ADC (prioridad 0)
    // siempre puede interrumpirlo para no perder muestras.
    NVIC_SetPriority(PendSV_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
}

// Ejecuta un paso del pipeline (en la ISR del ADC, una vez por período de muestreo).
// Solo mueve muestras: la de entrada va al bloque que se está llenando y al DAC sale la
// muestra correspondiente del último bloque ya filtrado. Al completar un bloque se pide
// a PendSV que lo filtre (ver pipeline_process_block).
void pipeline_step(void) {
    q15_t in_sample = 0;

    if (adc_stage_is_running()) {
        // Modo RUN: Adquisición en vivo del ADC
        if (!adc_stage_read_sample(&in_sample)) {
            return;
        }
    } else {
        // Modo STOP: Reproducción continua del buffer circular
        in_sample = circular_buffer_get_playback_sample(&s_circ_buffer);
    }

    uint32_t cur = s_blk_cur;
    uint32_t idx = s_blk_idx;

    // Etapa 3: Salida Analógica DAC
    dac_stage_write_sample(s_blk_out[cur][idx]);
    s_blk_in[cur][idx] = in_sample;

    if (++idx < PROCESSING_BLOCK_SIZE) {
        s_blk_idx = idx;
        return;
    }

    // Bloque completo: pasa a filtrarse y la ISR sigue con el otro par de buffers
    s_blk_idx = 0;
    if (s_blk_busy) {
        // El bloque anterior todavía no terminó de filtrarse: el filtro no entra en tiempo
        // real a esta Fs. Se pasa a bypass para que el resto del sistema (UART) siga vivo.
        s_overrun_count++;
        s_overrun_pending = true;
        processing_stage_set_mode(PROCESSING_MODE_BYPASS);
    } else {
        s_blk_ready = cur;
        s_blk_busy = true;
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
    s_blk_cur = cur ^ 1U;
}

// Filtra el bloque que dejó listo pipeline_step (se llama desde PendSV_Handler, con
// menor prioridad que la ISR del ADC). Mientras tanto la ISR usa el otro par de buffers.
void pipeline_process_block(void) {
    if (!s_blk_busy) {
        return;
    }

    const q15_t *in = s_blk_in[s_blk_ready];
    q15_t *out = s_blk_out[s_blk_ready];

    // Etapa 2: Procesamiento DSP
    processing_stage_process_block(in, out, PROCESSING_BLOCK_SIZE);

    for (uint32_t i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        // Guardamos la muestra procesada en el buffer de salida diferenciado
        circular_buffer_push(&s_out_circ_buffer, out[i]);

        // Salida UART (para PC / Serial Plotter)
        uart_stage_feed_sample(in[i], out[i]);
    }

    s_blk_busy = false;
}

// Indica si hubo una sobrecarga desde la última consulta (y limpia el aviso).
bool pipeline_take_overrun(void) {
    bool pending = s_overrun_pending;
    s_overrun_pending = false;
    return pending;
}

// Cantidad total de bloques que no llegaron a filtrarse a tiempo.
uint32_t pipeline_get_overrun_count(void) {
    return s_overrun_count;
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
