#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

// --set-at F,ID,VALUE: frame F sets one add-on setting (OfpsSettingId ID, integer VALUE) through the add-on's
// OptimizerFpsSetSettingV1 export, as the overlay would while the game sits in a menu (menu mode, Task 12 fix round 1).
// Returns the export's status: 0 applied; the export refuses diagnostic settings; 1 also when the export is missing.
inline std::uint32_t SetAddonSettingAt(int frame, std::uint32_t id, std::int32_t value)
{
    union Value { std::int32_t i; float f; };
    using SetSetting = std::uint32_t (*)(std::uint32_t, Value);
    HMODULE addon = GetModuleHandleW(L"optimizer-fps-dlss5.addon64");
    const auto set = addon ? reinterpret_cast<SetSetting>(GetProcAddress(addon, "OptimizerFpsSetSettingV1")) : nullptr;
    if (!set) { std::printf("[set] frame %d: the add-on's OptimizerFpsSetSettingV1 export is MISSING\n", frame); return 1; }
    Value v{}; v.i = value;
    const std::uint32_t status = set(id, v);
    std::printf("[set] frame %d: setting %u = %d (status %u)\n", frame, unsigned(id), int(value), unsigned(status));
    return status;
}
