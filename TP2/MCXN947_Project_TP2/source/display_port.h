/*
 * display_port.h
 *
 * Backend de visualización: adapta U8g2 al LPI2C3 (LP_FLEXCOMM3) de la FRDM-MCXN947
 * para controlar un OLED SSD1306 de 128x64 por I2C.
 *
 * Adaptado del ejemplo MCXN947_U8g2_OLED (display_port.c/.h). Cambios respecto del ejemplo:
 *   - Nombres de funciones según la convención de este repositorio (snake_case).
 *   - Se configura U8g2 en modo FULL BUFFER (_f, 1024 B) en vez de page buffer de 2 páginas (_2).
 *     Con page buffer no existe la posibilidad de enviar solo una región (cada página se
 *     vuelve a dibujar y a transmitir en cada ciclo FirstPage/NextPage); con full buffer
 *     u8g2_UpdateDisplayArea() puede transmitir únicamente las tiles modificadas.
 *
 * Este módulo es el único que conoce el bus I2C y el controlador SSD1306.
 * La inicialización de pines, reloj y LPI2C3 la hacen BOARD_InitBootPins(),
 * BOARD_InitBootClocks() y BOARD_InitBootPeripherals() (archivos generados por MCUXpresso).
 */

#ifndef DISPLAY_PORT_H_
#define DISPLAY_PORT_H_

#include <stdint.h>
#include <stdbool.h>
#include "fsl_common.h" // status_t
#include "u8g2.h"       // u8g2_t, u8x8_t

/**
 * @brief Dirección I2C de 7 bits del SSD1306 (0x3C es la habitual en módulos de 0.96").
 *        Algunos módulos con resistencia de selección en el reverso usan 0x3D.
 */
#define DISPLAY_I2C_ADDR_7BIT (0x3CU)

/**
 * @brief Callback de U8x8 para la comunicación por bytes sobre LPI2C3 (polling, bloqueante).
 */
uint8_t u8x8_byte_lpi2c_mcxn947(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

/**
 * @brief Callback de U8x8 para demoras y GPIO (usa SDK_DelayAtLeastUs).
 */
uint8_t u8x8_gpio_and_delay_mcxn947(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

/**
 * @brief Configura U8g2 (SSD1306 128x64, I2C, full buffer), envía la secuencia de
 *        inicialización del controlador y lo saca del modo de ahorro de energía.
 * @param u8g2 Instancia de u8g2_t provista por quien llama (debe persistir).
 * @return true si el display respondió; false si hubo un error de bus (por ejemplo NACK).
 * @note Bloqueante (I2C por polling). No llamar desde una ISR.
 */
bool display_port_init(u8g2_t *u8g2);

/**
 * @brief Último estado reportado por el driver LPI2C durante las transferencias al display.
 */
status_t display_port_get_last_error(void);

#endif /* DISPLAY_PORT_H_ */

