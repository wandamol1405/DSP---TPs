# TP2 - Filtros FIR

Realizar un programa aplicativo que sea capaz de aplicar un filtro FIR, por muestras a un buffer de memoria de 512 muestras que es adquirido con el laboratorio 1 teniendo en cuenta las frecuencias de muestreo de 8, 16, 22, 44 y 48KHz. Con una de las teclas de la placa de evaluación FRDM-MCXN947, se habilitará la aplicación del filtro o se hace bypass del filtro a un buffer de salida distinto del buffer de entrada. De este buffer de salida se enviarán las muestras al DAC de la placa FRDM-MCXN947. Visualizar el resultado utilizando un osciloscopio y comparar los resultados.

### Requisitos de los filtros:

- Pasa bajos Fc = 3600 Hz Astop = 30 dB
- Pasa altos Fc = 35 Hz Astop = 30 dB
- Pasa Banda Fc = 35 Hz Fc2 = 3500 Hz Astop = 30 dB
- Elimina Banda Fr = 50 Hz Bw = 15 Hz Astop = 25 dB
