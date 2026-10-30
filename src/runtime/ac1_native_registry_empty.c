/* Leaf-only standalone contracts have no generated region registry. Keep this
 * in a separate TU and link it only into those fixtures. Main game builds use
 * their generated registry; fixtures with their own registry omit this file. */
#include "ac1_native.h"
ac1_native_region *const ac1_native_regions[] = {NULL};
const size_t ac1_native_region_count = 0;
