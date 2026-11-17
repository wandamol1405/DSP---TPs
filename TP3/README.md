# TP3 - FFT

Realizar un programa que calcule la Transformada Rápida de Fourier (FFT) sobre buffers de 512, 1024 y 2048 muestras adquiridos con el laboratorio 1, teniendo en cuenta las frecuencias de muestreo de 8, 16, 22, 44 y 48 kHz. Con una de las teclas de la placa de evaluación FRDM-MCXN947 se habilita la FFT o se hace bypass hacia un buffer de salida distinto del buffer de entrada. El resultado se transmite por el puerto serie y el espectro se grafica en la PC. A partir de los resultados se sacan conclusiones sobre la FFT al cambiar la frecuencia de muestreo o el tamaño de la ventana.

## Qué hay en esta carpeta

| Ruta | Contenido |
| --- | --- |
| `MCXN947_Project_TP3/` | Proyecto de MCUXpresso con el firmware de la placa |
| `tools/` | Interfaz gráfica en Python para ver el espectro en la PC |
| `tests/` | Pruebas de la interfaz gráfica y simulador de la placa |
| `notebooks/` | Cuaderno de Jupyter sobre ventanas y fenómenos de la FFT |
| `images/` | Capturas y mediciones (incluye `ejemplo_senal_interna/`) |
| `decisiones.md` | Arquitectura, formato de la trama y decisiones de diseño |
| `ejemplo_senal_interna.md` | Ensayos y capturas de referencia usando el generador interno |

### Firmware (`MCXN947_Project_TP3/source/`)

| Archivo | Qué hace |
| --- | --- |
| `MCXN947_Project_TP3.c` | Programa principal: inicialización, loop principal, comandos por UART y pulsadores |
| `pipeline.c` / `.h` | Conecta las etapas: arma las ventanas de N muestras y dispara su procesamiento |
| `adc_stage.c` / `.h` | Adquisición con el ADC a la frecuencia de muestreo elegida |
| `processing_stage.c` / `.h` | FFT con CMSIS-DSP o bypass, hacia el buffer de salida |
| `uart_stage.c` / `.h` | Envío del buffer de salida a la PC y recepción de comandos |
| `dac_stage.c` / `.h` | Salida de la señal de entrada por el DAC, para verla en el osciloscopio |
| `signal_gen.c` / `.h` | Generador interno configurable (fuente, forma de onda y frecuencia por DDS) |
| `circular_buffer.c` / `.h` | Buffer circular de muestras de entrada |
| `ui.c` / `.h`, `display_port.c` / `.h`, `u8g2/` | Pantalla OLED |

### Programas de PC

| Archivo | Qué hace |
| --- | --- |
| `tools/fft_gui.py` | Interfaz gráfica: se conecta al puerto serie, grafica el espectro y la señal en el tiempo, y envía comandos a la placa |
| `tools/requirements.txt` | Dependencias de Python |
| `tests/fake_board.py` | Simulador de la placa, para probar la interfaz sin hardware |
| `tests/test_frame_parser.py` | Pruebas del lector de tramas de la interfaz |

## Conexiones

| Función | Conexión |
| --- | --- |
| Entrada analógica | ADC1, canal A23, pin P1_23 (J9[23]). La señal va montada sobre una continua de 1,65 V |
| Salida analógica | DAC0, pin P4_2 (J1[4]), al osciloscopio |
| Pantalla OLED | SSD1306 por I2C: SDA en P1_8 (J2[18]), SCL en P1_9 (J2[20]), 3,3 V y GND |
| Puerto serie | USB de depuración de la placa (MCU-Link), 115200 baudios, 8N1 |

## Cómo compilar y cargar el firmware

El proyecto se desarrolló con MCUXpresso IDE 25.6 y el SDK MCXN947 26.06.00.

1. En MCUXpresso, importar la carpeta `MCXN947_Project_TP3/` con *File > Import > Existing Projects into Workspace*.
2. Compilar el proyecto.
3. Conectar la placa por el USB de depuración y cargar el programa con la configuración *LinkServer Debug* o *LinkServer Release* que viene en el proyecto.

La configuración de pines, relojes y periféricos se cambia desde las herramientas de MCUXpresso. Los archivos generados no se editan a mano.

## Controles y comandos

Los pulsadores de la placa cambian la frecuencia de muestreo y el modo:

| Pulsador | Función |
| --- | --- |
| SW2 | Frecuencia de muestreo: 8, 16, 22, 44 y 48 kHz. El color del LED indica cuál está activa |
| SW3 | Modo: BYPASS, FFT 512, FFT 1024, FFT 2048 y vuelve a BYPASS |

Los mismos cambios y el resto de las funciones se manejan con comandos de una letra por el puerto serie:

| Comando | Función |
| --- | --- |
| `m` | Pasa al modo siguiente (igual que SW3) |
| `f` | Pasa a la frecuencia de muestreo siguiente (igual que SW2) |
| `r` | Alterna RUN y STOP de la adquisición |
| `c` | Activa o desactiva el envío continuo de tramas |
| `s` | Envía una sola trama |
| `g` | Cambia la fuente de señal: ADC -> DAC LOOPBACK (`J1[4] -> J9[23]`) -> SINTETICA |
| `k` | Cambia la forma de onda del generador interno: SENO -> CUADRADA -> TRIANGULAR -> SIERRA |
| `w` | Recorre presets de frecuencia del generador (`250, 1000, 1004, 3000, 5000 Hz`) |
| `+` / `-` | Ajusta la frecuencia del generador en `+1` / `-1 Hz` (`[` / `]` para `-10` / `+10 Hz`) |
| `<Hz>+Enter` | Fija una frecuencia arbitraria para el generador (`1 .. 50000 Hz`) |
| `t` | Muestra el estado y los contadores: tramas enviadas y descartadas, comandos perdidos y ventanas descartadas |
| `h` | Muestra la ayuda |

La placa atiende los comandos en todo momento, también con el envío continuo activo. Las respuestas son líneas de texto que salen entre dos tramas.

## Interfaz gráfica

Instalación, desde `TP3/tools/`:

```bash
python3 -m venv venv
venv/bin/pip install -r requirements.txt
```

En Linux puede hacer falta instalar además el paquete del sistema `python3-tk`.

Ejecución:

```bash
venv/bin/python3 fft_gui.py
```

La interfaz decodifica la trama mínima del firmware y envía los comandos soportados por la placa (`m`, `f`, `r`, `c`, `s`, `t`, `h` y los controles del generador interno `g`, `k`, `w`, `+`/`-`, `[`/`]`, `<Hz>`).

Para probarla con el simulador, en otra terminal:

```bash
venv/bin/python3 ../tests/fake_board.py
```

El simulador imprime el puerto que hay que escribir en el campo "Puerto" de la interfaz, por ejemplo `/dev/pts/4`.

Para correr las pruebas del lector de tramas:

```bash
venv/bin/python3 ../tests/test_frame_parser.py
```

## Cuaderno de Jupyter

[notebooks/fft_ventanas_y_fenomenos.ipynb](notebooks/fft_ventanas_y_fenomenos.ipynb) muestra con señales simuladas la resolución en frecuencia, el muestreo coherente, la fuga espectral, el efecto de la ventana (rectangular contra Hann), el aliasing y el piso de cálculo de la FFT en q15, y los compara con las capturas de la placa. Está guardado con los resultados, así que se puede leer en GitHub sin ejecutarlo.

Para ejecutarlo, desde `TP3/tools/`:

```bash
venv/bin/pip install jupyter
venv/bin/jupyter notebook ../notebooks/fft_ventanas_y_fenomenos.ipynb
```

## Más información

La arquitectura del firmware, el formato de la trama y el porqué de cada decisión están en [decisiones.md](decisiones.md). Los ensayos de referencia sin generador externo están documentados en [ejemplo_senal_interna.md](ejemplo_senal_interna.md).
