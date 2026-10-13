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
 * primeras 16 filas (tiles 0-1) y azul en las 48 restantes: el encabezado ocupa justo la franja
 * amarilla y nada atraviesa el límite y=16.
 *
 *   x:  0.....47 | 48..........127
 *   y0  +-----------------------+
 *       |      DSP FILTER       |  HEADER  (tiles 0,0  16x2)
 *   y16 +--------+--------------+
 *       | grafico|  Fs: ...     |  GRAPH   (tiles 0,2  6x4) | FS (tiles 6,2 10x2)
 *       |        |  Fc: ...     |                           | FC (tiles 6,4 10x2)
 *   y48 |  NOMBRE|  BW: ...     |  NAME    (tiles 0,6  6x2) | BW (tiles 6,6 10x2)
 *   y63 +--------+--------------+
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

static const ui_region_t REGION_HEADER = {0, 0, 16, 2};
static const ui_region_t REGION_GRAPH = {0, 2, 6, 4};
static const ui_region_t REGION_NAME = {0, 6, 6, 2};
static const ui_region_t REGION_FS = {6, 2, 10, 2};
static const ui_region_t REGION_FC = {6, 4, 10, 2};
static const ui_region_t REGION_BW = {6, 6, 10, 2};
// Filtro = gráfico + nombre: columnas iguales y filas contiguas, se transmiten en una sola región
static const ui_region_t REGION_FILTER = {0, 2, 6, 6};

#define TILE_PX 8u

// ============================================================================
// Fuentes y posiciones de texto (en píxeles)
// ============================================================================
#define FONT_HEADER u8g2_font_7x14_tr
#define FONT_NAME u8g2_font_5x8_tr
#define FONT_PARAM u8g2_font_6x12_tr

#define HEADER_TEXT "DSP FILTER"
#define HEADER_BASELINE_Y 12u // dentro de la franja amarilla (y < 16)
#define SEPARATOR_X 47u       // línea vertical entre la columna del filtro y la de parámetros
#define PARAM_LABEL_X 50u
#define PARAM_VALUE_X 70u
#define PARAM_BASELINE_OFFSET 12u // baseline respecto del borde superior de la región (16 px de alto)
#define NAME_BOX_X 2u
#define NAME_BOX_W 43u
#define NAME_BOX_Y 51u
#define NAME_BOX_H 11u
#define NAME_BASELINE_Y 59u

// Máximo valor mostrable: asegura que "99999 kHz" (9 caracteres de 6 px) entre en la región de valores
#define UI_MAX_DISPLAY_HZ 99999000u
#define UI_VALUE_BUF_SIZE 12u

// ============================================================================
// Gráficos de respuesta en frecuencia: polilíneas (sin bitmaps, cero bytes de assets)
// Área de trazado: x 8..42, nivel alto y=26, nivel bajo y=40; ejes en x=6 e y=44.
// ============================================================================
#define GX0 8u
#define GX1 42u
#define GY_HI 26u
#define GY_LO 40u
#define AXIS_X 6u
#define AXIS_Y 44u
#define AXIS_TOP_Y 22u

typedef struct {
    uint8_t x;
    uint8_t y;
} ui_point_t;

static const ui_point_t SHAPE_BYPASS[] = {{GX0, GY_HI}, {GX1, GY_HI}};
static const ui_point_t SHAPE_LOWPASS[] = {{GX0, GY_HI}, {24, GY_HI}, {32, GY_LO}, {GX1, GY_LO}};
static const ui_point_t SHAPE_HIGHPASS[] = {{GX0, GY_LO}, {18, GY_LO}, {26, GY_HI}, {GX1, GY_HI}};
static const ui_point_t SHAPE_BANDPASS[] = {{GX0, GY_LO}, {14, GY_LO}, {19, GY_HI}, {31, GY_HI}, {36, GY_LO}, {GX1, GY_LO}};
static const ui_point_t SHAPE_BANDSTOP[] = {{GX0, GY_HI}, {14, GY_HI}, {19, GY_LO}, {31, GY_LO}, {36, GY_HI}, {GX1, GY_HI}};

typedef struct {
    const char *name;
    const ui_point_t *shape;
    uint8_t shape_len;
} ui_filter_info_t;

#define SHAPE(arr) arr, (uint8_t)(sizeof(arr) / sizeof(arr[0]))

// Indexada por ui_filter_t
static const ui_filter_info_t FILTER_INFO[UI_FILTER_COUNT] = {
    [UI_FILTER_BYPASS] = {"BYPASS", SHAPE(SHAPE_BYPASS)},
    [UI_FILTER_LOWPASS] = {"LOWPASS", SHAPE(SHAPE_LOWPASS)},
    [UI_FILTER_HIGHPASS] = {"HIGHPASS", SHAPE(SHAPE_HIGHPASS)},
    [UI_FILTER_BANDPASS] = {"BANDPASS", SHAPE(SHAPE_BANDPASS)},
    [UI_FILTER_BANDSTOP] = {"BANDSTOP", SHAPE(SHAPE_BANDSTOP)},
};

// ============================================================================
// Estado
// ============================================================================
typedef struct {
    uint32_t fs_hz;
    uint32_t fc_hz;
    uint32_t bw_hz;
    ui_filter_t filter;
} ui_state_t;

// Bits de s_dirty: qué parámetros cambiaron desde el último ui_process()
#define UI_DIRTY_FS (1u << 0)
#define UI_DIRTY_FC (1u << 1)
#define UI_DIRTY_BW (1u << 2)
#define UI_DIRTY_FILTER (1u << 3)
#define UI_DIRTY_FULL (1u << 4)

static u8g2_t s_u8g2; // instancia de U8g2 (incluye el framebuffer de 1024 B)
static bool s_enabled = false; // true una vez que el display inicializó correctamente

// Estado pedido por la aplicación. Lo escriben los ui_set_*() (posiblemente desde ISR) y lo lee
// ui_process(); todo acceso es dentro de una sección crítica.
static ui_state_t s_pending = {UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_FILTER_BYPASS};
static uint8_t s_dirty = 0;

// Estado que realmente está dibujado en el display. Solo lo toca el contexto principal.
static ui_state_t s_shown = {UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_FILTER_BYPASS};

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

// Formatea una frecuencia: "--", "440 Hz", "8 kHz", "22.05 kHz", "44.1 kHz" (máx. 9 caracteres).
static void format_frequency(char *buf, uint32_t hz) {
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

static void draw_header(void) {
    clear_region(&REGION_HEADER);
    u8g2_SetFontMode(&s_u8g2, 1); // fuente transparente
    u8g2_SetFontDirection(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, FONT_HEADER);
    u8g2_uint_t w = u8g2_GetStrWidth(&s_u8g2, HEADER_TEXT);
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)((128u - w) / 2u), HEADER_BASELINE_Y, HEADER_TEXT);
    u8g2_DrawHLine(&s_u8g2, 0, 15, 128); // separador, última fila de la franja amarilla
}

static void draw_filter_graph(ui_filter_t type) {
    const ui_filter_info_t *info = &FILTER_INFO[type];

    clear_region(&REGION_GRAPH);
    u8g2_DrawVLine(&s_u8g2, SEPARATOR_X, REGION_GRAPH.ty * TILE_PX, REGION_GRAPH.th * TILE_PX);

    // Ejes
    u8g2_DrawVLine(&s_u8g2, AXIS_X, AXIS_TOP_Y, AXIS_Y - AXIS_TOP_Y + 1u);
    u8g2_DrawHLine(&s_u8g2, AXIS_X, AXIS_Y, GX1 - AXIS_X + 1u);

    // Curva de 2 px de espesor
    for (uint8_t i = 0; (i + 1u) < info->shape_len; i++) {
        const ui_point_t *a = &info->shape[i];
        const ui_point_t *b = &info->shape[i + 1u];
        u8g2_DrawLine(&s_u8g2, a->x, a->y, b->x, b->y);
        u8g2_DrawLine(&s_u8g2, a->x, (u8g2_uint_t)(a->y + 1u), b->x, (u8g2_uint_t)(b->y + 1u));
    }
}

static void draw_filter_name(ui_filter_t type) {
    const char *name = FILTER_INFO[type].name;

    clear_region(&REGION_NAME);
    u8g2_DrawVLine(&s_u8g2, SEPARATOR_X, REGION_NAME.ty * TILE_PX, REGION_NAME.th * TILE_PX);

    // Etiqueta en video inverso, centrada
    u8g2_DrawBox(&s_u8g2, NAME_BOX_X, NAME_BOX_Y, NAME_BOX_W, NAME_BOX_H);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetFontDirection(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, FONT_NAME);
    u8g2_uint_t w = u8g2_GetStrWidth(&s_u8g2, name);
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)(NAME_BOX_X + (NAME_BOX_W - w) / 2u), NAME_BASELINE_Y, name);
    u8g2_SetDrawColor(&s_u8g2, 1);
}

// Dibuja una fila "Etiqueta: valor" en su región (borra primero para no dejar restos de caracteres)
static void draw_param(const ui_region_t *r, const char *label, uint32_t hz) {
    char value[UI_VALUE_BUF_SIZE];
    u8g2_uint_t baseline = (u8g2_uint_t)(r->ty * TILE_PX + PARAM_BASELINE_OFFSET);

    format_frequency(value, hz);

    clear_region(r);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetFontDirection(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, FONT_PARAM);
    u8g2_DrawStr(&s_u8g2, PARAM_LABEL_X, baseline, label);
    u8g2_DrawStr(&s_u8g2, PARAM_VALUE_X, baseline, value);
}

// Reconstruye todo el framebuffer a partir de un estado y lo transmite completo
static void draw_full(const ui_state_t *st) {
    u8g2_ClearBuffer(&s_u8g2);
    draw_header();
    draw_filter_graph(st->filter);
    draw_filter_name(st->filter);
    draw_param(&REGION_FS, "Fs:", st->fs_hz);
    draw_param(&REGION_FC, "Fc:", st->fc_hz);
    draw_param(&REGION_BW, "BW:", st->bw_hz);
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
    s_shown = (ui_state_t){UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_FREQUENCY_NONE, UI_FILTER_BYPASS};
    draw_full(&s_shown);

    s_enabled = true;
    return true;
}

void ui_show_screen(void) {
    uint32_t primask = DisableGlobalIRQ();
    s_dirty |= UI_DIRTY_FULL;
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

void ui_set_filter_type(ui_filter_t type) {
    if ((unsigned int)type >= (unsigned int)UI_FILTER_COUNT) {
        return;
    }
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.filter != type) {
        s_pending.filter = type;
        s_dirty |= UI_DIRTY_FILTER;
    }
    EnableGlobalIRQ(primask);
}

void ui_set_cutoff_frequency(uint32_t hz) {
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.fc_hz != hz) {
        s_pending.fc_hz = hz;
        s_dirty |= UI_DIRTY_FC;
    }
    EnableGlobalIRQ(primask);
}

void ui_set_bandwidth(uint32_t hz) {
    uint32_t primask = DisableGlobalIRQ();
    if (s_pending.bw_hz != hz) {
        s_pending.bw_hz = hz;
        s_dirty |= UI_DIRTY_BW;
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

    // Cada parámetro actualiza solo su región, y solo si el valor mostrado realmente difiere
    if (((dirty & UI_DIRTY_FILTER) != 0u) && (want.filter != s_shown.filter)) {
        s_shown.filter = want.filter;
        draw_filter_graph(s_shown.filter);
        draw_filter_name(s_shown.filter);
        flush_region(&REGION_FILTER);
    }
    if (((dirty & UI_DIRTY_FS) != 0u) && (want.fs_hz != s_shown.fs_hz)) {
        s_shown.fs_hz = want.fs_hz;
        draw_param(&REGION_FS, "Fs:", s_shown.fs_hz);
        flush_region(&REGION_FS);
    }
    if (((dirty & UI_DIRTY_FC) != 0u) && (want.fc_hz != s_shown.fc_hz)) {
        s_shown.fc_hz = want.fc_hz;
        draw_param(&REGION_FC, "Fc:", s_shown.fc_hz);
        flush_region(&REGION_FC);
    }
    if (((dirty & UI_DIRTY_BW) != 0u) && (want.bw_hz != s_shown.bw_hz)) {
        s_shown.bw_hz = want.bw_hz;
        draw_param(&REGION_BW, "BW:", s_shown.bw_hz);
        flush_region(&REGION_BW);
    }
}
