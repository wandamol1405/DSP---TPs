# TP1 — Muestreo con MCXN947

## Objetivo

Desarrollar una aplicación para la placa **FRDM-MCXN947** capaz de adquirir una señal analógica mediante el conversor A/D integrado del microcontrolador **MCXN947**, trabajando a distintas frecuencias de muestreo.

Las frecuencias requeridas son:

* 8 kS/s
* 16 kS/s
* 22 kS/s
* 44 kS/s
* 48 kS/s

La aplicación deberá almacenar las muestras adquiridas en un **buffer circular de 512 muestras en formato Q15** y enviar los valores adquiridos al **DAC de 12 bits**.

La frecuencia de muestreo se seleccionará mediante una tecla de la placa y se indicará mediante el LED RGB. Otra tecla permitirá controlar el estado de adquisición mediante una función **Run/Stop**.

---

## Requerimientos

### Adquisición

* Utilizar el ADC integrado del MCXN947.
* Adquirir una muestra a la frecuencia de muestreo seleccionada.
* Soportar las siguientes frecuencias:

  * 8 kS/s
  * 16 kS/s
  * 22 kS/s
  * 44 kS/s
  * 48 kS/s

### Almacenamiento

* Utilizar un buffer circular de **512 muestras**.
* Representar las muestras mediante el formato **Q15**.
* Mantener el índice de escritura de forma circular.

### Salida

* Enviar las muestras adquiridas al **DAC de 12 bits**.
* Mantener la frecuencia de actualización correspondiente a la frecuencia de muestreo seleccionada.

### Interfaz

* Una tecla permitirá cambiar cíclicamente la frecuencia de muestreo.
* El LED RGB indicará la frecuencia seleccionada.
* Otra tecla permitirá alternar entre:

  * `RUN`: adquisición activa.
  * `STOP`: adquisición detenida.

---

## Plan de implementación

El desarrollo se dividirá en etapas para poder verificar cada componente antes de integrar el sistema completo.

### 1. Configuración inicial del proyecto

Configurar el proyecto para la **FRDM-MCXN947** y verificar:

* Clock configuration.
* GPIO.
* Teclas de la placa.
* LED RGB.
* ADC.
* Timer/peripheral trigger.
* DAC.

En esta etapa se documentarán las configuraciones relevantes realizadas mediante MCUXpresso.

### 2. Prueba del ADC

Implementar una primera versión capaz de:

```text
Entrada analógica
       │
       ▼
      ADC
       │
       ▼
 Lectura de muestra
```

Verificar que el ADC funciona correctamente antes de introducir la temporización y el almacenamiento.

### 3. Generación de la frecuencia de muestreo

Implementar el mecanismo que determine cuándo debe realizarse una nueva adquisición.

Se evaluará el uso de un **timer como fuente de trigger del ADC**, buscando que la frecuencia de muestreo sea independiente de la ejecución del código principal.

Primero se verificará una frecuencia de referencia y posteriormente:

```text
8 kS/s
16 kS/s
22 kS/s
44 kS/s
48 kS/s
```

### 4. Conversión a Q15 y buffer circular

Agregar el almacenamiento de las muestras:

```text
ADC
 │
 ▼
Conversión a Q15
 │
 ▼
Buffer circular
512 muestras
```

El buffer deberá reutilizar automáticamente las posiciones una vez alcanzado su final.

### 5. Salida mediante DAC

Integrar el DAC:

```text
             ┌──► Buffer circular
             │
ADC ─► Q15 ──┤
             │
             └──► DAC
```

Verificar que la señal adquirida pueda observarse en la salida del DAC.

### 6. Control Run/Stop

Incorporar la tecla de control de adquisición.

El estado `RUN` permitirá adquirir muestras normalmente, mientras que `STOP` detendrá la adquisición.

### 7. Selector de frecuencia y LED RGB

Implementar el cambio cíclico de frecuencia:

```text
8 kHz → 16 kHz → 22 kHz → 44 kHz → 48 kHz → 8 kHz
```

Asociar cada frecuencia a un estado del LED RGB.

### 8. Integración y pruebas

Integrar todos los componentes y verificar:

* Correcta adquisición de la señal.
* Correcta frecuencia de muestreo.
* Funcionamiento del buffer circular.
* Conversión ADC → Q15.
* Conversión Q15 → DAC.
* Funcionamiento de Run/Stop.
* Cambio de frecuencia.
* Indicación mediante LED RGB.
* Funcionamiento estable a 48 kS/s.

---

## Arquitectura prevista

La arquitectura inicial de la aplicación será:

```text
                         ┌───────────────┐
                         │     Timer     │
                         └───────┬───────┘
                                 │
                              Trigger
                                 │
                                 ▼
┌──────────────┐          ┌───────────────┐
│    Señal     │─────────►│      ADC      │
│   analógica  │          └───────┬───────┘
└──────────────┘                  │
                                  ▼
                           ┌───────────────┐
                           │   ADC → Q15   │
                           └───────┬───────┘
                                   │
                     ┌─────────────┴─────────────┐
                     │                           │
                     ▼                           ▼
             ┌───────────────┐           ┌───────────────┐
             │    Buffer     │           │      DAC      │
             │ circular 512  │           │    12 bits    │
             └───────────────┘           └───────────────┘


       ┌──────────────┐
       │    Tecla     │──────► Run / Stop
       └──────────────┘

       ┌──────────────┐
       │    Tecla     │──────► Selector de frecuencia
       └──────────────┘                    │
                                           ├──► Timer
                                           └──► LED RGB
```

*Este diagrama representa la arquitectura prevista inicialmente. Se actualizará durante la implementación si la configuración real de los periféricos requiere modificar el flujo.*

---

## Organización del trabajo

El desarrollo se realizará sobre un único proyecto de MCUXpresso compartido entre los cuatro integrantes.

Para evitar conflictos:

* Cada integrante trabajará preferentemente en una rama propia.
* Los cambios deberán realizarse mediante commits pequeños y descriptivos.
* Las modificaciones importantes de configuración de periféricos deberán documentarse en el commit correspondiente.
* La integración de las ramas se realizará después de comprobar que el proyecto continúa compilando.
* Se evitará que varias personas modifiquen simultáneamente los mismos archivos de configuración.

La división definitiva de tareas se establecerá una vez analizada la estructura generada por MCUXpresso.

---

## Estructura del proyecto

El repositorio contendrá tanto el código desarrollado manualmente como los archivos generados por MCUXpresso necesarios para reconstruir el proyecto.

Se deberá conservar especialmente:

* Configuración del proyecto.
* Configuración de clocks.
* Configuración de periféricos.
* Archivos de configuración generados por MCUXpresso.
* Código fuente.
* Archivos de cabecera.
* Configuración de la placa y del SDK utilizada por el proyecto.

*La lista exacta de archivos a versionar se definirá después de crear el proyecto inicial y revisar qué archivos genera MCUXpresso.*

Los archivos temporales, resultados de compilación y configuraciones específicas del entorno local deberán excluirse mediante `.gitignore`.

---

## Pruebas previstas

| Prueba                 | Resultado esperado                             |
| ---------------------- | ---------------------------------------------- |
| Inicialización del ADC | El ADC funciona correctamente                  |
| Adquisición a 8 kS/s   | Frecuencia de muestreo correcta                |
| Adquisición a 16 kS/s  | Frecuencia de muestreo correcta                |
| Adquisición a 22 kS/s  | Frecuencia de muestreo correcta                |
| Adquisición a 44 kS/s  | Frecuencia de muestreo correcta                |
| Adquisición a 48 kS/s  | Frecuencia de muestreo correcta                |
| Buffer circular        | Las 512 posiciones se reutilizan correctamente |
| DAC                    | La señal adquirida se reproduce en la salida   |
| Run/Stop               | La adquisición puede iniciarse y detenerse     |
| Selector de frecuencia | Las cinco frecuencias pueden seleccionarse     |
| LED RGB                | Indica la frecuencia seleccionada              |

---

## Documentación final

El informe deberá incluir:

1. **Configuración del ambiente**

   * Configuración del MCU.
   * Clocks.
   * ADC.
   * Timer.
   * DAC.
   * GPIO.
   * Otros periféricos utilizados.

2. **Diagrama de diseño**

   * Bloques funcionales.
   * Flujo de datos.
   * Control de la aplicación.
   * Relación entre ADC, buffer, DAC y temporización.

3. **Código fuente**

   * Proyecto completo de MCUXpresso.
   * Código desarrollado.
   * Configuraciones necesarias para reproducir el proyecto.

4. **Demostración**

   * Funcionamiento de las distintas frecuencias.
   * Run/Stop.
   * Adquisición y salida mediante DAC.
