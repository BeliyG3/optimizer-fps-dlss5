// The add-on's settings persistence, without ReShade (stage 27.T).
//
// hosts/reshade/addon/ini_schema.h is the ReShade-free half of config_store.cpp: the key list, the
// defaults, the clamps and the mapping between [PeripheralWarp] keys and the add-on's settings. Here it
// is bound to an in-memory ini instead of reshade::get_config_value / set_config_value, so what a game's
// ReShade.ini does to the add-on can be checked in ctest: the saved key set, a round trip, a real 26.15
// section full of keys that no longer exist, the clamps and the defaults.

#include "hosts/reshade/addon/ini_schema.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool Near(float lhs, float rhs, float tolerance = 1.0e-4f)
{
    return std::abs(lhs - rhs) <= tolerance;
}

// One [PeripheralWarp] section held in memory, in file order. Get* answers only for keys that are
// present (as ReShade does); Set* overwrites a key in place or appends a new one and never removes
// anything, so keys belonging to somebody else survive a save exactly as they do in a real ReShade.ini.
class MemoryIni {
public:
    MemoryIni() = default;

    // Parses a literal "key=value" section body (CRLF or LF); blank lines and ';' comments are skipped.
    explicit MemoryIni(const std::string &text)
    {
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            entries_.push_back({line.substr(0, eq), line.substr(eq + 1)});
        }
    }

    bool GetInt(const char *key, int &out) const
    {
        const std::string *value = Find(key);
        if (value == nullptr) return false;
        out = std::atoi(value->c_str());
        return true;
    }
    bool GetFloat(const char *key, float &out) const
    {
        const std::string *value = Find(key);
        if (value == nullptr) return false;
        out = static_cast<float>(std::atof(value->c_str()));
        return true;
    }
    void SetInt(const char *key, int value) { Put(key, std::to_string(value)); }
    void SetFloat(const char *key, float value)
    {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        Put(key, buffer);
    }

    bool Has(const char *key) const { return Find(key) != nullptr; }
    std::string Value(const char *key) const
    {
        const std::string *value = Find(key);
        return value != nullptr ? *value : std::string();
    }
    std::vector<std::string> Keys() const
    {
        std::vector<std::string> keys;
        for (const Entry &entry : entries_) keys.push_back(entry.key);
        return keys;
    }

private:
    struct Entry {
        std::string key;
        std::string value;
    };

    const std::string *Find(const char *key) const
    {
        for (const Entry &entry : entries_)
            if (entry.key == key) return &entry.value;
        return nullptr;
    }
    void Put(const char *key, std::string value)
    {
        for (Entry &entry : entries_) {
            if (entry.key == key) {
                entry.value = std::move(value);
                return;
            }
        }
        entries_.push_back({key, std::move(value)});
    }

    std::vector<Entry> entries_;
};

bool SameSettings(const ofps::reshade::AddonPersisted &lhs, const ofps::reshade::AddonPersisted &rhs)
{
    return lhs.showCenterOutline == rhs.showCenterOutline &&
           lhs.showWorkOutline == rhs.showWorkOutline &&
           lhs.workShiftEnabled == rhs.workShiftEnabled &&
           Near(lhs.brightnessPercent, rhs.brightnessPercent) &&
           Near(lhs.gamma, rhs.gamma) &&
           lhs.temporal.modelPasses == rhs.temporal.modelPasses &&
           lhs.temporal.spreadPasses == rhs.temporal.spreadPasses &&
           lhs.temporal.mode == rhs.temporal.mode &&
           lhs.temporal.every == rhs.temporal.every &&
           lhs.temporal.maxQueue == rhs.temporal.maxQueue;
}

bool SameConfig(const ofps::sdk::ConfigV2 &lhs, const ofps::sdk::ConfigV2 &rhs)
{
    return lhs.mode == rhs.mode && lhs.colorFilter == rhs.colorFilter && lhs.flags == rhs.flags &&
           Near(lhs.xAxis.centerPercent, rhs.xAxis.centerPercent) &&
           Near(lhs.xAxis.workPercent, rhs.xAxis.workPercent) &&
           Near(lhs.yAxis.centerPercent, rhs.yAxis.centerPercent) &&
           Near(lhs.yAxis.workPercent, rhs.yAxis.workPercent) &&
           Near(lhs.globalScalePercent, rhs.globalScalePercent) &&
           Near(lhs.centerOffsetXPercent, rhs.centerOffsetXPercent) &&
           Near(lhs.centerOffsetYPercent, rhs.centerOffsetYPercent) &&
           Near(lhs.workShiftXPercent, rhs.workShiftXPercent) &&
           Near(lhs.workShiftYPercent, rhs.workShiftYPercent);
}

// A layout that differs from the default in every persisted field and still validates. The mode stays
// Peripheral because that is the only mode with centre offsets and Work shift; Uniform is covered by
// the 26.15 fixture below.
ofps::reshade::AddonPersisted NonDefaultPersisted()
{
    ofps::reshade::AddonPersisted persisted;
    persisted.config.mode = ofps::sdk::WarpMode::Peripheral;
    persisted.config.colorFilter = ofps::sdk::ColorFilter::Bilinear;
    persisted.config.xAxis = {70.0f, 85.0f};
    persisted.config.yAxis = {60.0f, 75.0f};
    persisted.config.globalScalePercent = 90.0f;
    persisted.config.flags = 0;
    persisted.config.centerOffsetXPercent = 2.5f;
    persisted.config.centerOffsetYPercent = -1.5f;
    persisted.config.workShiftXPercent = 1.25f;
    persisted.config.workShiftYPercent = -0.75f;
    persisted.showCenterOutline = true;
    persisted.showWorkOutline = true;
    persisted.workShiftEnabled = true;
    persisted.brightnessPercent = -3.5f;
    persisted.gamma = 1.15f;
    persisted.temporal.modelPasses = 3;
    persisted.temporal.spreadPasses = false;
    persisted.temporal.mode = 3;
    persisted.temporal.every = 4;
    persisted.temporal.maxQueue = 1;
    return persisted;
}

// (a) A save writes exactly kIniKeys[] - no read-only key, no Debug* diagnostic, nothing left out.
void TestSavedKeySet()
{
    Check(std::size(ofps::reshade::kIniKeys) == 22, "the add-on persists 22 keys");

    MemoryIni ini;
    ofps::reshade::SaveToStore(ini, NonDefaultPersisted());
    std::vector<std::string> written = ini.Keys();
    std::vector<std::string> declared(std::begin(ofps::reshade::kIniKeys), std::end(ofps::reshade::kIniKeys));
    Check(written.size() == declared.size(), "a save writes as many keys as kIniKeys declares");
    std::sort(written.begin(), written.end());
    std::sort(declared.begin(), declared.end());
    Check(written == declared, "the written key set is exactly kIniKeys");
    Check(std::adjacent_find(written.begin(), written.end()) == written.end(),
          "no key is written twice");
    for (const std::string &key : written)
        Check(key.rfind("Debug", 0) != 0, "no Debug* diagnostic key is ever written");
    for (const char *readOnly : {"CrashGuard", "FloatingWindow", "Passive", "TraceExit", "DebugLayer"})
        Check(!ini.Has(readOnly), "read-only keys are never written back");
}

// (b) Save then load returns the same settings.
void TestRoundTrip()
{
    const ofps::reshade::AddonPersisted saved = NonDefaultPersisted();
    MemoryIni ini;
    ofps::reshade::SaveToStore(ini, saved);

    ofps::reshade::AddonPersisted loaded;
    Check(ofps::reshade::LoadFromStore(ini, loaded), "a saved layout is seen on load");
    Check(SameConfig(loaded.config, saved.config), "the layout survives the round trip");
    Check(SameSettings(loaded, saved), "outlines, colour, temporal survive the round trip");
    Check(ofps::sdk::ValidateConfig(loaded.config) == ofps::sdk::Status::Ok, "the round-tripped layout still validates");
}

// (c) A real 26.15 section: keys that no longer exist are ignored on load and survive a save.
void TestLegacySection()
{
    const std::string legacy =
        "[PeripheralWarp]\r\n"
        "Mode=1\r\n"
        "ColorFilter=1\r\n"
        "CenterX=75\r\n"
        "WorkX=88\r\n"
        "CenterY=75\r\n"
        "WorkY=88\r\n"
        "GlobalScale=95\r\n"
        "Flags=1\r\n"
        "OffsetX=0\r\n"
        "OffsetY=0\r\n"
        "WorkShiftX=0\r\n"
        "WorkShiftY=0\r\n"
        "ShowCenterOutline=1\r\n"
        "ShowWorkOutline=0\r\n"
        "WorkShiftEnabled=1\r\n"
        "Brightness=2.5\r\n"
        "Gamma=1.05\r\n"
        "TemporalMode=1\r\n"
        "TemporalEvery=3\r\n"
        "TemporalMaxQueue=2\r\n"
        "TemporalFeather=3.5\r\n"
        "TemporalSeparateZone=1\r\n"
        "TemporalToneMatch=1\r\n"
        "TemporalToneSmoothing=0.6\r\n"
        "TemporalCatmullRom=1\r\n"
        "TemporalHoleFill=1\r\n"
        "TemporalWarpBase=1\r\n"
        "TemporalMaxMotion=120\r\n"
        "TemporalResidualBlend=0\r\n"
        "TemporalShowZone=0\r\n"
        "MotionScale=1\r\n"
        "MotionInvert=1\r\n"
        "TemporalCenterX=53\r\n"
        "DebugTiming=1\r\n"
        "CrashGuard=1\r\n";

    MemoryIni ini(legacy);
    ofps::reshade::AddonPersisted loaded;
    Check(ofps::reshade::LoadFromStore(ini, loaded), "the 26.15 section is recognised as a saved layout");
    Check(loaded.config.mode == ofps::sdk::WarpMode::Uniform &&
              loaded.config.colorFilter == ofps::sdk::ColorFilter::AdaptiveFourTap,
          "26.15 Mode and ColorFilter are read");
    Check(Near(loaded.config.xAxis.centerPercent, 75.0f) && Near(loaded.config.xAxis.workPercent, 88.0f) &&
              Near(loaded.config.globalScalePercent, 95.0f),
          "26.15 axis and Global scale are read");
    Check(loaded.showCenterOutline && !loaded.showWorkOutline && loaded.workShiftEnabled,
          "26.15 outlines and Work shift are read");
    Check(Near(loaded.brightnessPercent, 2.5f) && Near(loaded.gamma, 1.05f), "26.15 colour is read");
    Check(loaded.temporal.mode == 1 && loaded.temporal.every == 3 && loaded.temporal.maxQueue == 2,
          "26.15 temporal settings are read");
    Check(ofps::sdk::ValidateConfig(loaded.config) == ofps::sdk::Status::Ok, "the 26.15 layout validates");
    // The withdrawn keys are not settings any more: nothing of them reaches the add-on.
    Check(loaded.temporal.mode == 1, "withdrawn Temporal keys do not override the cadence");

    // Only the model-pass keys are added to this legacy section.
    const std::size_t before = ini.Keys().size();
    ofps::reshade::SaveToStore(ini, loaded);
    const std::vector<std::string> after = ini.Keys();
    Check(after.size() == before + 2, "a save adds the model-pass keys and removes none");
    for (const char *foreign : {"TemporalFeather", "TemporalSeparateZone", "TemporalToneMatch",
                                "TemporalToneSmoothing", "TemporalCatmullRom", "TemporalHoleFill",
                                "TemporalWarpBase", "TemporalMaxMotion", "TemporalResidualBlend",
                                "TemporalShowZone", "MotionScale", "MotionInvert", "TemporalCenterX",
                                "DebugTiming", "CrashGuard"})
        Check(ini.Has(foreign), "a save leaves the keys it does not own untouched");
    Check(ini.Value("MotionInvert") == "1" && ini.Value("DebugTiming") == "1",
          "the values of foreign keys are not rewritten");
    Check(!ini.Has("OptiScalerTakeover"), "the obsolete takeover key is never added");
}

// (d) The ini's temporal mode, sanitised (mode 2 was withdrawn in 26.26).
void TestTemporalMode()
{
    Check(ofps::reshade::TemporalModeFromIni(2) == 1, "TemporalMode=2 falls back to Interpolate (sync)");
    Check(ofps::reshade::TemporalModeFromIni(3) == 3, "TemporalMode=3 passes through");
    Check(ofps::reshade::TemporalModeFromIni(0) == 0 && ofps::reshade::TemporalModeFromIni(1) == 1,
          "TemporalMode 0 and 1 pass through");
    Check(ofps::reshade::TemporalModeFromIni(-1) == 0, "a negative TemporalMode clamps to every frame");
    Check(ofps::reshade::TemporalModeFromIni(7) == 1, "a too large TemporalMode clamps to Interpolate (sync)");

    MemoryIni ini("TemporalMode=2\nTemporalEvery=2\n");
    ofps::reshade::AddonPersisted loaded;
    ofps::reshade::LoadFromStore(ini, loaded);
    Check(loaded.temporal.mode == 1, "an ini with the withdrawn mode 2 loads as mode 1");
}

// (e) The clamps applied on load.
void TestClamps()
{
    {
        MemoryIni ini("TemporalEvery=99\nTemporalMaxQueue=99\n");
        ofps::reshade::AddonPersisted loaded;
        Check(!ofps::reshade::LoadFromStore(ini, loaded), "temporal keys alone are not a saved layout");
        Check(loaded.temporal.every == 8 && loaded.temporal.maxQueue == 8,
              "TemporalEvery / TemporalMaxQueue clamp to 8");
    }
    {
        MemoryIni ini("TemporalEvery=0\nTemporalMaxQueue=-4\n");
        ofps::reshade::AddonPersisted loaded;
        ofps::reshade::LoadFromStore(ini, loaded);
        Check(loaded.temporal.every == 1 && loaded.temporal.maxQueue == 0,
              "TemporalEvery clamps to 1 and TemporalMaxQueue to 0");
    }
    {
        MemoryIni ini("Brightness=999\nGamma=99\n");
        ofps::reshade::AddonPersisted loaded;
        ofps::reshade::LoadFromStore(ini, loaded);
        Check(Near(loaded.brightnessPercent, 20.0f) && Near(loaded.gamma, 1.4f),
              "Brightness clamps to +-20 percent and Gamma to 0.7..1.4");
    }
    {
        MemoryIni ini("Brightness=-999\nGamma=0\n");
        ofps::reshade::AddonPersisted loaded;
        ofps::reshade::LoadFromStore(ini, loaded);
        Check(Near(loaded.brightnessPercent, -20.0f), "Brightness clamps at -20 percent");
        Check(Near(loaded.gamma, 1.0f), "Gamma=0 is rejected and the default stays");
    }
    {
        // The layout itself is not clamped on load: an out-of-range Mode reaches ValidateConfig,
        // which rejects the whole section, and the add-on keeps its defaults (LoadPersistedConfigOnce).
        MemoryIni ini("Mode=5\nCenterX=80\n");
        ofps::reshade::AddonPersisted loaded;
        Check(ofps::reshade::LoadFromStore(ini, loaded), "an out-of-range Mode still counts as a layout key");
        Check(static_cast<int>(loaded.config.mode) == 5, "Mode is read verbatim, not clamped");
        Check(ofps::sdk::ValidateConfig(loaded.config) != ofps::sdk::Status::Ok,
              "an out-of-range Mode makes the saved layout invalid, so the defaults stay active");
    }
}

// (f) An ini without a [PeripheralWarp] section leaves every default in place.
void TestDefaults()
{
    MemoryIni ini("SomeoneElsesKey=7\n");
    ofps::reshade::AddonPersisted loaded;
    Check(!ofps::reshade::LoadFromStore(ini, loaded), "an ini without our keys reports no saved layout");
    const ofps::reshade::AddonPersisted defaults;
    Check(SameConfig(loaded.config, defaults.config) && SameConfig(loaded.config, ofps::sdk::DefaultConfigV2()),
          "the layout stays at ofps::sdk::DefaultConfigV2()");
    Check(SameSettings(loaded, defaults), "outlines, colour, temporal stay at their defaults");
    Check(!defaults.showCenterOutline && !defaults.showWorkOutline &&
              !defaults.workShiftEnabled && Near(defaults.brightnessPercent, 0.0f) && Near(defaults.gamma, 1.0f),
          "the defaults mirror AddonState");
}

void TestObsoleteOwnershipKeys()
{
    for (const char *key : {"OptiScalerTakeover", "ForceBridgeWarpOff"}) {
        for (int value : {0, 1}) {
            MemoryIni ini(std::string(key) + "=" + std::to_string(value) + "\n");
            auto loaded = NonDefaultPersisted();
            const auto expected = loaded;
            // LoadTemporalFromStore resets absent temporal keys to their defaults.
            loaded.temporal = {};
            auto baseline = expected;
            baseline.temporal = {};
            Check(!ofps::reshade::LoadFromStore(ini, loaded), "obsolete keys are not layout keys");
            Check(SameSettings(loaded, baseline) && SameConfig(loaded.config, baseline.config),
                  "obsolete ownership values do not change persisted settings");
            ofps::reshade::SaveToStore(ini, loaded);
            Check(ini.Value(key) == std::to_string(value), "obsolete keys remain untouched");
            MemoryIni output;
            ofps::reshade::SaveToStore(output, loaded);
            Check(!output.Has(key), "obsolete keys are not written back");
        }
    }
}

void TestAbiRoundTrip() {
    MemoryIni input("TemporalMode=2\nTemporalEvery=99\nDebugTiming=1\nCenterX=80\n");
    auto values = ofps::reshade::SchemaDefaults();
    Check(ofps::reshade::LoadValuesFromStore(input, values), "schema reads supplied keys");
    Check((values.explicitMask[0] & (1ull << OFPS_SET_TEMPORAL_EVERY)) != 0, "explicit mask tracks supplied cadence");
#ifdef OFPS_TEST_CORE
    struct Host final : IOfpsHost {
        void Log(OfpsLogLevel, const char *) override {}
        void OnEvent(OfpsEvent, const OfpsEventData *) override {}
    } host;
    IOfpsCore *core = nullptr;
    Check(OfpsCreateCore(OFPS_ABI_VERSION, &host, &core) >= 0, "ini core created");
    if (!core) return;
    Check(core->SetSettings(&values) == OFPS_OK, "ini values accepted");
    core->GetSettings(&values);
    Check(values.v[OFPS_SET_TEMPORAL_MODE].i == 1 && values.v[OFPS_SET_TEMPORAL_EVERY].i == 8, "ini uses core normalization");
#endif
    MemoryIni output;
    ofps::reshade::SaveValuesToStore(output, values);
    auto loaded = ofps::reshade::SchemaDefaults();
    ofps::reshade::LoadValuesFromStore(output, loaded);
    for (uint32_t i=0; i<OFPS_SET_COUNT; ++i) {
        if (kOfpsSettings[i].flags & OFPS_FLAG_PERSISTED)
            Check(loaded.v[i].i == values.v[i].i, "effective persisted value survives ABI round trip");
    }
    Check(loaded.v[OFPS_SET_DEBUG_TIMING].i == 0, "diagnostics are never persisted");
#ifdef OFPS_TEST_CORE
    core->UnregisterHost(&host); core->Release();
#endif
}

} // namespace

void TestIniMigration(const std::string &root);

int main(int argc, char **argv)
{
    Check(argc == 2, "fixture directory is required");
    if (argc == 2) TestIniMigration(argv[1]);
    TestSavedKeySet();
    TestRoundTrip();
    TestLegacySection();
    TestTemporalMode();
    TestClamps();
    TestDefaults();
    TestObsoleteOwnershipKeys();
    TestAbiRoundTrip();
    {
        ofps::reshade::TemporalConfig t;
        ofps::reshade::LoadTemporalFromStore(MemoryIni("ModelPasses=99\nSpreadPasses=0\n"), t);
        Check(t.modelPasses == 3 && !t.spreadPasses, "model passes clamp high and spread reads false");
        ofps::reshade::LoadTemporalFromStore(MemoryIni("ModelPasses=-4\n"), t);
        Check(t.modelPasses == 1 && t.spreadPasses, "model passes clamp low and spread defaults on");
    }
    if (failures == 0) std::cout << "addon ini schema tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
