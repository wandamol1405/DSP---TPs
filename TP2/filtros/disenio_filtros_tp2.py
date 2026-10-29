"""
disenio_filtros_tp2.py

TP2 - Diseño de los filtros FIR por el método de ventaneo (Kaiser), en
Python. Calcula los coeficientes de los 4 filtros de la consigna (pasa
bajos, pasa altos, pasa banda, elimina banda) en las 5 frecuencias de
muestreo del TP (4 x 5 = 20 diseños), verifica su respuesta en frecuencia
sobre los coeficientes Q15 (los que se exportan), y escribe
filter_coeffs.h listo para CMSIS-DSP (arm_fir_init_q15 / arm_fir_q15).

Requiere numpy y scipy (`pip install numpy scipy`); matplotlib solo para
las gráficas (`pip install matplotlib`).

Todo gira alrededor de un único bloque parametrizable:

    disenar_fir_kaiser(ftype, cutoff, astop_db, fs, df, numtaps=None)

  ftype    : 'lowpass' | 'highpass' | 'bandpass' | 'bandstop'
  cutoff   : corte en Hz (lowpass/highpass) o [f1, f2] (bandpass/bandstop)
  astop_db : atenuación pedida en la banda de rechazo, en dB
  fs       : frecuencia de muestreo en Hz
  df       : ancho de banda de transición en Hz (escalar o uno por borde)
  numtaps  : cantidad de coeficientes. Si se omite, se calcula
             automáticamente para cumplir astop_db; si se da, se fuerza
             esa longitud y la atenuación lograda queda como dato de salida
             (puede no alcanzar astop_db si numtaps es chico).

Para elimina banda, cutoff_bandstop_fr_bw(fr, bw, df) convierte una
especificación por frecuencia de rechazo (Fr) y ancho de banda (Bw) a los
dos cortes -6 dB que espera disenar_fir_kaiser.

Los parámetros de los 4 filtros del TP (Fc/Fr/Bw/Astop/df) y las 5
frecuencias de muestreo están en main(), al final del archivo.
"""
from __future__ import annotations

import os
import numpy as np
from scipy.signal import firwin, remez, kaiser_beta as _kaiser_beta

# NFFT de verificación: Fs/NFFT de resolución. 2**18 alcanza de sobra para
# resolver una transición de 20 Hz incluso a 48 kHz (~0.18 Hz/bin).
NFFT_DEFAULT = 2 ** 18


# =============================================================================
# Bloques básicos
# =============================================================================

def kaiser_beta(astop_db):
    """Beta de Kaiser a partir de la atenuación pedida (depende solo de A,
    no de numtaps ni del ancho de transición -- ver scipy.signal.kaiserord)."""
    return _kaiser_beta(astop_db)


def zonas_paso_rechazo(fs, cutoff, df, pass_zero):
    """
    Arma las zonas [desde, hasta] (Hz) de paso/rechazo a partir de los
    cortes (-6 dB) y el ancho de transición de cada uno. Alterna
    paso/rechazo empezando según pass_zero (True = la primera zona, desde
    0 Hz, es de paso). cutoff y df pueden ser escalares o uno por borde
    (pasabanda/elimina banda).

    fs      : frecuencia de muestreo en Hz
    cutoff  : corte en Hz (lowpass/highpass) o [f1, f2] (bandpass/bandstop)
    df      : ancho de transición en Hz (escalar o uno por borde)
    pass_zero: True si la primera zona (desde 0 Hz) es de paso, False si es de rechazo

    Devuelve (zonas, es_paso): zonas es una lista de (lo, hi) y es_paso
    una lista paralela de booleanos.
    """
    # atleast_1d admite pasar un escalar (lowpass/highpass, un solo corte) o
    # una lista (bandpass/bandstop, dos cortes) con el mismo código.
    cutoff_arr = np.atleast_1d(cutoff).astype(float)  # p.ej. array([3600.]) o array([35., 3500.])
    # broadcast_to repite df si vino como escalar, para tener un ancho de
    # transición por cada corte aunque el que llama haya pasado uno solo
    # (p.ej. df=400 -> array([400., 400.]) si cutoff tiene 2 elementos).
    df_arr = np.broadcast_to(np.atleast_1d(df).astype(float), cutoff_arr.shape)
    # Por si cutoff no viniera ya ordenado de menor a mayor (no debería
    # pasar en este TP, pero lo hace robusto igual).
    orden = np.argsort(cutoff_arr)
    c, d = cutoff_arr[orden], df_arr[orden]

    # Arma la lista plana de "puntos de quiebre" del eje de frecuencia:
    # [0, Fpass1, Fstop1, Fpass2, Fstop2, ..., Fs/2]
    # donde Fpass_i = c[i] - d[i]/2 y Fstop_i = c[i] + d[i]/2 son los bordes
    # de la banda de transición alrededor de cada corte -6 dB. Ejemplo para
    # un pasabajos con un solo corte en 3600 Hz y df=400 a Fs=8000:
    #   puntos = [0, 3400, 3800, 4000]  (Fpass=3400, Fstop=3800, Nyquist=4000)
    puntos = [0.0]
    for ci, di in zip(c, d):
        puntos += [ci - di / 2, ci + di / 2]
    puntos.append(fs / 2)

    # Empareja esos puntos de a dos consecutivos para formar las zonas:
    # puntos[0::2] = [0, 3800]  (arranques de zona, salteando de a 2)
    # puntos[1::2] = [3400, 4000]  (finales de zona, salteando de a 2, corridos 1)
    # zip da [(0, 3400), (3800, 4000)]: la zona de paso [0, 3400] y la de
    # rechazo [3800, 4000], dejando afuera la banda de transición en el medio
    # (ahí no se exige nada, ni ganancia 1 ni la atenuación pedida).
    zonas = list(zip(puntos[0::2], puntos[1::2]))

    # Va alternando paso/rechazo zona a zona, arrancando según pass_zero
    # (para lowpass/bandstop la primera zona, desde 0 Hz, es de paso; para
    # highpass/bandpass es de rechazo).
    es_paso, paso = [], pass_zero
    for _ in zonas:
        es_paso.append(paso)
        paso = not paso
    return zonas, es_paso


def _respuesta_db(coeffs, fs, nfft):
    """Respuesta en frecuencia |H(f)| de los coeficientes `coeffs`, en dB,
    evaluada en `nfft//2 + 1` puntos entre 0 Hz y Fs/2 (Nyquist)."""
    # rfft (FFT "real") alcanza porque los coeficientes son reales: la mitad
    # negativa del espectro es el conjugado de la positiva, no aporta nada
    # nuevo. Pedirle nfft puntos a una señal de `len(coeffs)` muestras
    # (nfft >> len(coeffs)) es "zero-padding": no agrega información, pero
    # interpola la respuesta en más puntos de frecuencia -> más resolución
    # para ver el rebote fino de la banda de rechazo en el gráfico.
    # El maximum con 1e-12 evita log10(0) = -inf si algún bin da ganancia
    # exactamente nula (saldría -240 dB en vez de -inf, igual de "silencio"
    # a los fines prácticos, pero sin romper la cuenta).
    H = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(coeffs, nfft)), 1e-12))
    # Eje de frecuencias en Hz correspondiente a cada bin de la rfft de
    # arriba: bin k representa la frecuencia k * Fs / nfft.
    f = np.arange(nfft // 2 + 1) * fs / nfft
    return f, H


def atenuacion_lograda(coeffs_norm, fs, cutoff, df, pass_zero, nfft=NFFT_DEFAULT):
    """
    Evalúa coeffs_norm (coeficientes YA normalizados, float o Q15/32768)
    sobre sus propias zonas de paso/rechazo.

    Devuelve (gmax_paso, atenuacion_min_rechazo), ambos en dB. La
    atenuación es -gmax_rechazo: cuanto más alto, mejor rechazo.
    """
    f, H = _respuesta_db(coeffs_norm, fs, nfft)
    zonas, es_paso = zonas_paso_rechazo(fs, cutoff, df, pass_zero)

    gmax_paso, gmax_rechazo = -np.inf, -np.inf
    for (lo, hi), paso in zip(zonas, es_paso):
        m = (f >= lo) & (f <= hi)  # máscara booleana: bins de f dentro de esta zona
        if not m.any():
            continue  # zona vacía (puede pasar si lo==hi, p.ej. df=0)
        if paso:
            # Peor caso en la banda de paso = el pico más alto (si pasa de
            # 0 dB, hay ganancia de más -> ver normalizar_pico_0db).
            gmax_paso = max(gmax_paso, H[m].max())
        else:
            # Peor caso en la banda de rechazo = el punto MENOS atenuado
            # (el más cercano a 0 dB), porque ese es el que define cuánta
            # atenuación "garantiza" el filtro en el peor punto.
            gmax_rechazo = max(gmax_rechazo, H[m].max())
    # La atenuación es -gmax_rechazo: si el punto menos atenuado está a
    # -25 dB, el filtro atenúa "al menos" 25 dB en toda la banda de rechazo.
    return gmax_paso, -gmax_rechazo


def ripple_gmax(coeffs_norm, fs, cutoff, df, pass_zero, nfft=NFFT_DEFAULT):
    """Ripple (máx-mín, dB) y ganancia máxima en toda la banda de paso."""
    f, H = _respuesta_db(coeffs_norm, fs, nfft)
    zonas, es_paso = zonas_paso_rechazo(fs, cutoff, df, pass_zero)
    # Junta los valores de H en TODAS las zonas de paso (puede haber más de
    # una, p.ej. pasabanda tiene rechazo-paso-rechazo -> una sola zona de
    # paso en el medio; elimina banda tiene paso-rechazo-paso -> dos zonas
    # de paso, una a cada lado del notch).
    vals = [H[(f >= lo) & (f <= hi)] for (lo, hi), paso in zip(zonas, es_paso)
            if paso and ((f >= lo) & (f <= hi)).any()]
    if not vals:
        return 0.0, 0.0
    allv = np.concatenate(vals)
    # Ripple = diferencia entre el pico más alto y el más bajo dentro de la
    # banda de paso (cuánto "ondula" la ganancia, debería ser chico si el
    # diseño es bueno). gmax = el pico más alto (debería rondar 0 dB si ya
    # se normalizó con normalizar_pico_0db).
    return float(allv.max() - allv.min()), float(allv.max())


def normalizar_pico_0db(h, fs, cutoff, df, pass_zero, nfft=NFFT_DEFAULT):
    """Escala h para que el pico de ganancia en la banda de paso sea 0 dB.
    Evita que el ripple del ventaneo sature al cuantizar a Q15."""
    gmax_paso, _ = atenuacion_lograda(h, fs, cutoff, df, pass_zero, nfft)
    # gmax_paso está en dB; 10**(-gmax_paso/20) es el factor lineal que hay
    # que multiplicar para que ese pico pase a valer exactamente 0 dB
    # (ganancia 1). Es la conversión estándar dB -> lineal para amplitud
    # (20*log10, no 10*log10, que es para potencia).
    return h * 10 ** (-gmax_paso / 20)


def cuantizar_q15(h, pad_par=True):
    """Redondea a Q15, satura a [-32768, 32767]. Si pad_par y la longitud
    es impar, agrega un 0 al final (arm_fir_init_q15 exige numTaps par)."""
    # normalizar_pico_0db solo garantiza que la GANANCIA en la banda de paso
    # sea 0 dB -- con ventaneo (Kaiser) eso alcanza para que cada coeficiente
    # individual quede chico, pero un diseño equirizado puede tener algún
    # tap individual más grande que 1 en magnitud (se cancelan entre sí para
    # dar la ganancia correcta). Si eso pasa, hay que volver a escalar antes
    # de cuantizar: si no, clip() satura cada tap grande al mismo valor
    # ±32767 y arruina por completo la forma del filtro (no es un problema
    # de 1-2 dB, es un filtro roto). Con Kaiser esto nunca dispara.
    pico = np.abs(h).max()
    if pico >= 1.0:
        h = h / (pico * 1.0001)  # margen chico para no quedar justo en el borde
    # Q15 representa el rango [-1, 1) en pasos de 1/32768: multiplicar por
    # 32768 y redondear al entero más cercano es la cuantización. clip
    # satura el 1 caso límite (h=1.0 exacto redondearía a 32768, que no
    # entra en int16 con signo) al máximo representable, 32767.
    q = np.clip(np.round(h * 32768).astype(np.int64), -32768, 32767)
    if pad_par and len(q) % 2 == 1:
        # Un tap extra en 0 no cambia la respuesta del filtro (multiplica
        # una muestra por 0), solo hace que la lista de coeficientes tenga
        # longitud par, que es lo que pide arm_fir_init_q15.
        q = np.append(q, 0)
    return q.astype(np.int32)


def cutoff_bandstop_fr_bw(fr, bw, df):
    """
    Convierte una especificación de elimina banda por frecuencia de
    rechazo (Fr) y ancho de banda (Bw) -- ambos en Hz -- a los dos cortes
    -6 dB que espera disenar_fir_kaiser: la banda [Fr-Bw/2, Fr+Bw/2] es la
    que hay que rechazar, y el corte -6 dB queda df/2 más afuera (hacia la
    banda de paso) a cada lado.

    fr, bw : frecuencia central y ancho de la banda a rechazar (Hz)
    df     : ancho de transición en Hz, escalar o [df_lo, df_hi]

    Devuelve cutoff = [f1, f2], listo para pasar a disenar_fir_kaiser.
    """
    df_arr = np.broadcast_to(np.atleast_1d(df).astype(float), (2,))
    # Bordes de la banda a rechazar, p.ej. Fr=50, Bw=15 -> [42.5, 67.5].
    # El corte -6 dB de cada lado se corre df/2 hacia afuera (lejos del
    # centro): [-1, 1] * df_arr/2 resta a la izquierda y suma a la derecha,
    # ensanchando el rango. Con df=20 queda [42.5-10, 67.5+10] = [32.5, 67.5].
    return np.array([fr - bw / 2, fr + bw / 2]) + np.array([-1, 1]) * df_arr / 2


def disenar_fir_kaiser(ftype, cutoff, astop_db, fs, df, numtaps=None, max_iter=2000):
    """
    Diseña un FIR por ventaneo de Kaiser.

    ftype    : 'lowpass' | 'highpass' | 'bandpass' | 'bandstop'
    cutoff   : corte en Hz (lowpass/highpass) o [f1, f2] (bandpass/bandstop)
    astop_db : atenuación objetivo/referencia en dB
    fs       : frecuencia de muestreo en Hz
    df       : ancho de banda de transición en Hz (escalar o uno por borde)
    numtaps  : si se da, FUERZA esa longitud (impar) sin buscar automático;
               la atenuación lograda queda como dato de salida.
               Si es None, se calcula para cumplir astop_db (sobre los
               coeficientes Q15 normalizados, los que se exportarían).

    Devuelve un dict: h (float, normalizado), q (Q15), numtaps, beta,
    astop_f (dB, sobre h float), astop_q (dB, sobre q), ripple_q, gmax_q.
    """
    # pass_zero=True significa "la zona que arranca en 0 Hz es de paso",
    # que es el caso de lowpass (pasa continua) y bandstop (pasa todo menos
    # la banda rechazada, incluida continua). highpass/bandpass rechazan
    # continua, así que pass_zero=False. Es el mismo valor que después usan
    # zonas_paso_rechazo/atenuacion_lograda para saber qué zona es cuál.
    pass_zero = ftype in ('lowpass', 'bandstop')
    # beta depende solo de astop_db (ver kaiser_beta): se calcula una sola
    # vez acá afuera, no cambia entre los distintos intentos de longitud n.
    beta = kaiser_beta(astop_db)

    def _disenar(n):
        """Arma un FIR de n taps con esta beta/cutoff/fs, lo normaliza a
        pico 0 dB, lo cuantiza a Q15, y mide qué atenuación logra en Q15
        (la que importa: es la que queda grabada en el .h, no la de h)."""
        h = firwin(n, cutoff, window=('kaiser', beta), pass_zero=ftype, fs=fs)
        h = normalizar_pico_0db(h, fs, cutoff, df, pass_zero)
        q = cuantizar_q15(h)
        _, astop_q = atenuacion_lograda(q / 32768.0, fs, cutoff, df, pass_zero)
        return h, q, astop_q

    if numtaps is not None:
        # Modo "taps fijos": se le pide un único diseño con esa longitud
        # exacta (forzada a impar, ver abajo) y listo -- no hay búsqueda.
        h, q, astop_q = _disenar(numtaps if numtaps % 2 else numtaps + 1)
    else:
        # Modo automático: arranca de la estimación de Kaiser y ajusta
        # hasta encontrar el n más chico que realmente cumple astop_db en
        # Q15 (la fórmula es una aproximación, no exacta -- ver notebook).
        nyq = fs / 2.0
        # min(df): si hay dos bordes con distinto ancho de transición
        # (pasabanda/elimina banda), el más angosto es el que manda --
        # exigir más taps de lo que ese borde necesita no tendría sentido,
        # y menos no alcanzaría para ÉL aunque sobre para el otro borde.
        width_norm = np.atleast_1d(df).astype(float).min() / nyq
        # Fórmula empírica de Kaiser para el orden del filtro (ver notebook
        # para la derivación completa): N ~ (A-7.95) / (2.285 * 2*pi * df/Fs).
        n_est = int(np.ceil((astop_db - 7.95) / (2.285 * np.pi * width_norm))) + 1
        # Tipo I (longitud impar) = fase lineal exacta con retardo entero
        # de muestras; si la estimación dio par, se suma 1.
        n = n_est if n_est % 2 else n_est + 1

        # Paso 1: subir n de a 2 (se mantiene impar) hasta que el diseño
        # realmente cumpla astop_db en Q15 -- la estimación de Kaiser a
        # veces se queda corta por una fracción de dB.
        for _ in range(max_iter):
            h, q, astop_q = _disenar(n)
            if astop_q >= astop_db:
                break
            n += 2
        else:
            raise RuntimeError(f"No se alcanzó Astop={astop_db} dB en {max_iter} iteraciones")

        # Paso 2: la estimación de Kaiser también puede quedarse larga
        # (sobreestimar) -- se prueba bajar de a 2 mientras el diseño más
        # corto siga cumpliendo, para no exportar más taps de los que hacen
        # falta. Apenas deja de cumplir, se descarta ese intento y se queda
        # con el último que sí cumplía.
        while n > 5:
            h2, q2, astop_q2 = _disenar(n - 2)
            if astop_q2 < astop_db:
                break
            n -= 2
            h, q, astop_q = h2, q2, astop_q2
        numtaps = n

    # Datos adicionales para reportar/graficar: atenuación sobre los
    # coeficientes en float (antes de cuantizar, para comparar contra la Q15
    # y ver cuánto perjudica la cuantización) y el ripple/ganancia pico.
    _, astop_f = atenuacion_lograda(h, fs, cutoff, df, pass_zero)
    ripple_q, gmax_q = ripple_gmax(q / 32768.0, fs, cutoff, df, pass_zero)
    return dict(h=h, q=q, numtaps=len(q), beta=beta, astop_f=astop_f,
                astop_q=astop_q, ripple_q=ripple_q, gmax_q=gmax_q,
                cutoff=np.atleast_1d(cutoff).astype(float),
                df=np.atleast_1d(df).astype(float),
                fs=fs, pass_zero=pass_zero)


def disenar_fir_equiripple(ftype, cutoff, astop_db, fs, df, numtaps=None,
                            ripple_paso_db=0.5, max_iter=2000):
    """
    Diseña un FIR equirizado (algoritmo de Parks-McClellan / intercambio de
    Remez), alternativa a disenar_fir_kaiser para el MISMO corte/Fs/Astop.

    A diferencia del ventaneo (Kaiser aplicado sobre un sinc ideal), acá se
    le pide directamente al algoritmo de Remez el filtro que minimiza el
    error máximo (en el sentido de Chebyshev/minimax) entre la respuesta
    real y la ideal, banda por banda. Para la misma especificación suele
    necesitar bastantes menos taps que Kaiser -- a costa de no tener una
    fórmula cerrada para estimar el orden de entrada (acá se parte de una
    estimación y se ajusta igual que en disenar_fir_kaiser).

    ftype    : 'lowpass' | 'highpass' | 'bandpass' | 'bandstop'
    cutoff   : corte en Hz (lowpass/highpass) o [f1, f2] (bandpass/bandstop)
    astop_db : atenuación pedida en la banda de rechazo, en dB
    fs       : frecuencia de muestreo en Hz
    df       : ancho de banda de transición en Hz (escalar o uno por borde)
    numtaps  : si se da, FUERZA esa longitud (impar); si es None, se busca
               automáticamente el n mínimo que cumple astop_db en Q15.
    ripple_paso_db : rizado máximo tolerado en la banda de paso, en dB. La
               consigna no lo fija (igual que df en Kaiser) -- cuanto más
               chico, más exigente la banda de paso y más taps hacen falta.

    Devuelve el mismo dict que disenar_fir_kaiser (mismas claves), así que
    se puede usar con las mismas exportar_header/graficar/atenuacion_lograda.
    """
    pass_zero = ftype in ('lowpass', 'bandstop')
    # remez necesita las bandas como lista plana [lo1, hi1, lo2, hi2, ...]
    # con un 0/1 de ganancia deseada por banda -- exactamente lo que ya arma
    # zonas_paso_rechazo (las mismas bandas que usa Kaiser para verificar,
    # acá se las pasamos directo al diseño en vez de solo a la verificación).
    zonas, es_paso = zonas_paso_rechazo(fs, cutoff, df, pass_zero)
    bands = [v for par in zonas for v in par]
    desired = [1 if p else 0 for p in es_paso]
    # Pesos: remez reparte el error de forma que (peso_i * error_i) quede
    # parejo entre bandas, así que el peso de cada banda es el inverso del
    # rizado que se le quiere permitir ahí. delta_s sale directo de
    # astop_db; delta_p es la aproximación lineal estándar de un rizado en
    # dB chico alrededor de ganancia 1.
    delta_s = 10 ** (-astop_db / 20)
    delta_p = 10 ** (ripple_paso_db / 20) - 1
    weight = [1 / delta_p if p else 1 / delta_s for p in desired]

    def _disenar(n):
        h = remez(n, bands, desired, weight=weight, fs=fs)
        h = normalizar_pico_0db(h, fs, cutoff, df, pass_zero)
        q = cuantizar_q15(h)
        _, astop_q = atenuacion_lograda(q / 32768.0, fs, cutoff, df, pass_zero)
        return h, q, astop_q

    def _disenar_o_falla(n):
        """Como _disenar, pero si Remez no converge (puede pasar con n chico,
        sobre todo con 3 bandas: pasabanda/elimina banda) lo trata igual que
        un diseño que no alcanza astop_db -- señal para seguir subiendo n,
        en vez de cortar la búsqueda con una excepción."""
        try:
            return _disenar(n)
        except ValueError:
            return None, None, -np.inf

    if numtaps is not None:
        h, q, astop_q = _disenar(numtaps if numtaps % 2 else numtaps + 1)
    else:
        # No hay fórmula cerrada para el orden de un diseño equirizado como
        # la de Kaiser, pero un equirizado para la misma especificación
        # nunca necesita MÁS taps que el Kaiser equivalente (es el diseño
        # que, para un orden dado, minimiza el error máximo -- en el peor
        # caso iguala a Kaiser, pero en la práctica suele necesitar bastante
        # menos). Arrancar la búsqueda en la mitad de la estimación de
        # Kaiser ahorra muchísimas iteraciones comparado con arrancar de un
        # n chico fijo, sobre todo en los filtros que terminan necesitando
        # miles de taps (pasaaltos/pasabanda/elimina banda a 44-48 kHz).
        nyq = fs / 2.0
        width_norm = np.atleast_1d(df).astype(float).min() / nyq
        n_kaiser = int(np.ceil((astop_db - 7.95) / (2.285 * np.pi * width_norm))) + 1
        n = max(5, n_kaiser // 2)
        n = n if n % 2 else n + 1

        # Paso 1: subir de a 2 (Tipo I, longitud impar) hasta cumplir
        # astop_db en Q15 -- o hasta que Remez converja, si el punto de
        # partida resultó demasiado corto.
        for _ in range(max_iter):
            h, q, astop_q = _disenar_o_falla(n)
            if astop_q >= astop_db:
                break
            n += 2
        else:
            raise RuntimeError(f"No se alcanzó Astop={astop_db} dB en {max_iter} iteraciones")

        # Paso 2: bajar mientras el diseño más corto siga cumpliendo, para
        # no quedarse con más taps de los que hacen falta (mismo criterio
        # que el paso 2 de disenar_fir_kaiser).
        while n > 5:
            h2, q2, astop_q2 = _disenar_o_falla(n - 2)
            if astop_q2 < astop_db:
                break
            n -= 2
            h, q, astop_q = h2, q2, astop_q2
        numtaps = n

    _, astop_f = atenuacion_lograda(h, fs, cutoff, df, pass_zero)
    ripple_q, gmax_q = ripple_gmax(q / 32768.0, fs, cutoff, df, pass_zero)
    return dict(h=h, q=q, numtaps=len(q), beta=None, astop_f=astop_f,
                astop_q=astop_q, ripple_q=ripple_q, gmax_q=gmax_q,
                cutoff=np.atleast_1d(cutoff).astype(float),
                df=np.atleast_1d(df).astype(float),
                fs=fs, pass_zero=pass_zero)


# =============================================================================
# Exportación a filter_coeffs.h (mismo formato que usaba disenio_filtros_tp2.m)
# =============================================================================

def exportar_header(resultados, filtros, fs_label, h_path, metodo='Kaiser (ventaneo)'):
    """
    resultados: dict {(key, fs_label): resultado de disenar_fir_kaiser o
                 disenar_fir_equiripple -- mismo formato de dict, se puede
                 pasar cualquiera de los dos (o incluso una mezcla).
    filtros: lista de specs (ver main()), en el orden en que se quieren
             las secciones del header.
    metodo: texto libre para el comentario de cabecera, indica con qué
            método se generaron estos coeficientes (Kaiser o equirizado).
    """
    max_taps = max(r['numtaps'] for r in resultados.values())  # el filtro más largo de los 20
    total = sum(r['numtaps'] for r in resultados.values())     # coeficientes totales a grabar en flash

    with open(h_path, 'w') as fid:
        # --- Comentario de cabecera: documenta cómo se generó el archivo y
        # deja una tabla resumen con los parámetros de cada uno de los 20
        # diseños, para poder revisarlos de un vistazo sin abrir el script.
        fid.write('/*\n * filter_coeffs.h\n *\n')
        fid.write(' * GENERADO AUTOMATICAMENTE por disenio_filtros_tp2.py: no editar a mano.\n')
        fid.write(f' * Filtros FIR por {metodo}, coeficientes en Q15.\n')
        fid.write(' * Longitud par por requisito de arm_fir_init_q15 (ultimo tap = 0\n')
        fid.write(' * cuando el diseno era impar).\n *\n')
        fid.write(' * filtro  Fs      taps  beta     Astop req/real[dB]  ripple paso[dB]\n')
        for F in filtros:
            for fsl in fs_label:
                r = resultados[(F['key'], fsl)]
                # beta es None en diseños equirizados (no usan ventana) --
                # se deja la columna en texto para no romper el formato.
                beta_str = f"{r['beta']:.3f}" if r['beta'] is not None else 'equirip'
                fid.write(f" * {F['key']:<6s}  {fsl:<6s} {r['numtaps']:5d}  {beta_str:>7s}  "
                          f"{F['astop']:4.0f} / {r['astop_q']:5.1f}          {r['ripple_q']:.2f}\n")
        fid.write(' */\n\n#ifndef FILTER_COEFFS_H\n#define FILTER_COEFFS_H\n\n')
        fid.write('#include "arm_math.h"\n\n')
        fid.write(f'#define MAX_FIR_TAPS {max_taps}\n\n')
        # fir_filter_config_t es lo que lee processing_stage.c: un puntero a
        # los coeficientes más cuántos son, para poder llamar
        # arm_fir_init_q15(..., config->pCoeffs, config->numTaps, ...).
        fid.write('typedef struct {\n    const q15_t *pCoeffs;\n    uint16_t numTaps;\n} fir_filter_config_t;\n\n')

        # --- Un array de coeficientes const por cada (filtro, Fs), p.ej.
        # coeffs_lp_8k[34]. Se escriben de a 12 por línea para que el header
        # no termine siendo una sola línea gigante.
        for F in filtros:
            fid.write(f"/* ---- {F['desc']} ---- */\n")
            for fsl in fs_label:
                r = resultados[(F['key'], fsl)]
                q = r['q']
                fid.write(f"static const q15_t coeffs_{F['key']}_{fsl}[{len(q)}] = {{\n")
                for m in range(0, len(q), 12):
                    fila = q[m:m + 12]
                    fid.write('    ' + ''.join(f'{v:6d}, ' for v in fila) + '\n')
                fid.write('};\n\n')

        # --- Una tabla fir_config_<filtro>[5] por cada tipo de filtro, con
        # un fir_filter_config_t por cada Fs en el mismo orden que fs_label
        # -- así processing_stage.c puede indexarla directo con el enum
        # sample_rate_t (fir_config_lp[SAMPLE_RATE_8K], etc.).
        fid.write(f"/* Tablas de configuracion. Orden de las Fs: {', '.join(fs_label)} */\n")
        for F in filtros:
            key = F['key']
            fid.write(f'static const fir_filter_config_t fir_config_{key}[] = {{\n')
            for fsl in fs_label:
                nombre = f'coeffs_{key}_{fsl}'
                fid.write(f'    {{ .pCoeffs = {nombre}, .numTaps = sizeof({nombre}) / sizeof({nombre}[0]) }},\n')
            fid.write('};\n\n')
        fid.write('#endif /* FILTER_COEFFS_H */\n')

    print(f'Exportado: {h_path}')
    print(f'MAX_FIR_TAPS = {max_taps}; coeficientes totales = {total} ({total*2/1024:.1f} KB en flash)')


# =============================================================================
# Gráficas (requiere matplotlib; opcional)
# =============================================================================

def graficar(resultados, filtros, fs_label, graf_dir, nfft=NFFT_DEFAULT):
    """Una figura por tipo de filtro, con un subplot por Fs (5 subplots
    apilados), mostrando |H(f)| en dB de los coeficientes Q15 junto con las
    bandas exigidas, para verificar visualmente que cada diseño cumple."""
    import matplotlib
    matplotlib.use('Agg')  # backend sin ventana: solo escribe a archivo, no necesita pantalla
    import matplotlib.pyplot as plt

    os.makedirs(graf_dir, exist_ok=True)
    for F in filtros:
        # Una fila de subplots por frecuencia de muestreo (5 en total),
        # apilados verticalmente en una sola figura por filtro.
        fig, axes = plt.subplots(len(fs_label), 1, figsize=(9, 2.2 * len(fs_label)))
        for ax, fsl in zip(np.atleast_1d(axes), fs_label):
            r = resultados[(F['key'], fsl)]
            fs = r['fs']
            f, Hq = _respuesta_db(r['q'] / 32768.0, fs, nfft)
            # Eje de frecuencia en escala logarítmica: así se ve bien tanto
            # el detalle cerca de 0 Hz (filtros con corte bajo) como el
            # resto del espectro hasta Nyquist, sin tener que hacer zoom.
            ax.semilogx(f[1:], Hq[1:], 'm', linewidth=0.9, label='Q15')
            # Dibuja las bandas exigidas encima de la curva: una línea
            # verde a 0 dB en cada zona de paso, una roja a -Astop en cada
            # zona de rechazo -- si la curva magenta se mantiene por encima
            # de la verde y por debajo de la roja, el filtro cumple.
            zonas, es_paso = zonas_paso_rechazo(fs, r['cutoff'], r['df'], r['pass_zero'])
            for (lo, hi), paso in zip(zonas, es_paso):
                ax.plot([max(lo, 1), hi], [0, 0] if paso else [-F['astop']] * 2,
                        'g' if paso else 'r', linewidth=2)
            # Líneas punteadas verticales marcando cada corte -6 dB.
            for c in r['cutoff']:
                ax.axvline(c, color='k', linestyle=':', linewidth=0.8)
            ax.set_xlim(1, fs / 2)  # de 1 Hz (semilogx no admite 0) a Nyquist
            ax.set_ylim(-80, 5)
            ax.grid(True, alpha=0.3)
            ax.set_ylabel('|H| [dB]')
            ax.set_title(f"{F['desc']}  Fs={fs} Hz  -  {r['numtaps']} taps, "
                         f"Astop real {r['astop_q']:.1f} dB (Q15)", fontsize=9)
        axes_flat = np.atleast_1d(axes)
        axes_flat[0].legend(loc='lower right')
        axes_flat[-1].set_xlabel('Frecuencia [Hz]')
        fig.tight_layout()
        fig.savefig(os.path.join(graf_dir, f"respuesta_{F['key']}.png"), dpi=110)
        plt.close(fig)  # libera la figura de memoria (importante al generar varias seguidas)
        print(f"Grafica: respuesta_{F['key']}.png")


# =============================================================================
# PARÁMETROS DE DISEÑO y punto de entrada
# =============================================================================

def main():
    # Frecuencias de muestreo: deben coincidir con s_sample_rate_hz en
    # adc_stage.c, en el mismo orden que sample_rate_t (adc_stage.h).
    fs_label = ['8k', '16k', '22k', '44k', '48k']
    fs_list = [8000, 16000, 22000, 44000, 48000]

    # =========================================================================
    # PARÁMETROS DE DISEÑO CONFIGURABLES
    # =========================================================================

    # --- 1. Pasa Bajos (LP) ---
    # Consigna: Fc = 3600 Hz, Astop >= 30 dB
    fc_lp = 3600          # Frecuencia de corte a -6 dB en Hz (consigna)
    astop_lp = 30         # Atenuación mínima requerida en la banda de rechazo en dB (consigna)
    df_lp = 400           # Ancho de la banda de transición en Hz (transición de 3400 a 3800 Hz)

    # --- 2. Pasa Altos (HP) ---
    # Consigna: Fc = 35 Hz, Astop >= 30 dB
    fc_hp = 35            # Frecuencia de corte a -6 dB en Hz (consigna)
    astop_hp = 30         # Atenuación mínima requerida en la banda de rechazo en dB (consigna)
    df_hp = 60            # Ancho de transición en Hz (rechazo <= 5 Hz, paso >= 65 Hz)

    # --- 3. Pasa Banda (BP) ---
    # Consigna: Fc1 = 35 Hz, Fc2 = 3500 Hz, Astop >= 30 dB
    fc_bp = [35, 3500]    # Frecuencias de corte inferior y superior a -6 dB en Hz (consigna)
    astop_bp = 30         # Atenuación mínima requerida en la banda de rechazo en dB (consigna)
    df_bp = [60, 400]     # Ancho de transición inferior (60 Hz) y superior (400 Hz)

    # --- 4. Elimina Banda / Notch (BS) ---
    # Consigna: Fr = 50 Hz, BW = 15 Hz (rechazo [42.5, 57.5] Hz), Astop >= 25 dB
    fr_bs = 50            # Frecuencia central de rechazo en Hz (consigna)
    bw_bs = 15            # Ancho de banda de rechazo en Hz (consigna: 42.5 a 57.5 Hz)
    astop_bs = 25         # Atenuación mínima requerida en la banda de rechazo en dB (consigna)
    # Transición asimétrica: df_lo=35 Hz hacia continua (paso [0, 7.5] Hz) y
    # df_hi=90 Hz hacia altas frecuencias para reducir fuertemente la cantidad de taps
    df_bs = [35, 90]

    # Cortes a -6 dB para elimina banda en base a Fr, BW y df
    cutoff_bs = cutoff_bandstop_fr_bw(fr=fr_bs, bw=bw_bs, df=df_bs)

    filtros = [
        dict(key='lp', desc='Pasa bajos',    ftype='lowpass',
             cutoff=fc_lp,                              df=df_lp,     astop=astop_lp),
        dict(key='hp', desc='Pasa altos',    ftype='highpass',
             cutoff=fc_hp,                              df=df_hp,      astop=astop_hp),
        dict(key='bp', desc='Pasa banda',    ftype='bandpass',
             cutoff=fc_bp,                              df=df_bp,     astop=astop_bp),
        dict(key='bs', desc='Elimina banda', ftype='bandstop',
             cutoff=cutoff_bs,                          df=df_bs,      astop=astop_bs),
    ]

    # El .h se escribe directo en el proyecto del firmware (ruta relativa a
    # este archivo, no al directorio desde el que se lo ejecute).
    out_dir = os.path.dirname(os.path.abspath(__file__))
    h_path = os.path.join(out_dir, '..', 'MCXN947_Project_TP2', 'source', 'filter_coeffs.h')
    graf_dir = os.path.join(out_dir, 'graficas')

    # resultados queda indexado por (clave_del_filtro, etiqueta_de_Fs),
    # p.ej. resultados[('lp', '8k')] -- así exportar_header/graficar pueden
    # recorrer filtros x fs_label en el orden que quieran y encontrar cada
    # diseño directo, sin tener que buscarlo.
    resultados = {}
    print(f"{'filt':<4s} {'Fs':>6s} {'taps':>6s} {'beta':>7s} {'Astop req/real':>16s} {'ripple':>8s}")
    for F in filtros:
        for fsl, fs in zip(fs_label, fs_list):
            r = disenar_fir_kaiser(F['ftype'], F['cutoff'], F['astop'], fs, F['df'])
            resultados[(F['key'], fsl)] = r
            print(f"{F['key']:<4s} {fsl:>6s} {r['numtaps']:>6d} {r['beta']:>7.3f} "
                  f"{F['astop']:>7.0f} / {r['astop_q']:<6.1f} {r['ripple_q']:>7.2f}dB")

    exportar_header(resultados, filtros, fs_label, h_path)
    graficar(resultados, filtros, fs_label, graf_dir)

    return resultados, filtros, fs_label


if __name__ == '__main__':
    main()
