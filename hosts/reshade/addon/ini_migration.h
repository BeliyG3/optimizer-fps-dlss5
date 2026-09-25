#pragma once

#include "core/api/ofps_settings_schema.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ofps::reshade {

struct MigrationResult {
    bool oldSection = false;
    bool newSection = false;
    std::vector<std::pair<std::string, std::string>> keys;
    bool ShouldMigrate() const { return oldSection && !newSection; }
};

inline bool ReadIniForMigration(const std::filesystem::path &path,
                                std::string &bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>{file},
                 std::istreambuf_iterator<char>{});
    return !file.bad();
}

inline std::string_view TrimIniText(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

inline bool IsIniSection(std::string_view line, std::string_view name) {
    line = TrimIniText(line);
    return line.size() == name.size() + 2u && line.front() == '[' &&
           line.back() == ']' && line.substr(1, name.size()) == name;
}

inline bool IsAnyIniSection(std::string_view line) {
    line = TrimIniText(line);
    return line.size() >= 2u && line.front() == '[' && line.back() == ']';
}

inline bool KnownMigrationKey(std::string_view key) {
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
        if (id != OFPS_SET_DEBUG_LAYER && key == kOfpsSettings[id].iniKey)
            return true;
    constexpr std::array<std::string_view, 5> shell = {
        "Passive", "FloatingWindow", "TraceExit", "DebugLayer", "CrashGuard"
    };
    for (std::string_view name : shell)
        if (key == name) return true;
    return false;
}

inline MigrationResult ScanIniForMigration(std::string_view input) {
    MigrationResult result{};
    bool readingOld = false;
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t next = input.find('\n', pos);
        const std::size_t end = next == std::string_view::npos ? input.size() : next + 1u;
        std::string_view line = input.substr(pos, end - pos);
        if (pos == 0 && line.substr(0, 3) == "\xef\xbb\xbf") line.remove_prefix(3);
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (IsAnyIniSection(line)) {
            readingOld = IsIniSection(line, "PeripheralWarp");
            result.oldSection |= readingOld;
            result.newSection |= IsIniSection(line, "OptimizerFPS");
            pos = end;
            continue;
        }
        if (!readingOld) { pos = end; continue; }
        const std::string_view trimmed = TrimIniText(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            pos = end;
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = TrimIniText(trimmed.substr(0, eq));
            if (KnownMigrationKey(key))
                result.keys.emplace_back(key, TrimIniText(trimmed.substr(eq + 1)));
        }
        pos = end;
    }
    return result;
}

// Complete all API reads before the first cache mutation. The adapters are also used by tests.
template <class Read, class Prepare, class Write>
bool PreflightAndMigrate(const MigrationResult &scan, Read &&read,
                         Prepare &&prepare, Write &&write,
                         std::string &failedKey) {
    for (const auto &[key, expected] : scan.keys) {
        std::string actual;
        if (!read(key, actual) || actual != expected) {
            failedKey = key;
            return false;
        }
    }
    if (!prepare()) {
        failedKey = "backup";
        return false;
    }
    for (const auto &[key, value] : scan.keys) write(key, value);
    return true;
}

} // namespace ofps::reshade
