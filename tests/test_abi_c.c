#include "ofps_core.h"
#include "ofps_settings_schema.h"
#include "test_abi_pod_list.h"
#include <stddef.h>
uint32_t OfpsAbiSizesInC(uint32_t *out, uint32_t capacity) {
#define OFPS_ABI_ENTRY(expression) (uint32_t)(expression),
static const uint32_t sizes[] = { OFPS_ABI_POD_LIST(OFPS_ABI_ENTRY) };
#undef OFPS_ABI_ENTRY
const uint32_t count = (uint32_t)(sizeof(sizes) / sizeof(sizes[0])); uint32_t i; for (i = 0; i < count && i < capacity; ++i) out[i] = sizes[i]; return count; } uint32_t OfpsAbiConstantsInC(void) { return (uint32_t)OFPS_ABI_VERSION + (uint32_t)OFPS_SET_COUNT * 1000u + (uint32_t)OFPS_GROUP_COUNT * 1000000u; }
