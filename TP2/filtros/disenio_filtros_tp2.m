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

%% ======================= PARÁMETROS DE DISEÑO ==========================

% Frecuencias de muestreo: etiqueta usada en los nombres C y Fs real [Hz].
% Deben coincidir con s_sample_rate_hz en adc_stage.c, en el mismo orden
% que sample_rate_t (adc_stage.h).
fs_label = {'8k', '16k', '22k', '44k', '48k'};
fs_list  = [8000, 16000, 22000, 44000, 48000];

% Convención: en el método de ventaneo la frecuencia de corte es el punto
% de -6 dB (ganancia 0.5), en el centro de la banda de transición.
% df = ancho de la banda de transición (de Fpass a Fstop) en Hz.
%
% LP: Fc = 3600 Hz, transición 3400-3800 Hz (debe caber bajo 4000 Hz,
%     el Nyquist de Fs = 8 kHz).
% HP: Fc = 35 Hz, rechazo por debajo de 10 Hz (infrasónico), paso desde 60 Hz.
% BP: bordes iguales a los del HP (35 Hz) y a un LP en 3500 Hz.
% BS: Fr = 50 Hz, BW = 15 Hz -> la banda 42.5-57.5 Hz debe quedar atenuada
%     >= 25 dB. Con transiciones de 20 Hz los cortes (-6 dB) quedan en
%     32.5 y 67.5 Hz, y las bandas de paso terminan/empiezan en 22.5/77.5 Hz.
filt(1) = struct('key','lp', 'desc','Pasa bajos',    'ftype','low', ...
                 'cutoff',3600,        'df',400,      'astop',30, 'pass_zero',true);
filt(2) = struct('key','hp', 'desc','Pasa altos',    'ftype','high', ...
                 'cutoff',35,          'df',50,       'astop',30, 'pass_zero',false);
filt(3) = struct('key','bp', 'desc','Pasa banda',    'ftype','bandpass', ...
                 'cutoff',[35 3500],   'df',[50 400], 'astop',30, 'pass_zero',false);
filt(4) = struct('key','bs', 'desc','Elimina banda', 'ftype','stop', ...
                 'cutoff',[32.5 67.5], 'df',[20 20],  'astop',25, 'pass_zero',true);

% Normalizar para que el pico de ganancia en banda de paso sea 0 dB.
% El ripple del ventaneo (+-delta, delta = 10^(-Astop/20)) haría que la
% ganancia supere 1 levemente; en Q15 eso satura señales de amplitud máxima.
NORMALIZAR_PICO_0DB = true;

% arm_fir_init_q15 exige numTaps par y >= 4. Los diseños son de longitud
% impar (tipo I: fase lineal y retardo entero, obligatorio para HP y BS),
% así que se agrega un coeficiente 0 al final.
PAD_PAR_CMSIS_Q15 = true;

% Estimación de carga con arm_fir_q15 y blockSize = 1 (MAC + desplazamiento
% del buffer de estados). Es una ESTIMACIÓN: medir en la placa con DWT->CYCCNT.
CICLOS_POR_TAP = 3;
F_CPU = 150e6;            % Cortex-M33 de la MCXN947

NFFT = 2^20;              % resolución de la verificación: Fs/NFFT
GRAFICAR = true;
out_dir = fileparts(mfilename('fullpath'));
if isempty(out_dir), out_dir = pwd; end
% El .h se escribe directamente en el proyecto del firmware
h_path = fullfile(out_dir, '..', 'MCXN947_Project_TP2', 'source', 'filter_coeffs.h');

%% ============================== DISEÑO =================================

respdB = @(h) 20*log10(max(abs(fft(h(:), NFFT)), 1e-12));
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

  for j = 1:numel(fs_list)
    Fs = fs_list(j);
    f  = (0:NFFT/2)' * Fs / NFFT;
    Wn = F.cutoff / (Fs/2);                 % normalizado a Nyquist

    % Bandas de paso y de rechazo a partir de cortes y transiciones
    lo = F.cutoff - F.df/2;
    hi = F.cutoff + F.df/2;
    pts = [0, reshape([lo; hi], 1, []), Fs/2];
    zonas = reshape(pts, 2, [])';           % cada fila: [desde hasta]
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
    n_est = ceil((A - 7.95) / (2.285 * 2*pi * min(F.df) / Fs)) + 1;
    n = n_est + mod(n_est + 1, 2);

    % Subir la longitud hasta cumplir Astop (la fórmula puede subestimar)
    while true
      h = fir1(n-1, Wn, F.ftype, kaiser(n, beta));
      H = respdB(h);  H = H(1:NFFT/2+1);
      if max(H(pmask)) - max(H(smask)) >= A, break; end
      n = n + 2;
    end
    % Bajar mientras siga cumpliendo (o puede sobreestimar)
    while n > 5
      h2 = fir1(n-3, Wn, F.ftype, kaiser(n-2, beta));
      H2 = respdB(h2);  H2 = H2(1:NFFT/2+1);
      if max(H2(pmask)) - max(H2(smask)) < A, break; end
      n = n - 2;  h = h2;  H = H2;
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
    R(k).cutoff = F.cutoff;  R(k).zonas = zonas;  R(k).es_paso = es_paso;
    R(k).n_est = n_est;      R(k).n = n;
    R(k).h = h;              R(k).q = q;
    R(k).astop_f = -max(Hf(smask));
    R(k).astop_q = -max(Hq(smask));
    R(k).ripple_q = max(Hq(pmask)) - min(Hq(pmask));
    R(k).gmax_q = max(Hq(pmask));
  end
end

%% ============================ RESULTADOS ===============================

fprintf('%-4s %6s %6s %6s %6s %6s %8s %9s %7s %6s %9s %7s %s\n', ...
  'filt', 'Fs', 'N est', 'N', 'taps', 'beta', 'Astop f', 'Astop Q15', ...
  'ripple', 'Gmax', 'retardo', 'CPU est', 'cumple');
for k = 1:numel(R)
  r = R(k);
  cpu = numel(r.q) * CICLOS_POR_TAP * r.fs / F_CPU * 100;
  retardo_ms = (r.n - 1) / 2 / r.fs * 1e3;
  ok = 'NO';  if r.astop_q >= r.astop_req, ok = 'SI'; end
  fprintf('%-4s %6d %6d %6d %6d %6.3f %8.1f %9.1f %7.2f %6.2f %7.1fms %6.0f%% %s\n', ...
    r.key, r.fs, r.n_est, r.n, numel(r.q), r.beta, r.astop_f, r.astop_q, ...
    r.ripple_q, r.gmax_q, retardo_ms, cpu, ok);
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
fprintf(fid, ' * filtro  Fs      taps  beta   Astop req/real[dB]  ripple paso[dB]\n');
for k = 1:numel(R)
  r = R(k);
  fprintf(fid, ' * %-6s  %-6s %5d  %.3f  %4.0f / %5.1f          %.2f\n', ...
    r.key, r.fs_label, numel(r.q), r.beta, r.astop_req, r.astop_q, r.ripple_q);
end
fprintf(fid, ' */\n\n#ifndef FILTER_COEFFS_H\n#define FILTER_COEFFS_H\n\n');
fprintf(fid, '#include "arm_math.h"\n\n');
fprintf(fid, '#define MAX_FIR_TAPS %d\n\n', max_taps);

% Definiciones que usa processing_stage.c
fprintf(fid, '/* Procesamiento muestra a muestra */\n');
fprintf(fid, '#define FIR_BLOCK_SIZE 1\n\n');
fprintf(fid, '/* Con ARM_MATH_DSP (Cortex-M33), arm_fir_init_q15 limpia numTaps + blockSize elementos */\n');
fprintf(fid, '#define FIR_STATE_BUFFER_SIZE (MAX_FIR_TAPS + FIR_BLOCK_SIZE)\n\n');
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
      title(sprintf('%s  Fs = %d Hz  -  %d taps, Astop real %.1f dB (Q15)', ...
        r.desc, r.fs, numel(r.q), r.astop_q));
      if p == 1, legend('float', 'Q15', 'Location', 'southeast'); end
    end
    xlabel('Frecuencia [Hz]');
    saveas(fig, fullfile(graf_dir, ['respuesta_' filt(i).key '.png']));
  end
end
