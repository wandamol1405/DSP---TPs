/*
 * ui.c
 *
 * Implementación de la interfaz gráfica del DSP sobre U8g2 (SSD1306 128x64, full buffer).
 *
 * Granularidad de actualización
 * -----------------------------
 * U8g2 en modo full buffer mantiene los 1024 B del framebuffer en RAM y dibujar en él NO
 * transmite nada. La transmisión se hace con u8g2_UpdateDisplayArea(tx, ty, tw, th), cuya
 * unidad mínima es la TILE de 8x8 píxeles: cada fila de tiles se envía como una transacción
 * I2C independiente (comando de posición + datos). No es posible transmitir menos que una
 * tile, ni una región no alineada a 8 píxeles. Por eso todas las regiones de este módulo se
 * definen alineadas a tiles, y todo lo que se dibuje en una región debe quedar dentro de ella
 * (U8g2 no recorta contra la región: si se dibujara afuera, el framebuffer quedaría distinto
 * del display hasta el próximo refresco completo).
 *
 * Distribución (128x64, 16x8 tiles). El panel bicolor típico de 0.96" tiene amarillo en las
 * primeras 16 filas (tiles 0-1) y azul en las 48 restantes: el encabezado (modo y N) ocupa
 * justo la franja amarilla y nada atraviesa el límite y=16.
 *
 *   x:  0...........55 | 56..........127
 *   y0  +--------------+---------------+
 *       |  <MODO>      |       N: <N>  |  MODE (tiles 0,0 7x2) | N (tiles 7,0 9x2)
 *   y16 +--------------+---------------+
 *       |  Fs:   ...                   |  FS   (tiles 0,2 16x2)
 *   y32 |  df:   ...                   |  DF   (tiles 0,4 16x2)
 *   y48 |  Pico: ...                   |  PEAK (tiles 0,6 16x2)
 *   y63 +------------------------------+
 */

#include "ui.h"
#include "display_port.h" // backend: display_port_init() y u8g2
#include "fsl_common.h"   // DisableGlobalIRQ() / EnableGlobalIRQ(): sección crítica del SDK

// ============================================================================
// Regiones (en tiles de 8x8 píxeles)
// ============================================================================
typedef struct {
    uint8_t tx; // columna de tile inicial
    uint8_t ty; // fila de tile inicial
    uint8_t tw; // ancho en tiles
    uint8_t th; // alto en tiles
} ui_region_t;

static const ui_region_t REGION_MODE = {0, 0, 7, 2};
static const ui_region_t REGION_N    = {7, 0, 9, 2};
static const ui_region_t REGION_FS   = {0, 2, 16, 2};
static const ui_region_t REGION_DF   = {0, 4, 16, 2};
static const ui_region_t REGION_PEAK = {0, 6, 16, 2};

#define TILE_PX 8u

// ============================================================================
// Fuentes y posiciones de texto (en píxeles)
// ============================================================================
#define FONT_HEADER u8g2_font_7x14_tr
#define FONT_PARAM  u8g2_font_6x12_tr

#define HEADER_BASELINE_Y     12u  // dentro de la franja amarilla (y < 16)
#define HEADER_MODE_X         4u   // margen izquierdo del modo en el encabezado
#define HEADER_N_RIGHT_X      124u // borde derecho de "N: <N>" en el encabezado
#define PARAM_LABEL_X         4u
#define PARAM_VALUE_X         44u
#define PARAM_BASELINE_OFFSET 12u  // baseline respecto del borde superior de la región (16 px de alto)

#define UI_MAX_DISPLAY_HZ   99999000u
#define UI_MAX_DISPLAY_N    9999u
#define UI_VALUE_BUF_SIZE   14u

static const char *const MODE_NAMES[UI_MODE_COUNT] = {
    [UI_MODE_BYPASS] = "BYPASS",
    [UI_MODE_FFT]    = "FFT",
};

// ============================================================================
// Estado
// ============================================================================
typedef struct {
    uint32_t fs_hz;
    uint32_t peak_hz;
    uint16_t n;
    ui_mode_t mode;
} ui_state_t;

// Bits de s_dirty: qué parámetros cambiaron desde el último ui_process()
#define UI_DIRTY_MODE (1u << 0)
#define UI_DIRTY_N    (1u << 1)
#define UI_DIRTY_FS   (1u << 2)
#define UI_DIRTY_PEAK (1u << 3)
#define UI_DIRTY_FULL (1u << 4)

static u8g2_t s_u8g2;          // instancia de U8g2 (incluye el framebuffer de 1024 B)
static bool s_enabled = false; // true una vez que el display inicializó correctamente

// Estado pedido por la aplicación. Lo escriben los ui_set_*() (posiblemente desde ISR) y lo lee
// ui_process(); todo acceso es dentro de una sección crítica.
static ui_state_t s_pending = {UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_WINDOW_SIZE_NONE, UI_MODE_BYPASS};
static uint8_t s_dirty = 0;

// Estado que realmente está dibujado en el display. Solo lo toca el contexto principal.
static ui_state_t s_shown = {UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_WINDOW_SIZE_NONE, UI_MODE_BYPASS};

// ============================================================================
// Formato de texto (sin printf: Redlib con CR_INTEGER_PRINTF no soporta flotantes y evita código extra)
// ============================================================================
static char *append_str(char *p, const char *s) {
    while (*s != '\0') {
        *p++ = *s++;
    }
    return p;
}

static char *append_uint(char *p, uint32_t v) {
    char tmp[10];
    uint8_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        *p++ = tmp[--n];
    }
    return p;
}

// Formatea la frecuencia de muestreo: "--", "8 kHz", "22.05 kHz", "48 kHz".
static void format_sampling_frequency(char *buf, uint32_t hz) {
    char *p = buf;

    if (hz == UI_FREQUENCY_NONE) {
        p = append_str(p, "--");
    } else if (hz < 1000u) {
        p = append_uint(p, hz);
        p = append_str(p, " Hz");
    } else {
        if (hz > UI_MAX_DISPLAY_HZ) {
            hz = UI_MAX_DISPLAY_HZ;
        }
        uint32_t centi = (hz + 5u) / 10u; // kHz x 100, redondeado a 10 Hz
        if (centi >= 10000u) {
            centi = ((hz + 500u) / 1000u) * 100u; // desde 100 kHz sin decimales
        }
        p = append_uint(p, centi / 100u);
        uint32_t frac = centi % 100u;
        if (frac != 0u) {
            *p++ = '.';
            *p++ = (char)('0' + (frac / 10u));
            if ((frac % 10u) != 0u) {
                *p++ = (char)('0' + (frac % 10u));
            }
        }
        p = append_str(p, " kHz");
    }
    *p = '\0';
}

// Formatea la resolución en frecuencia df = Fs / N con un decimal en Hz: "--", "15.6 Hz", "3.9 Hz".
static void format_resolution(char *buf, uint32_t fs_hz, uint16_t n) {
    char *p = buf;

    if ((fs_hz == UI_FREQUENCY_NONE) || (n == UI_WINDOW_SIZE_NONE)) {
        p = append_str(p, "--");
    } else {
        if (fs_hz > UI_MAX_DISPLAY_HZ) {
            fs_hz = UI_MAX_DISPLAY_HZ;
        }
        uint32_t df_x10 = (fs_hz * 10u + ((uint32_t)n / 2u)) / (uint32_t)n;
        p = append_uint(p, df_x10 / 10u);
        *p++ = '.';
        *p++ = (char)('0' + (df_x10 % 10u));
        p = append_str(p, " Hz");
    }
    *p = '\0';
}

// Formatea la frecuencia del pico en Hz enteros: "--", "1000 Hz", "988 Hz", "1008 Hz".
static void format_peak_frequency(char *buf, uint32_t hz) {
    char *p = buf;

    if (hz == UI_FREQUENCY_NONE) {
        p = append_str(p, "--");
    } else {
        if (hz > UI_MAX_DISPLAY_HZ) {
            hz = UI_MAX_DISPLAY_HZ;
        }
        p = append_uint(p, hz);
        p = append_str(p, " Hz");
    }
    *p = '\0';
}

// Formatea el tamaño de ventana: "N: --", "N: 512", "N: 1024", "N: 2048".
static void format_window_size(char *buf, uint16_t n) {
    char *p = append_str(buf, "N: ");

    if (n == UI_WINDOW_SIZE_NONE) {
        p = append_str(p, "--");
    } else {
        uint16_t val = (n > UI_MAX_DISPLAY_N) ? UI_MAX_DISPLAY_N : n;
        p = append_uint(p, val);
    }
    *p = '\0';
}

// ============================================================================
// Primitivas de dibujo por región (solo escriben en el framebuffer)
// ============================================================================

// Borra la región completa (rectángulo de color de fondo) antes de volver a dibujar su contenido
static void clear_region(const ui_region_t *r) {
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, (u8g2_uint_t)(r->tx * TILE_PX), (u8g2_uint_t)(r->ty * TILE_PX),
                 (u8g2_uint_t)(r->tw * TILE_PX), (u8g2_uint_t)(r->th * TILE_PX));
    u8g2_SetDrawColor(&s_u8g2, 1);
}

// Transmite al display solo las tiles de la región
static void flush_region(const ui_region_t *r) {
    u8g2_UpdateDisplayArea(&s_u8g2, r->tx, r->ty, r->tw, r->th);
}

static void draw_mode(ui_mode_t mode) {
    const char *name = MODE_NAMES[mode];

    clear_region(&REGION_MODE);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetFontDirection(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, FONT_HEADER);
    u8g2_DrawStr(&s_u8g2, HEADER_MODE_X, HEADER_BASELINE_Y, name);
    u8g2_DrawHLine(&s_u8g2, (u8g2_uint_t)(REGION_MODE.tx * TILE_PX), 15,
                   (u8g2_uint_t)(REGION_MODE.tw * TILE_PX));
}

static void draw_window_size(uint16_t n) {
    char label[UI_VALUE_BUF_SIZE];
    format_window_size(label, n);

    clear_region(&REGION_N);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetFontDirection(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, FONT_HEADER);
    u8g2_uint_t w = u8g2_GetStrWidth(&s_u8g2, label);
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)(HEADER_N_RIGHT_X - w), HEADER_BASELINE_Y, label);
    u8g2_DrawHLine(&s_u8g2, (u8g2_uint_t)(REGION_N.tx * TILE_PX), 15,
                   (u8g2_uint_t)(REGION_N.tw * TILE_PX));
}

// Dibuja una fila "Etiqueta: valor" en su región (borra primero para no dejar restos de caracteres)
static void draw_row(const ui_region_t *r, const char *label, const char *value) {
    u8g2_uint_t baseline = (u8g2_uint_t)(r->ty * TILE_PX + PARAM_BASELINE_OFFSET);

    clear_region(r);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetFontDirection(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, FONT_PARAM);
    u8g2_DrawStr(&s_u8g2, PARAM_LABEL_X, baseline, label);
    u8g2_DrawStr(&s_u8g2, PARAM_VALUE_X, baseline, value);
}

static void draw_fs(uint32_t fs_hz) {
    char value[UI_VALUE_BUF_SIZE];
    format_sampling_frequency(value, fs_hz);
    draw_row(&REGION_FS, "Fs:", value);
}

static void draw_df(uint32_t fs_hz, uint16_t n) {
    char value[UI_VALUE_BUF_SIZE];
    format_resolution(value, fs_hz, n);
    draw_row(&REGION_DF, "df:", value);
}

static void draw_peak(uint32_t peak_hz) {
    char value[UI_VALUE_BUF_SIZE];
    format_peak_frequency(value, peak_hz);
    draw_row(&REGION_PEAK, "Pico:", value);
}

// Reconstruye todo el framebuffer a partir de un estado y lo transmite completo
static void draw_full(const ui_state_t *st) {
    u8g2_ClearBuffer(&s_u8g2);
    draw_mode(st->mode);
    draw_window_size(st->n);
    draw_fs(st->fs_hz);
    draw_df(st->fs_hz, st->n);
    draw_peak(st->peak_hz);
    u8g2_SendBuffer(&s_u8g2);
}

// ============================================================================
// API pública
// ============================================================================
bool ui_init(void) {
    s_enabled = false;

    if (!display_port_init(&s_u8g2)) {
        return false;
    }

    // Pantalla completa con el estado por defecto; los cambios pedidos antes de ui_init()
    // (si los hubiera) quedaron en s_pending/s_dirty y se aplican en el primer ui_process()
    s_shown = (ui_state_t){UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_WINDOW_SIZE_NONE, UI_MODE_BYPASS};
    draw_full(&s_shown);

    s_enabled = true;
    return true;
}

void ui_show_screen(void) {
    uint32_t primask = DisableGlobalIRQ();
    s_dirty |= UI_DIRTY_FULL;
    EnableGlobalIRQ(primask);
}

void ui_set_mode(ui_mode_t mode) {
    if ((unsigned int)mode >= (unsigned int)UI_MODE_COUNT) {
        return;
    }
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.mode != mode) {
        s_pending.mode = mode;
        s_dirty |= UI_DIRTY_MODE;
    }
    EnableGlobalIRQ(primask);
}

void ui_set_window_size(uint16_t n) {
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.n != n) {
        s_pending.n = n;
        s_dirty |= UI_DIRTY_N;
    }
    EnableGlobalIRQ(primask);
}

void ui_set_sampling_frequency(uint32_t hz) {
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.fs_hz != hz) {
        s_pending.fs_hz = hz;
        s_dirty |= UI_DIRTY_FS;
    }
    EnableGlobalIRQ(primask);
}

void ui_set_peak_frequency(uint32_t hz) {
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.peak_hz != hz) {
        s_pending.peak_hz = hz;
        s_dirty |= UI_DIRTY_PEAK;
    }
    EnableGlobalIRQ(primask);
}

void ui_process(void) {
    if (!s_enabled) {
        return;
    }

    // Toma una copia coherente del pedido y limpia los pendientes. Lo que una ISR modifique
    // desde este punto quedará marcado de nuevo y se atenderá en la próxima llamada.
    uint32_t primask = DisableGlobalIRQ();
    ui_state_t want = s_pending;
    uint8_t dirty = s_dirty;
    s_dirty = 0;
    EnableGlobalIRQ(primask);

    if (dirty == 0u) {
        return; // nada que hacer: no se comunica con el display
    }

    if ((dirty & UI_DIRTY_FULL) != 0u) {
        s_shown = want;
        draw_full(&s_shown);
        return;
    }

    bool df_changed = false;

    // Cada parámetro actualiza solo su región, y solo si el valor mostrado realmente difiere
    if (((dirty & UI_DIRTY_MODE) != 0u) && (want.mode != s_shown.mode)) {
        s_shown.mode = want.mode;
        draw_mode(s_shown.mode);
        flush_region(&REGION_MODE);
    }
    if (((dirty & UI_DIRTY_N) != 0u) && (want.n != s_shown.n)) {
        s_shown.n = want.n;
        draw_window_size(s_shown.n);
        flush_region(&REGION_N);
        df_changed = true;
    }
    if (((dirty & UI_DIRTY_FS) != 0u) && (want.fs_hz != s_shown.fs_hz)) {
        s_shown.fs_hz = want.fs_hz;
        draw_fs(s_shown.fs_hz);
        flush_region(&REGION_FS);
        df_changed = true;
    }
    if (df_changed) {
        draw_df(s_shown.fs_hz, s_shown.n);
        flush_region(&REGION_DF);
    }
    if (((dirty & UI_DIRTY_PEAK) != 0u) && (want.peak_hz != s_shown.peak_hz)) {
        s_shown.peak_hz = want.peak_hz;
        draw_peak(s_shown.peak_hz);
        flush_region(&REGION_PEAK);
    }
}
