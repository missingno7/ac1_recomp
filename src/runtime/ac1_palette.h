#ifndef AC1_PALETTE_H
#define AC1_PALETTE_H
#include "ac1_native.h"
int ac1_palette_prepare(const CPU *, ac1_region_plan *);
void ac1_palette_native(CPU *, const ac1_region_plan *);
void ac1_palette_pending(const CPU *, ac1_pending_upload *);
int ac1_palette_should_replay(const CPU *, const ac1_region_plan *, uint32_t);
void ac1_palette_residual_hook(CPU *, const ac1_region_plan *, uint32_t);
#endif
