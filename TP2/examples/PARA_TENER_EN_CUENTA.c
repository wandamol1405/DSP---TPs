/*INCLUDES*/
#include "MK64F12.h"
#include "arm_math.h"
#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "peripherals.h"
#include "pin_mux.h"
#include <stdio.h>
/*Podemos hacer esto para codigo mas limpio*/
#include "FiltrosTP2.h"
/**Prototipo de funciones**/
Void init_filtros(void);

/*Definimos las ctes, como por ejemplo*/
#define buffer_size 512
#define TEST_LENGTH_SAMPLES 1
#define BLOCK_SIZE 1
#define NUM_TAPS_X                                                             \
  X(número del filtro a considerar esto se puede definir en FiltrosTP2.h)

/*Definimos las variables globales, algunas pueden ser del tipo “volatile” o
 * “static” si así lo consideran*/

/* Importante las instancias*/

arm_fir_instance_q15
    arreglo_filtros[5][4]; // Podríamos hacer esto para todos los FIR
arm_fir_instance_q15 *pte_aux;

arm_status status; // Se usa dependiendo del CMSIS inicilaizar el FIR

uint32_t blockSize = BLOCK_SIZE;

int main(void) {
  // inicializacion punteros

  /* Init board hardware. */
  BOARD_InitBootPins();
  BOARD_InitBootClocks();
  BOARD_InitBootPeripherals();

  BOARD_InitButtonsPins();
  BOARD_InitLEDsPins();
  BOARD_InitPins();

  init_filtros(); // Inicializamos los filtros
  p_buffer_out = buffer_in;
  pte_aux = &arreglo_filtros[fmuest][filtroTipo];

  adc16_channel_config_t
      adc16_ConfigStruct; // Estructura que es necesaria para iniciar ADC
  adc16_ConfigStruct.channelNumber = 12;
  adc16_ConfigStruct.enableInterruptOnConversionCompleted = false;
  adc16_ConfigStruct.enableDifferentialConversion = false;

  // ETC Y TODO LO QUE SEA NECESARIO

#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
  /* Init FSL debug console. */
  BOARD_InitDebugConsole();
#endif
  /*inicio */

  PRINTF("inicio\n");
  while (1) {
  }

  return 0;
}

/*******Funciones*******/

void init_filtros(void) {

  /* EJ: inicialización filtro Pasa BAJOS, para frec. 8k */

  q15_t pStatePB_8k[BLOCK_SIZE + NUM_TAPS_X]; // segun filtro

  arm_fir_init_q15(&arreglo_filtros[K8][PB], NUM_TAPS_X, PB3k5_8k, pStatePB_8k,
                   BLOCK_SIZE);

  // en algunos CMSIS se tiene que inicializar de la siguiente manera:

  Status = arm_fir_init_q15(&arreglo_filtros[K8][PB], NUM_TAPS_X, PB3k5_8k,
                            pStatePB_8k, BLOCK_SIZE);

  // arreglo_filtros[K8][PB] se puede armar una instancia donde cada índice es
  // un tipo de filtro a considerar.

  // Asi con todos!!
}

// PIT
void PIT_CHANNEL_0_IRQHANDLER(void) {
  static uint32_t intStatus;

  /* Reading all interrupt flags of status register */
  intStatus = PIT_GetStatusFlags(PIT_PERIPHERAL, PIT_CHANNEL_0);

  // SE ALMACENA EL DATO RECIBIDO POR EL ADC,  SE DESPLAZA Y TRANSFORMADO EN
  // PUNTO FIJO

  adc_value = ADC16_GetChannelConversionValue(ADC16_1_PERIPHERAL, 0U);
  adc_value_fix = (int16_t)((adc_value >> 1) & 0x07FFF);

  buffer[index_buffer] = (q15_t)adc_value_fix;

  // FILTRADO
  if (f_filtrar) {

    arm_fir_q15(pte_aux, &buffer[0] + index_buffer,
                &buffer_out[0] + index_buffer, blockSize);

    // 		Tener en cuenta las declaraciones de cada variable

    prueba = buffer_out[index_buffer] + offset;
    dac_value = (prueba >> 3) & 0x0FFF;
    dac_value_fix = dac_value;
  }

  else {
    dac_value = adc_value_fix;
    dac_value_fix = (dac_value >> 3) & 0x0FFF;
  }

  DAC_SetBufferValue(DAC_1_PERIPHERAL, 0U, dac_value_fix);
  index_buffer++;

  if (index_buffer == buffer_size) {
    index_buffer = 0;
  }

  // Limpia la bandera de interrupción
  PIT_ClearStatusFlags(PIT_PERIPHERAL, PIT_CHANNEL_0, intStatus);

#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}

// BOTON 1
void GPIOA_IRQHANDLER(void) {
  /* Get pin flags */
  uint32_t pin_flags = GPIO_PortGetInterruptFlags(GPIOA);

  /* Place your interrupt code here */
  if (pin_flags & (1 << BOARD_SW3_GPIO_PIN)) {

    f_filtrar ^=
        1; // togglea la variable convertir cada vez que se preciona SW3
  }
}
/* Clear pin flags */
GPIO_PortClearInterruptFlags(GPIOA, pin_flags);

#if defined __CORTEX_M && (__CORTEX_M == 4U)
__DSB();
#endif
}

// Segundo Boton
void GPIOC_IRQHANDLER(void) {

  // se verifica debido a que pin se produjo la interrupción
  uint32_t pin_flags = GPIO_PortGetInterruptFlags(GPIOC);
  if (pin_flags & (1 << BOARD_SW2_GPIO_PIN)) {

    // DEFINIR LA LOGICA PARA ELEGIR: el tipo de filtro con “filtroTipo”
    //  Y CAMBIAR RECUENCIA DE MUESTREO. Pueden ser arreglos circulares también.

    pte_aux = &arreglo_filtros[fmuest][filtroTipo]; // se establece a que filtro
                                                    // debe apuntar el filtro

    PIT_SetTimerPeriod(
        PIT_PERIPHERAL, PIT_CHANNEL_0,
        periodos[fmuest]); // se establece el periodo de interrupcion del timer

    // El switch se utiliza para establecer el color del LED para cada tipo de
    // filtro
    switch (filtroTipo) {
    case 0:       // led en rojo PASA BAJOS
      offset = 0; // 32718;
      GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_PIN, 1);
      GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_PIN, 1);
      GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_PIN, 0);
      break;
    case 1: // led lila PASA ALTOS
      offset = 16384;
      GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_PIN, 0);
      GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_PIN, 1);
      GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_PIN, 0);
      break;
    case 2: // led blanco PASA BANDA
      offset = 16384;
      GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_PIN, 0);
      GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_PIN, 0);
      GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_PIN, 0);

      break;
    case 3:       // led amarillo RECHAZA BANDA
      offset = 0; // 32718;
      GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_PIN, 1);
      GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_PIN, 0);
      GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_PIN, 0);
      break;
    case 4: // led verde
      GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_PIN, 1);
      GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_PIN, 0);
      GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_PIN, 1);
      break;
    default:
      break;
    }
  }

  GPIO_PortClearInterruptFlags(GPIOC,
                               (1 << BOARD_SW2_GPIO_PIN)); // Clear pin flags

#if defined __CORTEX_M && (__CORTEX_M == 4U)
  __DSB();
#endif
}