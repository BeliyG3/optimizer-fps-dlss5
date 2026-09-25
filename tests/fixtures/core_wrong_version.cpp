#include "core/api/ofps_core.h"
#include <cstring>
extern "C" uint32_t OfpsCoreVersion(OfpsVersion *out) {
    if (out && out->size >= sizeof(*out)) {
        out->abi = 1; std::memcpy(out->release, "2026.9.0", 9);
    }
    return 1;
}
extern "C" int OfpsCreateCore(uint32_t, IOfpsHost *, IOfpsCore **) { return OFPS_E_STATE; }
