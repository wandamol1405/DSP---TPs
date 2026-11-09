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

// Ventanas ping-pong: mientras la ISR del ADC llena s_frame_in[s_cur], PendSV procesa la
// otra (s_frame_ready). Se dimensionan para el N máximo (2 x 2048 x 2 bytes = 8 KB); con
// N menor se usa solo el principio de cada una.
static q15_t s_frame_in[2][PIPELINE_MAX_FRAME];
static volatile uint32_t s_cur = 0;            // ventana que llena la ISR
static volatile uint32_t s_idx = 0;            // próxima posición a escribir en s_frame_in[s_cur]
static volatile uint32_t s_frame_ready = 0;    // ventana que procesa PendSV
static volatile bool s_frame_busy = false;     // true desde que se pide procesar hasta que termina

// Tamaño de ventana. s_pending_n es el que eligió el usuario; s_frame_n es el de la ventana
// que se está llenando, y solo cambia cuando la ISR arranca una ventana nueva (s_idx == 0).
// Así una ventana nunca mezcla muestras de dos N distintos.
static volatile fft_size_t s_pending_n = FFT_SIZE_1024;
static uint32_t s_frame_n = 1024U;             // solo lo usa la ISR
static volatile uint32_t s_ready_n = 1024U;    // N de la ventana que procesa PendSV

static volatile uint32_t s_overrun_count = 0;   // ventanas que no se procesaron a tiempo
static volatile bool s_overrun_pending = false; // aviso para el loop principal

// Cantidad de muestras de cada tamaño de ventana, indexada por fft_size_t
static const uint16_t s_fft_size_n[FFT_SIZE_COUNT] = {
    [FFT_SIZE_512]  = 512U,
    [FFT_SIZE_1024] = 1024U,
    [FFT_SIZE_2048] = 2048U,
};

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
// Solo mueve muestras: la de entrada va a la ventana que se está llenando y se repite por
// el DAC. Al completar N muestras se pide a PendSV que procese la ventana (ver
// pipeline_process_block).
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

    // Etapa 3: el DAC repite la entrada, para seguir viendo la señal en el osciloscopio
    // (el resultado de la FFT es un espectro, no una señal para sacar por el DAC)
    dac_stage_write_sample(in_sample);

    uint32_t cur = s_cur;
    uint32_t idx = s_idx;

    // Arranca una ventana nueva: recién acá se aplica el cambio de N pendiente
    if (idx == 0U) {
        s_frame_n = s_fft_size_n[s_pending_n];
    }

    s_frame_in[cur][idx] = in_sample;

    if (++idx < s_frame_n) {
        s_idx = idx;
        return;
    }

    // Ventana completa
    s_idx = 0;
    if (s_frame_busy) {
        // PendSV todavía no terminó con la ventana anterior: esta se descarta y se vuelve a
        // llenar el mismo buffer. No se cambia de buffer porque el otro lo está leyendo PendSV.
        s_overrun_count++;
        s_overrun_pending = true;
        return;
    }

    s_frame_ready = cur;
    s_ready_n = s_frame_n;
    s_frame_busy = true;
    s_cur = cur ^ 1U;
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

// Procesa la ventana que dejó lista pipeline_step (se llama desde PendSV_Handler, con
// menor prioridad que la ISR del ADC). Mientras tanto la ISR llena la otra ventana.
void pipeline_process_block(void) {
    if (!s_frame_busy) {
        return;
    }

    const q15_t *in = s_frame_in[s_frame_ready];
    uint32_t n = s_ready_n;

    // Etapa 2: Procesamiento DSP. Por ahora en vacío; la FFT se agrega en la issue #15.

    // Salida UART (para PC / Serial Plotter). Sin procesamiento, la salida es la entrada.
    for (uint32_t i = 0; i < n; i++) {
        uart_stage_feed_sample(in[i], in[i]);
    }

    s_frame_busy = false;
}

// Avanza al siguiente tamaño de ventana. Solo se anota: la ISR lo aplica al arrancar la
// próxima ventana, así la que se está llenando termina con el N con el que empezó.
fft_size_t pipeline_next_fft_size(void) {
    fft_size_t next = (fft_size_t)((s_pending_n + 1) % FFT_SIZE_COUNT);
    s_pending_n = next;
    return next;
}

fft_size_t pipeline_get_fft_size(void) {
    return s_pending_n;
}

uint32_t pipeline_get_fft_size_n(void) {
    return s_fft_size_n[s_pending_n];
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

uint16_t pipeline_get_num_taps(void) {
    return processing_stage_get_num_taps();
}
