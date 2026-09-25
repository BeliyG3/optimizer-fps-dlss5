#pragma once
#include "core/api/ofps_core.h"
#include "core/api/ofps_settings_schema.h"
#include "core/context.h"
#include "optimizer_fps/types_v2.h"
#include <cstdint>
namespace ofps::core {
struct StatusStrings {
    char reason[512];
    char temporalReason[160];
    char modelPassReason[192];
    char fallbackReason[128];
};
struct StatusExtras {
    std::uint32_t directHost;
    std::uint32_t deviceRemoved;
};
void FillStatus(const Status &in, const StatusExtras &extras, StatusStrings *strings, OfpsStatus *out);
constexpr std::uint32_t kStatusRowsMax = 48;
constexpr std::uint32_t kStatusRowText = 192;
struct StatusLineBuffer {
    OfpsStatusRow rows[kStatusRowsMax];
    char text[kStatusRowsMax][kStatusRowText];
};
std::uint32_t BuildStatusLines(const Status &in, const TemporalSettings &t, StatusLineBuffer *buffer);
bool BuildLayoutPreview(const ofps::sdk::ConfigV2 &config, std::uint32_t nativeW, std::uint32_t nativeH,
                        OfpsLayoutPreview *out);
} // namespace ofps::core
