% =========================================================================
% disenio_filtros_tp2.m
%
% TP2 - Diseño de los filtros FIR por el método de ventaneo (Kaiser).
% Genera los 20 conjuntos de coeficientes (4 filtros x 5 Fs), verifica la
% respuesta en frecuencia en float y en Q15, grafica y exporta
% filter_coeffs.h listo para CMSIS-DSP (arm_fir_q15).
%
% Requiere Signal Processing Toolbox (fir1, kaiser). Compatible con Octave
% (pkg load signal).
%
% Todo lo que se puede ajustar está en la sección PARÁMETROS DE DISEÑO.
% =========================================================================
clear; close all; clc;
if exist('OCTAVE_VERSION', 'builtin'), pkg load signal; end

% Sin Signal Processing Toolbox: equivalentes de kaiser y fir1 (ventaneo de
% la respuesta ideal, misma escala que fir1: ganancia 1 en DC para low/stop,
% en Nyquist para high y en el centro de la banda para bandpass).
if ~exist('fir1', 'file') || ~exist('kaiser', 'file')
  snc = @(x) (x == 0) + sin(pi*x) ./ (pi*x + (x == 0));
  kaiser = @(n, b) besseli(0, b*sqrt(1 - (((0:n-1)' - (n-1)/2) / ((n-1)/2)).^2)) / besseli(0, b);
  % bandas de paso [a b] (normalizadas a Nyquist): e = [0 Wn 1], s = 1 o 2
  ideal = @(m, a, b) sum(b(:) .* snc(b(:) * m) - a(:) .* snc(a(:) * m), 1);
  ideal_e = @(m, e, s) ideal(m, e(s:2:end-1), e(s+1:2:end));
  pass0 = @(ftype) any(strcmp(ftype, {'low', 'stop'}));
  escala = @(h, m, f0) h / abs(sum(h .* exp(-1j*pi*f0*m)));
  fir1 = @(ord, Wn, ftype, w) escala(w(:).' .* ideal_e((0:ord) - ord/2, [0 Wn 1], 1 + ~pass0(ftype)), ...
    (0:ord) - ord/2, ~pass0(ftype) * ((numel(Wn) == 1) + (numel(Wn) == 2) * mean(Wn)));
end

%% ======================= PARÁMETROS DE DISEÑO ==========================

% Frecuencias de muestreo: etiqueta usada en los nombres C y Fs real [Hz].
% Deben coincidir con s_sample_rate_hz en adc_stage.c, en el mismo orden
% que sample_rate_t (adc_stage.h).
fs_label = {'8k', '16k', '22k', '44k', '48k'};
fs_list  = [8000, 16000, 22000, 44000, 48000];

% Convención: en el método de ventaneo la frecuencia de corte es el punto
% de -6 dB (ganancia 0.5), en el centro de la banda de transición.
% df = ancho de la banda de transición (de Fpass a Fstop) en Hz. Los valores
% de abajo son los NOMINALES (la consigna no los fija): se usan tal cual si
% el filtro entra en el presupuesto de CPU, y si no se ensanchan (ver
% PRESUPUESTO DE CPU).
%
% 'fref' y 'ancla' definen dónde se ubica cada transición:
%   'centro': fref es el corte (-6 dB), la transición queda centrada en él
%             y al ensancharla el corte no se mueve.
%   'stop'  : fref es el borde de la banda de rechazo que exige la consigna;
%             la transición crece hacia la banda de paso, así la banda
%             rechazada nunca se achica (el corte -6 dB se corre hacia afuera).
%
% LP: Fc = 3600 Hz, transición 3400-3800 Hz (debe caber bajo 4000 Hz,
%     el Nyquist de Fs = 8 kHz).
% HP: Fc = 35 Hz, rechazo por debajo de 10 Hz (infrasónico), paso desde 60 Hz.
% BP: bordes iguales a los del HP (35 Hz) y a un LP en 3500 Hz.
% BS: Fr = 50 Hz, BW = 15 Hz -> la banda 42.5-57.5 Hz debe quedar atenuada
%     >= 25 dB. Con transiciones de 20 Hz los cortes (-6 dB) quedan en
%     32.5 y 67.5 Hz, y las bandas de paso terminan/empiezan en 22.5/77.5 Hz.
filt(1) = struct('key','lp', 'desc','Pasa bajos',    'ftype','low', ...
                 'fref',3600,        'ancla','centro', 'df',400,      'astop',30, 'pass_zero',true);
filt(2) = struct('key','hp', 'desc','Pasa altos',    'ftype','high', ...
                 'fref',35,          'ancla','centro', 'df',50,       'astop',30, 'pass_zero',false);
filt(3) = struct('key','bp', 'desc','Pasa banda',    'ftype','bandpass', ...
                 'fref',[35 3500],   'ancla','centro', 'df',[50 400], 'astop',30, 'pass_zero',false);
filt(4) = struct('key','bs', 'desc','Elimina banda', 'ftype','stop', ...
                 'fref',[42.5 57.5], 'ancla','stop',   'df',[20 20],  'astop',25, 'pass_zero',true);

% Normalizar para que el pico de ganancia en banda de paso sea 0 dB.
% El ripple del ventaneo (+-delta, delta = 10^(-Astop/20)) haría que la
% ganancia supere 1 levemente; en Q15 eso satura señales de amplitud máxima.
NORMALIZAR_PICO_0DB = true;

% arm_fir_init_q15 exige numTaps par y >= 4. Los diseños son de longitud
% impar (tipo I: fase lineal y retardo entero, obligatorio para HP y BS),
% así que se agrega un coeficiente 0 al final.
PAD_PAR_CMSIS_Q15 = true;

% Estimación de carga con arm_fir_q15 procesando por bloques
% (PROCESSING_BLOCK_SIZE en processing_stage.h). Es una ESTIMACIÓN: medir en la
% placa con DWT->CYCCNT (ciclos por bloque / (taps * muestras del bloque)).
CICLOS_POR_TAP = 3;
F_CPU = 150e6;            % Cortex-M33 de la MCXN947

%% ------------------------- PRESUPUESTO DE CPU --------------------------
% El orden de Kaiser es N ~ (A - 7.95) / (2.285 * 2*pi * df/Fs): con df fijo
% en Hz, N crece proporcional a Fs. Cada filtro tiene como máximo
%   taps_max = CPU_MAX * F_CPU / (CICLOS_POR_TAP * Fs)
% y si con el df nominal no entra se ensanchan sus transiciones más angostas
% (relajando la transición, nunca el Astop) hasta que entre. El límite de
% ensanche es geométrico: cada banda de paso o rechazo conserva al menos
% ANCHO_MIN_ZONA Hz (p. ej. en el HP, rechazo en [0, 5 Hz] como mínimo).
CPU_MAX = 0.50;           % fracción de CPU para el FIR (resto: ADC, DAC, UART)
ANCHO_MIN_ZONA = 5;       % [Hz]
FACTOR_ENSANCHE = 1.05;   % paso de ensanche si fir1 necesita más que Kaiser

% Si ni con la transición más ancha posible entra en el presupuesto, el
% filtro se marca NO VIABLE (como FIR por ventaneo, a esa Fs, con este
% presupuesto). Qué se exporta en ese caso:
%   'cumplir' : el diseño que cumple Astop con la transición más ancha,
%               aunque exceda el presupuesto (en la placa NO va a andar).
%   'recortar': el diseño de taps_max con la transición más ancha; entra en
%               el presupuesto pero NO cumple Astop (se informa el real).
SI_NO_ENTRA = 'cumplir';

NFFT = 2^20;              % resolución de la verificación: Fs/NFFT
GRAFICAR = true;
out_dir = fileparts(mfilename('fullpath'));
if isempty(out_dir), out_dir = pwd; end
% El .h se escribe directamente en el proyecto del firmware
h_path = fullfile(out_dir, '..', 'MCXN947_Project_TP2', 'source', 'filter_coeffs.h');

%% ============================== DISEÑO =================================

respdB = @(h) 20*log10(max(abs(fft(h(:), NFFT)), 1e-12));
semi = @(H) H(1:NFFT/2+1);                 % 0..Fs/2
rango = @(H, pm, sm) max(H(pm)) - max(H(sm));  % paso - rechazo [dB]
% Coeficientes como se exportan: normalizados (si corresponde) y en Q15
aq15 = @(h, H, pm) round(h * 10^(-NORMALIZAR_PICO_0DB*max(H(pm))/20) * 32768) / 32768;
R = struct([]);

for i = 1:numel(filt)
  F = filt(i);
  A = F.astop;

  % Beta de Kaiser a partir de la atenuación requerida
  if A > 50
    beta = 0.1102*(A - 8.7);
  elseif A >= 21
    beta = 0.5842*(A - 21)^0.4 + 0.07886*(A - 21);
  else
    beta = 0;
  end

  % Cómo crece cada transición al ensancharla: wl/wr = fracción de df a la
  % izquierda/derecha de fref (ver 'ancla' en PARÁMETROS DE DISEÑO)
  K = numel(F.fref);
  if strcmp(F.ancla, 'centro')
    wl = 0.5*ones(1, K);
  else
    % la zona a la izquierda del borde t es de paso -> la transición va hacia ella
    wl = double((mod(1:K, 2) == 1) == F.pass_zero);
  end
  wr = 1 - wl;

  for j = 1:numel(fs_list)
    Fs = fs_list(j);
    f  = (0:NFFT/2)' * Fs / NFFT;

    % Presupuesto: taps CMSIS máximos y longitud de diseño (impar) máxima
    taps_max = floor(CPU_MAX * F_CPU / (CICLOS_POR_TAP * Fs));
    n_lim = taps_max - PAD_PAR_CMSIS_Q15;
    n_lim = n_lim - (mod(n_lim, 2) == 0);

    % df máximo de cada transición: cada zona conserva ANCHO_MIN_ZONA Hz;
    % el hueco entre dos transiciones que crecen una hacia la otra se reparte
    df_max = inf(1, K);
    for t = 1:K
      if wl(t) > 0
        if t == 1
          parte = F.fref(1) - ANCHO_MIN_ZONA;
        else
          parte = (F.fref(t) - F.fref(t-1) - ANCHO_MIN_ZONA) / (1 + (wr(t-1) > 0));
        end
        df_max(t) = min(df_max(t), parte / wl(t));
      end
      if wr(t) > 0
        if t == K
          parte = Fs/2 - ANCHO_MIN_ZONA - F.fref(K);
        else
          parte = (F.fref(t+1) - F.fref(t) - ANCHO_MIN_ZONA) / (1 + (wl(t+1) > 0));
        end
        df_max(t) = min(df_max(t), parte / wr(t));
      end
    end

    % df que, según la fórmula de Kaiser, da exactamente n_lim
    df_obj = (A - 7.95) * Fs / (2.285 * 2*pi * (n_lim - 1));
    viable = true;

    while true
      % Solo se ensanchan las transiciones más angostas que df_obj
      df = min(max(F.df, df_obj), df_max);
      cutoff = F.fref + (wr - wl) .* df/2;
      Wn = cutoff / (Fs/2);                 % normalizado a Nyquist

      % Bandas de paso y de rechazo a partir de cortes y transiciones
      lo = cutoff - df/2;
      hi = cutoff + df/2;
      pts = [0, reshape([lo; hi], 1, []), Fs/2];
      zonas = reshape(pts, 2, [])';         % cada fila: [desde hasta]
      es_paso = false(size(zonas, 1), 1);
      pasa = F.pass_zero;
      for z = 1:size(zonas, 1)
        es_paso(z) = pasa;
        pasa = ~pasa;
      end
      pmask = false(size(f));
      smask = false(size(f));
      for z = 1:size(zonas, 1)
        m = f >= zonas(z,1) & f <= zonas(z,2);
        if es_paso(z), pmask = pmask | m; else, smask = smask | m; end
      end

      % Longitud estimada (fórmula de Kaiser), forzada a impar
      n_est = ceil((A - 7.95) / (2.285 * 2*pi * min(df) / Fs)) + 1;
      n = n_est + mod(n_est + 1, 2);

      % Astop se verifica sobre los coeficientes Q15 que se exportan
      cumple = @(h, H, pm, sm) rango(semi(respdB(aq15(h, H, pm))), pm, sm) >= A;

      % Subir la longitud hasta cumplir Astop (la fórmula puede subestimar)
      while true
        h = fir1(n-1, Wn, F.ftype, kaiser(n, beta));
        H = respdB(h);  H = H(1:NFFT/2+1);
        if cumple(h, H, pmask, smask), break; end
        n = n + 2;
      end
      % Bajar mientras siga cumpliendo (o puede sobreestimar)
      while n > 5
        h2 = fir1(n-3, Wn, F.ftype, kaiser(n-2, beta));
        H2 = respdB(h2);  H2 = H2(1:NFFT/2+1);
        if ~cumple(h2, H2, pmask, smask), break; end
        n = n - 2;  h = h2;  H = H2;
      end

      if n <= n_lim, break; end
      % No entra: ensanchar más, salvo que la transición que fija N ya esté
      % en su máximo geométrico
      [~, t] = min(df);
      if df(t) >= df_max(t), viable = false; break; end
      df_obj = max(df_obj, min(df)) * FACTOR_ENSANCHE;
    end

    n_cumple = n;                           % longitud que cumple Astop
    if ~viable && strcmp(SI_NO_ENTRA, 'recortar')
      n = n_lim;
      h = fir1(n-1, Wn, F.ftype, kaiser(n, beta));
      H = respdB(h);  H = H(1:NFFT/2+1);
    end
    if ~viable
      estado = 'NO VIABLE';
    elseif any(df > F.df)
      estado = 'df ensanchado';
    else
      estado = 'nominal';
    end

    if NORMALIZAR_PICO_0DB
      h = h * 10^(-max(H(pmask))/20);
    end
    Hf = respdB(h);  Hf = Hf(1:NFFT/2+1);

    % Conversión a Q15 y verificación con los coeficientes cuantizados
    q = round(h * 32768);
    q = min(max(q, -32768), 32767);
    if PAD_PAR_CMSIS_Q15 && mod(numel(q), 2) == 1
      q = [q, 0];
    end
    Hq = respdB(q / 32768);  Hq = Hq(1:NFFT/2+1);

    k = numel(R) + 1;
    R(k).key = F.key;        R(k).desc = F.desc;
    R(k).fs = Fs;            R(k).fs_label = fs_label{j};
    R(k).astop_req = A;      R(k).beta = beta;
    R(k).cutoff = cutoff;    R(k).zonas = zonas;  R(k).es_paso = es_paso;
    R(k).df_nom = F.df;      R(k).df = df;
    R(k).n_est = n_est;      R(k).n = n;          R(k).n_cumple = n_cumple;
    R(k).taps_max = taps_max;  R(k).estado = estado;
    R(k).h = h;              R(k).q = q;
    R(k).astop_f = -max(Hf(smask));
    R(k).astop_q = -max(Hq(smask));
    R(k).ripple_q = max(Hq(pmask)) - min(Hq(pmask));
    R(k).gmax_q = max(Hq(pmask));
  end
end

%% ============================ RESULTADOS ===============================

% df de cada transición como texto, p. ej. '50/400'
dfstr = @(v) strjoin(arrayfun(@(x) sprintf('%.0f', x), v, 'UniformOutput', false), '/');
cpu_pct = @(r) numel(r.q) * CICLOS_POR_TAP * r.fs / F_CPU * 100;

fprintf('Presupuesto: %.0f%% de CPU a %.0f MHz, %g ciclos/tap\n\n', ...
  CPU_MAX*100, F_CPU/1e6, CICLOS_POR_TAP);
fprintf('%-4s %6s %6s %6s %5s/%-5s %-9s %-9s %6s %8s %9s %7s %6s %9s %7s %-6s %s\n', ...
  'filt', 'Fs', 'N est', 'N', 'taps', 'max', 'df nom', 'df usado', 'beta', ...
  'Astop f', 'Astop Q15', 'ripple', 'Gmax', 'retardo', 'CPU est', 'cumple', 'estado');
for k = 1:numel(R)
  r = R(k);
  retardo_ms = (r.n - 1) / 2 / r.fs * 1e3;
  ok = 'NO';  if r.astop_q >= r.astop_req, ok = 'SI'; end
  fprintf('%-4s %6d %6d %6d %5d/%-5d %-9s %-9s %6.3f %8.1f %9.1f %7.2f %6.2f %7.1fms %6.0f%% %-6s %s\n', ...
    r.key, r.fs, r.n_est, r.n, numel(r.q), r.taps_max, dfstr(r.df_nom), dfstr(r.df), ...
    r.beta, r.astop_f, r.astop_q, r.ripple_q, r.gmax_q, retardo_ms, cpu_pct(r), ok, r.estado);
end
nv = find(strcmp({R.estado}, 'NO VIABLE'));
for k = nv
  fprintf('NO VIABLE: %s a %d Hz necesita %d taps para Astop = %d dB (max %d)\n', ...
    R(k).desc, R(k).fs, R(k).n_cumple + PAD_PAR_CMSIS_Q15, R(k).astop_req, R(k).taps_max);
end
max_taps = max(arrayfun(@(r) numel(r.q), R));
total = sum(arrayfun(@(r) numel(r.q), R));
fprintf('\nMAX_FIR_TAPS = %d; coeficientes totales = %d (%.1f KB en flash)\n', ...
  max_taps, total, total*2/1024);

%% ========================= EXPORTAR filter_coeffs.h ====================

fid = fopen(h_path, 'w');
fprintf(fid, '/*\n * filter_coeffs.h\n *\n');
fprintf(fid, ' * GENERADO AUTOMATICAMENTE por disenio_filtros_tp2.m: no editar a mano.\n');
fprintf(fid, ' * Filtros FIR por ventaneo (Kaiser), coeficientes en Q15.\n');
fprintf(fid, ' * Longitud par por requisito de arm_fir_init_q15 (ultimo tap = 0\n');
fprintf(fid, ' * cuando el diseno era impar).\n *\n');
fprintf(fid, ' * Presupuesto de CPU: %.0f%% a %.0f MHz, estimando %g ciclos/tap\n', ...
  CPU_MAX*100, F_CPU/1e6, CICLOS_POR_TAP);
fprintf(fid, ' * (taps max = CPU_MAX * F_CPU / (ciclos/tap * Fs)). La transicion df\n');
fprintf(fid, ' * nominal se ensancha solo si el filtro no entra en ese presupuesto;\n');
fprintf(fid, ' * el Astop pedido no se relaja.\n *\n');
fprintf(fid, ' * filtro  Fs     taps/max   df nom->usado [Hz]  CPU est  Astop req/real[dB]  ripple[dB]  retardo  estado\n');
for k = 1:numel(R)
  r = R(k);
  fprintf(fid, ' * %-6s  %-5s %5d/%-5d  %9s -> %-9s %5.0f%%     %4.0f / %5.1f        %5.2f   %6.1fms  %s\n', ...
    r.key, r.fs_label, numel(r.q), r.taps_max, dfstr(r.df_nom), dfstr(r.df), cpu_pct(r), ...
    r.astop_req, r.astop_q, r.ripple_q, (r.n - 1) / 2 / r.fs * 1e3, r.estado);
end
if ~isempty(nv)
  fprintf(fid, ' *\n * NO VIABLE = ni con la transicion mas ancha posible (cada banda conserva\n');
  fprintf(fid, ' * >= %g Hz) el FIR por ventaneo entra en el presupuesto a esa Fs.\n', ANCHO_MIN_ZONA);
  if strcmp(SI_NO_ENTRA, 'recortar')
    fprintf(fid, ' * Se exporta el diseno de taps max: entra en el presupuesto pero NO cumple Astop.\n');
  else
    fprintf(fid, ' * Se exporta el diseno que cumple Astop: EXCEDE el presupuesto de CPU.\n');
  end
  fprintf(fid, ' * Alternativa a evaluar (cambio de firmware): IIR biquad\n');
  fprintf(fid, ' * (arm_biquad_cascade_df1_q15), unas pocas secciones de 2do orden.\n');
end
fprintf(fid, ' */\n\n#ifndef FILTER_COEFFS_H\n#define FILTER_COEFFS_H\n\n');
fprintf(fid, '#include "arm_math.h"\n\n');
fprintf(fid, '#define MAX_FIR_TAPS %d\n\n', max_taps);

% Definiciones que usa processing_stage.c (el tamaño de bloque y el buffer de
% estado se definen allí, junto con PROCESSING_BLOCK_SIZE)
fprintf(fid, 'typedef struct {\n    const q15_t *pCoeffs;\n    uint16_t numTaps;\n} fir_filter_config_t;\n\n');

for i = 1:numel(filt)
  fprintf(fid, '/* ---- %s ---- */\n', filt(i).desc);
  for k = find(strcmp({R.key}, filt(i).key))
    r = R(k);
    fprintf(fid, 'static const q15_t coeffs_%s_%s[%d] = {\n', r.key, r.fs_label, numel(r.q));
    for m = 1:12:numel(r.q)
      fila = r.q(m:min(m+11, numel(r.q)));
      fprintf(fid, '    %s', sprintf('%6d, ', fila));
      fprintf(fid, '\n');
    end
    fprintf(fid, '};\n\n');
  end
end

% Tablas de configuración, con los campos de fir_filter_config_t
fprintf(fid, '/* Tablas de configuracion. Orden de las Fs: %s */\n', strjoin(fs_label, ', '));
for i = 1:numel(filt)
  key = filt(i).key;
  fprintf(fid, 'static const fir_filter_config_t fir_config_%s[] = {\n', key);
  for j = 1:numel(fs_label)
    nombre = sprintf('coeffs_%s_%s', key, fs_label{j});
    fprintf(fid, '    { .pCoeffs = %s, .numTaps = sizeof(%s) / sizeof(%s[0]) },\n', ...
      nombre, nombre, nombre);
  end
  fprintf(fid, '};\n\n');
end
fprintf(fid, '#endif /* FILTER_COEFFS_H */\n');
fclose(fid);
fprintf('Exportado: %s\n', h_path);

%% ============================== GRÁFICAS ===============================

if GRAFICAR
  graf_dir = fullfile(out_dir, 'graficas');
  if ~exist(graf_dir, 'dir'), mkdir(graf_dir); end
  for i = 1:numel(filt)
    idx = find(strcmp({R.key}, filt(i).key));
    fig = figure('Position', [100 100 900 220*numel(idx)]);
    for p = 1:numel(idx)
      r = R(idx(p));
      Hf = respdB(r.h);      Hf = Hf(2:NFFT/2+1);
      Hq = respdB(r.q/32768); Hq = Hq(2:NFFT/2+1);
      fp = (1:NFFT/2)' * r.fs / NFFT;
      subplot(numel(idx), 1, p);
      semilogx(fp, Hf, 'b', 'LineWidth', 1.2); hold on;
      semilogx(fp, Hq, 'm--', 'LineWidth', 0.8);
      for z = 1:size(r.zonas, 1)
        a = max(r.zonas(z,1), 1);  b = r.zonas(z,2);
        if r.es_paso(z)
          plot([a b], [0 0], 'g', 'LineWidth', 2);
        else
          plot([a b], -r.astop_req*[1 1], 'r', 'LineWidth', 2);
        end
      end
      for c = r.cutoff
        plot([c c], [-80 5], 'k:');
      end
      xlim([1 r.fs/2]);  ylim([-80 5]);  grid on;
      ylabel('|H| [dB]');
      title(sprintf('%s  Fs = %d Hz  -  %d taps, df = %s Hz, Astop real %.1f dB (Q15) - %s', ...
        r.desc, r.fs, numel(r.q), dfstr(r.df), r.astop_q, r.estado));
      if p == 1, legend('float', 'Q15', 'Location', 'southeast'); end
    end
    xlabel('Frecuencia [Hz]');
    saveas(fig, fullfile(graf_dir, ['respuesta_' filt(i).key '.png']));
  end
end
