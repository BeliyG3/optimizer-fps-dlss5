#include "hosts/reshade/addon/ini_migration.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

namespace {

using ofps::reshade::MigrationResult;
using ofps::reshade::PreflightAndMigrate;
using ofps::reshade::ScanIniForMigration;

void Require(bool condition) {
    if (!condition) std::abort();
}

std::string Read(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.good());
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

bool HasKey(const MigrationResult &scan, const std::string &name) {
    for (const auto &[key, value] : scan.keys) {
        (void)value;
        if (key == name) return true;
    }
    return false;
}

void TestPreflight(const MigrationResult &scan) {
    Require(scan.keys.size() > 1u);
    std::map<std::string, std::string> source;
    for (const auto &[key, value] : scan.keys) source[key] = value;
    std::map<std::string, std::string> cache;
    std::string failed;
    const std::string lateKey = scan.keys.back().first;
    int writes = 0;
    bool prepared = false;
    const bool failedRead = PreflightAndMigrate(
        scan,
        [&](const std::string &key, std::string &value) {
            if (key == lateKey) return false;
            value = source.at(key);
            return true;
        },
        [&] { prepared = true; return true; },
        [&](const std::string &key, const std::string &value) {
            ++writes;
            cache[key] = value;
        }, failed);
    Require(!failedRead && failed == lateKey && !prepared && writes == 0 && cache.empty());

    failed.clear();
    const bool failedBackup = PreflightAndMigrate(
        scan,
        [&](const std::string &key, std::string &value) {
            value = source.at(key);
            return true;
        },
        [] { return false; },
        [&](const std::string &, const std::string &) { ++writes; }, failed);
    Require(!failedBackup && failed == "backup" && writes == 0);

    failed.clear();
    const bool migrated = PreflightAndMigrate(
        scan,
        [&](const std::string &key, std::string &value) {
            value = source.at(key);
            return true;
        },
        [] { return true; },
        [&](const std::string &key, const std::string &value) {
            ++writes;
            cache[key] = value;
        }, failed);
    Require(migrated && failed.empty() && writes == static_cast<int>(scan.keys.size()));
    Require(cache == source);
}

} // namespace

void TestIniMigration(const std::string &root) {
    std::string missing;
    Require(!ofps::reshade::ReadIniForMigration(root + "/missing.ini", missing));
    std::string readBack;
    Require(ofps::reshade::ReadIniForMigration(root + "/legacy_2615.ini", readBack));
    const std::string old = Read(root + "/legacy_2615.ini");
    Require(readBack == old);
    const MigrationResult first = ScanIniForMigration(old);
    Require(first.ShouldMigrate() && !first.keys.empty());
    Require(HasKey(first, "CrashGuard") && HasKey(first, "TemporalMode"));
    for (const auto &[key, value] : first.keys) {
        Require(key != "OptiScalerTakeover" && key != "ForceBridgeWarpOff");
        Require(key != "DisabledAddons" && !value.empty());
    }
    TestPreflight(first);

    const MigrationResult recent = ScanIniForMigration(Read(root + "/legacy_2628.ini"));
    Require(recent.ShouldMigrate());
    const MigrationResult background = ScanIniForMigration(Read(root + "/legacy_background.ini"));
    Require(background.ShouldMigrate() && HasKey(background, "DebugLayer"));
    Require(!ScanIniForMigration(Read(root + "/new_wins.ini")).ShouldMigrate());
    Require(!ScanIniForMigration(Read(root + "/both_sections.ini")).ShouldMigrate());
    Require(!ScanIniForMigration(Read(root + "/empty_new.ini")).ShouldMigrate());

    const auto empty = ScanIniForMigration(Read(root + "/empty_new.ini"));
    Require(empty.newSection && empty.oldSection);
    const auto none = ScanIniForMigration("[Other]\r\nMode=2\r\n");
    Require(!none.newSection && !none.oldSection && none.keys.empty());
    const auto diagnostic = ScanIniForMigration(
        "\xef\xbb\xbf[PeripheralWarp]\r\nDebugTiming=1\r\n"
        "DisabledAddons=NeverWrite\r\n[PeripheralWarpDLSS]\r\nMode=2\r\n");
    Require(diagnostic.keys.size() == 1u);
    Require(diagnostic.keys[0].first == "DebugTiming");
    Require(diagnostic.keys[0].second == "1");
}
