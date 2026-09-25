#include "ini_section.h"
#include "hosts/reshade/addon/ini_migration.h"

#include <reshade.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ofps::remote {

const char *ActiveIniSectionReadOnly() {
    std::size_t length = 0;
    ::reshade::get_reshade_base_path(nullptr, &length);
    if (length == 0) {
        ::reshade::log::message(::reshade::log::level::error,
                              "Optimizer FPS: x86 ReShade.ini section scan failed");
        return "OptimizerFPS";
    }
    std::vector<char> base(length);
    ::reshade::get_reshade_base_path(base.data(), &length);
    if (length == 0 || base.back() != '\0') {
        ::reshade::log::message(::reshade::log::level::error,
                              "Optimizer FPS: x86 ReShade.ini section scan failed");
        return "OptimizerFPS";
    }
    std::ifstream file(std::filesystem::path(
                           std::u8string(reinterpret_cast<const char8_t *>(base.data()))) /
                           "ReShade.ini",
                       std::ios::binary);
    if (!file) {
        ::reshade::log::message(::reshade::log::level::error,
                              "Optimizer FPS: x86 ReShade.ini section scan failed");
        return "OptimizerFPS";
    }
    const std::string bytes(std::istreambuf_iterator<char>{file},
                            std::istreambuf_iterator<char>{});
    if (file.bad()) {
        ::reshade::log::message(::reshade::log::level::error,
                              "Optimizer FPS: x86 ReShade.ini section scan failed");
        return "OptimizerFPS";
    }
    const auto scan = ofps::reshade::ScanIniForMigration(bytes);
    return scan.newSection || !scan.oldSection ? "OptimizerFPS" : "PeripheralWarp";
}

} // namespace ofps::remote
