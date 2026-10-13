/*
 * fsl_common.h (STUB solo para el test en PC)
 *
 * Reemplaza al fsl_common.h del SDK únicamente en la compilación de host de ui.c.
 * Nunca se usa en el build del firmware.
 */
#ifndef FSL_COMMON_H_
#define FSL_COMMON_H_

#include <stdint.h>

typedef int32_t status_t;
#define kStatus_Success 0

/* En PC no hay interrupciones; se cuenta la cantidad de secciones críticas abiertas/cerradas
 * para verificar que están balanceadas. */
extern int stub_irq_depth;
static inline uint32_t DisableGlobalIRQ(void) {
    stub_irq_depth++;
    return 0u;
}
static inline void EnableGlobalIRQ(uint32_t primask) {
    (void)primask;
    stub_irq_depth--;
}

#endif /* FSL_COMMON_H_ */

