/*
 * sim_display.h (solo test en PC)
 *
 * Simulador de un SSD1306 128x64 por I2C en modo de direccionamiento por páginas, que cuenta
 * qué se transmitió. Reemplaza al backend display_port.c en el test de host: el módulo ui.c
 * se compila sin modificaciones y U8g2 genera el tráfico I2C real, que acá se interpreta.
 */
#ifndef SIM_DISPLAY_H_
#define SIM_DISPLAY_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    unsigned transfers;      // transacciones I2C (START..STOP)
    unsigned data_bytes;     // bytes de datos de pixel escritos en GDDRAM
    unsigned data_transfers; // transacciones de datos
} sim_stats_t;

extern uint8_t sim_gddram[8][128]; // [página][columna], bit n = fila 8*página + n
extern uint8_t sim_touched[8][16]; // tiles (8x8) escritas desde el último sim_reset_stats()
extern sim_stats_t sim_stats;
extern bool sim_fail_init; // true: display_port_init() simula un display ausente

void sim_reset_stats(void);
bool sim_pixel(int x, int y);

#endif /* SIM_DISPLAY_H_ */

