/*
 * filter_coeffs.h
 *
 * Definiciones de coeficientes y estructuras para filtros FIR (TP2).
 * Actualmente contiene valores placeholder (mock) para permitir la
 * compilación y prueba de la arquitectura.
 */

#ifndef FILTER_COEFFS_H_
#define FILTER_COEFFS_H_

#include "arm_math.h" // Tipo q15_t

// Tamaño de bloque para procesamiento muestra a muestra
#define FIR_BLOCK_SIZE 1

// Número máximo de coeficientes (taps) estimado entre todos los filtros.
// Modificar este valor cuando se tengan los coeficientes reales si superan este tamaño.
#define MAX_FIR_TAPS 128

// Tamaño necesario para el buffer de estados de CMSIS-DSP
#define FIR_STATE_BUFFER_SIZE (MAX_FIR_TAPS + FIR_BLOCK_SIZE - 1)

// ============================================================================
// Estructura descriptora de configuración de filtro
// ============================================================================
typedef struct {
    const q15_t *pCoeffs;
    uint16_t numTaps;
} fir_filter_config_t;

// ============================================================================
// Definiciones de número de coeficientes (Taps) - Mocks temporales
// ============================================================================

// Filtro Pasa Bajos (LP)
#define NUM_TAPS_LP_8K  31
#define NUM_TAPS_LP_16K 31
#define NUM_TAPS_LP_22K 31
#define NUM_TAPS_LP_44K 31
#define NUM_TAPS_LP_48K 31

// Filtro Pasa Altos (HP)
#define NUM_TAPS_HP_8K  31
#define NUM_TAPS_HP_16K 31
#define NUM_TAPS_HP_22K 31
#define NUM_TAPS_HP_44K 31
#define NUM_TAPS_HP_48K 31

// Filtro Pasa Banda (BP)
#define NUM_TAPS_BP_8K  31
#define NUM_TAPS_BP_16K 31
#define NUM_TAPS_BP_22K 31
#define NUM_TAPS_BP_44K 31
#define NUM_TAPS_BP_48K 31

// Filtro Elimina Banda (BS)
#define NUM_TAPS_BS_8K  31
#define NUM_TAPS_BS_16K 31
#define NUM_TAPS_BS_22K 31
#define NUM_TAPS_BS_44K 31
#define NUM_TAPS_BS_48K 31

// ============================================================================
// Arreglos de coeficientes Q15 - Mocks (Filtro Identidad / Impulso unitario)
// ============================================================================
// Como placeholder, usamos un impulso unitario centrado o al inicio para 
// que el filtro se comporte inicialmente como un passthrough retrasado o directo.

// Helper macro para mock
#define MOCK_COEFFS_31 { 32767, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

// Pasa Bajos
static const q15_t coeffs_lp_8k [NUM_TAPS_LP_8K]  = MOCK_COEFFS_31;
static const q15_t coeffs_lp_16k[NUM_TAPS_LP_16K] = MOCK_COEFFS_31;
static const q15_t coeffs_lp_22k[NUM_TAPS_LP_22K] = MOCK_COEFFS_31;
static const q15_t coeffs_lp_44k[NUM_TAPS_LP_44K] = MOCK_COEFFS_31;
static const q15_t coeffs_lp_48k[NUM_TAPS_LP_48K] = MOCK_COEFFS_31;

// Pasa Altos
static const q15_t coeffs_hp_8k [NUM_TAPS_HP_8K]  = MOCK_COEFFS_31;
static const q15_t coeffs_hp_16k[NUM_TAPS_HP_16K] = MOCK_COEFFS_31;
static const q15_t coeffs_hp_22k[NUM_TAPS_HP_22K] = MOCK_COEFFS_31;
static const q15_t coeffs_hp_44k[NUM_TAPS_HP_44K] = MOCK_COEFFS_31;
static const q15_t coeffs_hp_48k[NUM_TAPS_HP_48K] = MOCK_COEFFS_31;

// Pasa Banda
static const q15_t coeffs_bp_8k [NUM_TAPS_BP_8K]  = MOCK_COEFFS_31;
static const q15_t coeffs_bp_16k[NUM_TAPS_BP_16K] = MOCK_COEFFS_31;
static const q15_t coeffs_bp_22k[NUM_TAPS_BP_22K] = MOCK_COEFFS_31;
static const q15_t coeffs_bp_44k[NUM_TAPS_BP_44K] = MOCK_COEFFS_31;
static const q15_t coeffs_bp_48k[NUM_TAPS_BP_48K] = MOCK_COEFFS_31;

// Elimina Banda
static const q15_t coeffs_bs_8k [NUM_TAPS_BS_8K]  = MOCK_COEFFS_31;
static const q15_t coeffs_bs_16k[NUM_TAPS_BS_16K] = MOCK_COEFFS_31;
static const q15_t coeffs_bs_22k[NUM_TAPS_BS_22K] = MOCK_COEFFS_31;
static const q15_t coeffs_bs_44k[NUM_TAPS_BS_44K] = MOCK_COEFFS_31;
static const q15_t coeffs_bs_48k[NUM_TAPS_BS_48K] = MOCK_COEFFS_31;

// ============================================================================
// Tablas de descriptores por frecuencia de muestreo
// Orden de frecuencias (según adc_stage.h): 8K, 16K, 22K, 44K, 48K
// ============================================================================

// Tabla para Filtro Pasa Bajos
static const fir_filter_config_t fir_config_lp[] = {
    { coeffs_lp_8k,  NUM_TAPS_LP_8K  },
    { coeffs_lp_16k, NUM_TAPS_LP_16K },
    { coeffs_lp_22k, NUM_TAPS_LP_22K },
    { coeffs_lp_44k, NUM_TAPS_LP_44K },
    { coeffs_lp_48k, NUM_TAPS_LP_48K }
};

// Tabla para Filtro Pasa Altos
static const fir_filter_config_t fir_config_hp[] = {
    { coeffs_hp_8k,  NUM_TAPS_HP_8K  },
    { coeffs_hp_16k, NUM_TAPS_HP_16K },
    { coeffs_hp_22k, NUM_TAPS_HP_22K },
    { coeffs_hp_44k, NUM_TAPS_HP_44K },
    { coeffs_hp_48k, NUM_TAPS_HP_48K }
};

// Tabla para Filtro Pasa Banda
static const fir_filter_config_t fir_config_bp[] = {
    { coeffs_bp_8k,  NUM_TAPS_BP_8K  },
    { coeffs_bp_16k, NUM_TAPS_BP_16K },
    { coeffs_bp_22k, NUM_TAPS_BP_22K },
    { coeffs_bp_44k, NUM_TAPS_BP_44K },
    { coeffs_bp_48k, NUM_TAPS_BP_48K }
};

// Tabla para Filtro Elimina Banda
static const fir_filter_config_t fir_config_bs[] = {
    { coeffs_bs_8k,  NUM_TAPS_BS_8K  },
    { coeffs_bs_16k, NUM_TAPS_BS_16K },
    { coeffs_bs_22k, NUM_TAPS_BS_22K },
    { coeffs_bs_44k, NUM_TAPS_BS_44K },
    { coeffs_bs_48k, NUM_TAPS_BS_48K }
};

#endif /* FILTER_COEFFS_H_ */

