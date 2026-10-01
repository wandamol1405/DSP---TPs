# TP2 - Filtros FIR

Realizar un programa aplicativo que sea capaz de aplicar un filtro FIR, por muestras a un buffer de memoria de 512 muestras que es dquirido con el laboratorio 1 teniendo en cuenta las frecuencias  de muestreo de 8, 16, 22, 44 y 48KHz. Con una de las teclas de la placa de evaluacion FRDM-K64F, se habilitará la aplicación del filtroo se hace bypass del filtro a un buffer de salida distinto del buffer de entrada. De este buffer de salida se enviarán las muestras al DAC de la placa FRDM-K64F. Visualizar el resultado utilizando un osciloscopio y comparar los resultado.

### Requisitos de los filtros:
Pasa bajos Fc = 3600 Hz Astop = 30 db
Pasa altos Fc = 35 Hz Astop = 30 db
Pasa Banda Fc = 35 Hz Fc2 = 3500 Hz Astop = 30 db
ELimina Banda Fr = 50Hz Bw = 15 Hz Astop = 25 db
