#include "ini_section.h"

#include <reshade.hpp>

namespace ofps::remote {

bool GetConfigInt(const char *key, int &value) {
    return ::reshade::get_config_value(nullptr, "OptimizerFPS", key, value) ||
           ::reshade::get_config_value(nullptr, "PeripheralWarp", key, value);
}

} // namespace ofps::remote
