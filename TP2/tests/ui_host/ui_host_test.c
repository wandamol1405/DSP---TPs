/*
 * ui_host_test.c (test en PC, no forma parte del firmware)
 *
 * Compila ui.c SIN modificar contra U8g2 real y un simulador de SSD1306 (sim_display.c).
 * Verifica:
 *   - ui_init() dibuja la pantalla completa (1024 B) y display == framebuffer.
 *   - Cada ui_set_*() transmite exactamente las tiles de su región y nada más.
 *   - Valores repetidos / no pendientes no generan tráfico I2C.
 *   - Tras cualquier secuencia de actualizaciones parciales, el display es idéntico al de un
 *     redibujado completo del mismo estado (no quedan restos ni regiones sin actualizar).
 *   - UI deshabilitada si el display no responde.
 * Genera ui_screens.txt con una vista ASCII de cada filtro.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.c" // se incluye el .c para poder inspeccionar el estado estático
#include "sim_display.h"

int stub_irq_depth = 0;
static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

typedef struct {
    const ui_region_t *r;
} reg_ref_t;

static bool tile_in(const ui_region_t *r, int tx, int ty) {
    return tx >= r->tx && tx < r->tx + r->tw && ty >= r->ty && ty < r->ty + r->th;
}

static void dump(FILE *f, const char *title) {
    fprintf(f, "=== %s ===\n", title);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) {
            fputc(sim_pixel(x, y) ? '#' : '.', f);
        }
        fputc('\n', f);
    }
}

static void expect_display_eq_buffer(const char *what) {
    CHECK(memcmp(sim_gddram, u8g2_GetBufferPtr(&s_u8g2), 1024) == 0, "display != framebuffer tras %s", what);
}

// Comprueba que las tiles tocadas == unión de las regiones esperadas
static void expect_touched(const ui_region_t **regs, int n, const char *what) {
    for (int ty = 0; ty < 8; ty++) {
        for (int tx = 0; tx < 16; tx++) {
            bool exp = false;
            for (int i = 0; i < n; i++) {
                exp |= tile_in(regs[i], tx, ty);
            }
            CHECK(sim_touched[ty][tx] == (exp ? 1 : 0), "%s: tile (%d,%d) tocada=%d esperada=%d", what, tx, ty,
                  sim_touched[ty][tx], exp);
        }
    }
}

static void test_format(void) {
    struct {
        uint32_t hz;
        const char *s;
    } t[] = {{UI_FREQUENCY_NONE, "--"}, {0, "0 Hz"}, {50, "50 Hz"}, {999, "999 Hz"}, {1000, "1 kHz"},
             {8000, "8 kHz"},           {22050, "22.05 kHz"}, {44100, "44.1 kHz"}, {48000, "48 kHz"},
             {1767, "1.77 kHz"},        {3465, "3.47 kHz"},   {3600, "3.6 kHz"},   {99999000u, "99999 kHz"},
             {4000000000u, "99999 kHz"}, {123456, "123 kHz"}};
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char b[UI_VALUE_BUF_SIZE];
        format_frequency(b, t[i].hz);
        CHECK(strcmp(b, t[i].s) == 0, "format(%u)='%s' esperado '%s'", t[i].hz, b, t[i].s);
        CHECK(strlen(b) <= 9, "valor demasiado largo: '%s'", b);
    }
}

int main(void) {
    FILE *f = fopen("ui_screens.txt", "w");

    test_format();

    // ---- init: pantalla completa ----
    sim_reset_stats();
    CHECK(ui_init() == true, "ui_init");
    CHECK(sim_stats.data_bytes >= 1024, "init debe transmitir pantalla completa (%u B)", sim_stats.data_bytes);
    expect_display_eq_buffer("init");
    dump(f, "init (BYPASS, todo --)");

    // ---- sin cambios: ningún tráfico ----
    sim_reset_stats();
    ui_process();
    CHECK(sim_stats.transfers == 0, "ui_process sin cambios no debe comunicar");
    ui_set_sampling_frequency(UI_FREQUENCY_NONE); // mismo valor
    ui_set_filter_type(UI_FILTER_BYPASS);
    ui_set_filter_type((ui_filter_t)99); // inválido: ignorado
    ui_process();
    CHECK(sim_stats.transfers == 0, "valores repetidos/ inválidos no deben comunicar");

    // ---- cada parámetro actualiza solo su región ----
    const ui_region_t *r_fs[] = {&REGION_FS}, *r_fc[] = {&REGION_FC}, *r_bw[] = {&REGION_BW}, *r_fl[] = {&REGION_FILTER};

    sim_reset_stats();
    ui_set_sampling_frequency(48000);
    ui_process();
    expect_touched(r_fs, 1, "Fs");
    CHECK(sim_stats.data_bytes == 10 * 2 * 8, "Fs bytes=%u", sim_stats.data_bytes);
    expect_display_eq_buffer("Fs");

    sim_reset_stats();
    ui_set_cutoff_frequency(3600);
    ui_process();
    expect_touched(r_fc, 1, "Fc");
    expect_display_eq_buffer("Fc");

    sim_reset_stats();
    ui_set_bandwidth(15);
    ui_process();
    expect_touched(r_bw, 1, "BW");
    expect_display_eq_buffer("BW");

    sim_reset_stats();
    ui_set_filter_type(UI_FILTER_BANDPASS);
    ui_process();
    expect_touched(r_fl, 1, "Filtro");
    CHECK(sim_stats.data_bytes == 6 * 6 * 8, "Filtro bytes=%u", sim_stats.data_bytes);
    expect_display_eq_buffer("Filtro");

    // vistas de cada filtro
    const char *names[] = {"BYPASS", "LOWPASS", "HIGHPASS", "BANDPASS", "BANDSTOP"};
    ui_set_sampling_frequency(22050);
    ui_set_cutoff_frequency(1767);
    ui_set_bandwidth(3465);
    for (int i = 0; i < UI_FILTER_COUNT; i++) {
        ui_set_filter_type((ui_filter_t)i);
        ui_process();
        dump(f, names[i]);
    }

    // ---- actualización completa == reconstrucción ----
    sim_reset_stats();
    ui_show_screen();
    ui_process();
    CHECK(sim_stats.data_bytes == 1024, "full redraw bytes=%u", sim_stats.data_bytes);
    expect_display_eq_buffer("full");

    // ---- aleatorio: parciales vs. redibujado completo, y regiones exactas ----
    static const uint32_t fsv[] = {UI_FREQUENCY_NONE, 123, 8000, 16000, 22050, 44100, 48000, 99999000u};
    static const uint32_t fcv[] = {UI_FREQUENCY_NONE, 35, 50, 1767, 3465, 3600, 12345, 123456, 50000};
    unsigned seed = 12345;
#define RND() (seed = seed * 1664525u + 1013904223u, seed >> 8)
    ui_state_t model = s_shown;
    for (int it = 0; it < 3000; it++) {
        ui_state_t next = model;
        unsigned mask = RND() & 0xF;
        if (mask & 1) { next.fs_hz = fsv[RND() % 8]; ui_set_sampling_frequency(next.fs_hz); }
        if (mask & 2) { next.fc_hz = fcv[RND() % 9]; ui_set_cutoff_frequency(next.fc_hz); }
        if (mask & 4) { next.bw_hz = fcv[RND() % 9]; ui_set_bandwidth(next.bw_hz); }
        if (mask & 8) { next.filter = (ui_filter_t)(RND() % UI_FILTER_COUNT); ui_set_filter_type(next.filter); }

        const ui_region_t *regs[4];
        int n = 0;
        if (next.fs_hz != model.fs_hz) regs[n++] = &REGION_FS;
        if (next.fc_hz != model.fc_hz) regs[n++] = &REGION_FC;
        if (next.bw_hz != model.bw_hz) regs[n++] = &REGION_BW;
        if (next.filter != model.filter) regs[n++] = &REGION_FILTER;

        sim_reset_stats();
        ui_process();
        expect_touched(regs, n, "aleatorio");
        expect_display_eq_buffer("aleatorio");

        uint8_t partial[8][128];
        memcpy(partial, sim_gddram, sizeof(partial));
        ui_show_screen();
        ui_process();
        CHECK(memcmp(partial, sim_gddram, sizeof(partial)) == 0, "it=%d: parcial != redibujo completo", it);
        model = next;
        if (g_fail > 20) break;
    }
    CHECK(stub_irq_depth == 0, "secciones críticas desbalanceadas: %d", stub_irq_depth);

    // ---- display ausente ----
    sim_fail_init = true;
    CHECK(ui_init() == false, "ui_init debe fallar sin display");
    sim_reset_stats();
    ui_set_cutoff_frequency(1);
    ui_show_screen();
    ui_process();
    CHECK(sim_stats.transfers == 0, "UI deshabilitada no debe comunicar");

    fclose(f);
    printf("%d checks, %d fallos\n", g_checks, g_fail);
    return g_fail != 0;
}

