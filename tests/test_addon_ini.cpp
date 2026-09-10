// The add-on's settings persistence, without ReShade (stage 27.T).
//
// adapters/reshade/addon/ini_schema.h is the ReShade-free half of config_store.cpp: the key list, the
// defaults, the clamps and the mapping between [PeripheralWarp] keys and the add-on's settings. Here it
// is bound to an in-memory ini instead of reshade::get_config_value / set_config_value, so what a game's
// ReShade.ini does to the add-on can be checked in ctest: the saved key set, a round trip, a real 26.15
// section full of keys that no longer exist, the clamps and the defaults.

#include "adapters/reshade/addon/ini_schema.h"

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

bool SameSettings(const pw_addon::AddonPersisted &lhs, const pw_addon::AddonPersisted &rhs)
{
    return lhs.showCenterOutline == rhs.showCenterOutline &&
           lhs.showWorkOutline == rhs.showWorkOutline &&
           lhs.workShiftEnabled == rhs.workShiftEnabled &&
           lhs.optiTakeover == rhs.optiTakeover &&
           Near(lhs.brightnessPercent, rhs.brightnessPercent) &&
           Near(lhs.gamma, rhs.gamma) &&
           lhs.temporal.mode == rhs.temporal.mode &&
           lhs.temporal.every == rhs.temporal.every &&
           lhs.temporal.maxQueue == rhs.temporal.maxQueue;
}

bool SameConfig(const pw::ConfigV2 &lhs, const pw::ConfigV2 &rhs)
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
pw_addon::AddonPersisted NonDefaultPersisted()
{
    pw_addon::AddonPersisted persisted;
    persisted.config.mode = pw::WarpMode::Peripheral;
    persisted.config.colorFilter = pw::ColorFilter::Bilinear;
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
    persisted.optiTakeover = false;
    persisted.brightnessPercent = -3.5f;
    persisted.gamma = 1.15f;
    persisted.temporal.mode = 3;
    persisted.temporal.every = 4;
    persisted.temporal.maxQueue = 1;
    return persisted;
}

// (a) A save writes exactly kIniKeys[] - no read-only key, no Debug* diagnostic, nothing left out.
void TestSavedKeySet()
{
    Check(std::size(pw_addon::kIniKeys) == 21, "the add-on persists 21 keys");

    MemoryIni ini;
    pw_addon::SaveToStore(ini, NonDefaultPersisted());
    std::vector<std::string> written = ini.Keys();
    std::vector<std::string> declared(std::begin(pw_addon::kIniKeys), std::end(pw_addon::kIniKeys));
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
    const pw_addon::AddonPersisted saved = NonDefaultPersisted();
    MemoryIni ini;
    pw_addon::SaveToStore(ini, saved);

    pw_addon::AddonPersisted loaded;
    Check(pw_addon::LoadFromStore(ini, loaded), "a saved layout is seen on load");
    Check(SameConfig(loaded.config, saved.config), "the layout survives the round trip");
    Check(SameSettings(loaded, saved), "outlines, colour, temporal and takeover survive the round trip");
    Check(pw::ValidateConfig(loaded.config) == pw::Status::Ok, "the round-tripped layout still validates");
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
    pw_addon::AddonPersisted loaded;
    Check(pw_addon::LoadFromStore(ini, loaded), "the 26.15 section is recognised as a saved layout");
    Check(loaded.config.mode == pw::WarpMode::Uniform &&
              loaded.config.colorFilter == pw::ColorFilter::AdaptiveFourTap,
          "26.15 Mode and ColorFilter are read");
    Check(Near(loaded.config.xAxis.centerPercent, 75.0f) && Near(loaded.config.xAxis.workPercent, 88.0f) &&
              Near(loaded.config.globalScalePercent, 95.0f),
          "26.15 axis and Global scale are read");
    Check(loaded.showCenterOutline && !loaded.showWorkOutline && loaded.workShiftEnabled,
          "26.15 outlines and Work shift are read");
    Check(Near(loaded.brightnessPercent, 2.5f) && Near(loaded.gamma, 1.05f), "26.15 colour is read");
    Check(loaded.temporal.mode == 1 && loaded.temporal.every == 3 && loaded.temporal.maxQueue == 2,
          "26.15 temporal settings are read");
    Check(loaded.optiTakeover, "a section without OptiScalerTakeover keeps the takeover on");
    Check(pw::ValidateConfig(loaded.config) == pw::Status::Ok, "the 26.15 layout validates");
    // The withdrawn keys are not settings any more: nothing of them reaches the add-on.
    Check(loaded.temporal.holeFill && loaded.temporal.residualCatmullRom && loaded.temporal.warpBase,
          "withdrawn Temporal* keys leave the fixed temporal machine alone");

    // OptiScalerTakeover is the one key of ours the 26.15 section lacks: the save adds it, and nothing else.
    const std::size_t before = ini.Keys().size();
    pw_addon::SaveToStore(ini, loaded);
    const std::vector<std::string> after = ini.Keys();
    Check(after.size() == before + 1, "a save adds only the one missing key and removes none");
    for (const char *foreign : {"TemporalFeather", "TemporalSeparateZone", "TemporalToneMatch",
                                "TemporalToneSmoothing", "TemporalCatmullRom", "TemporalHoleFill",
                                "TemporalWarpBase", "TemporalMaxMotion", "TemporalResidualBlend",
                                "TemporalShowZone", "MotionScale", "MotionInvert", "TemporalCenterX",
                                "DebugTiming", "CrashGuard"})
        Check(ini.Has(foreign), "a save leaves the keys it does not own untouched");
    Check(ini.Value("MotionInvert") == "1" && ini.Value("DebugTiming") == "1",
          "the values of foreign keys are not rewritten");
    Check(!MemoryIni(legacy).Has("OptiScalerTakeover") && ini.Has("OptiScalerTakeover"),
          "the takeover key is added by the first save");
}

// (d) The ini's temporal mode, sanitised (mode 2 was withdrawn in 26.26).
void TestTemporalMode()
{
    Check(pw_addon::TemporalModeFromIni(2) == 1, "TemporalMode=2 falls back to Interpolate (sync)");
    Check(pw_addon::TemporalModeFromIni(3) == 3, "TemporalMode=3 passes through");
    Check(pw_addon::TemporalModeFromIni(0) == 0 && pw_addon::TemporalModeFromIni(1) == 1,
          "TemporalMode 0 and 1 pass through");
    Check(pw_addon::TemporalModeFromIni(-1) == 0, "a negative TemporalMode clamps to every frame");
    Check(pw_addon::TemporalModeFromIni(7) == 1, "a too large TemporalMode clamps to Interpolate (sync)");

    MemoryIni ini("TemporalMode=2\nTemporalEvery=2\n");
    pw_addon::AddonPersisted loaded;
    pw_addon::LoadFromStore(ini, loaded);
    Check(loaded.temporal.mode == 1, "an ini with the withdrawn mode 2 loads as mode 1");
}

// (e) The clamps applied on load.
void TestClamps()
{
    {
        MemoryIni ini("TemporalEvery=99\nTemporalMaxQueue=99\n");
        pw_addon::AddonPersisted loaded;
        Check(!pw_addon::LoadFromStore(ini, loaded), "temporal keys alone are not a saved layout");
        Check(loaded.temporal.every == 8 && loaded.temporal.maxQueue == 8,
              "TemporalEvery / TemporalMaxQueue clamp to 8");
    }
    {
        MemoryIni ini("TemporalEvery=0\nTemporalMaxQueue=-4\n");
        pw_addon::AddonPersisted loaded;
        pw_addon::LoadFromStore(ini, loaded);
        Check(loaded.temporal.every == 1 && loaded.temporal.maxQueue == 0,
              "TemporalEvery clamps to 1 and TemporalMaxQueue to 0");
    }
    {
        MemoryIni ini("Brightness=999\nGamma=99\n");
        pw_addon::AddonPersisted loaded;
        pw_addon::LoadFromStore(ini, loaded);
        Check(Near(loaded.brightnessPercent, 20.0f) && Near(loaded.gamma, 1.4f),
              "Brightness clamps to +-20 percent and Gamma to 0.7..1.4");
    }
    {
        MemoryIni ini("Brightness=-999\nGamma=0\n");
        pw_addon::AddonPersisted loaded;
        pw_addon::LoadFromStore(ini, loaded);
        Check(Near(loaded.brightnessPercent, -20.0f), "Brightness clamps at -20 percent");
        Check(Near(loaded.gamma, 1.0f), "Gamma=0 is rejected and the default stays");
    }
    {
        // The layout itself is not clamped on load: an out-of-range Mode reaches ValidateConfig,
        // which rejects the whole section, and the add-on keeps its defaults (LoadPersistedConfigOnce).
        MemoryIni ini("Mode=5\nCenterX=80\n");
        pw_addon::AddonPersisted loaded;
        Check(pw_addon::LoadFromStore(ini, loaded), "an out-of-range Mode still counts as a layout key");
        Check(static_cast<int>(loaded.config.mode) == 5, "Mode is read verbatim, not clamped");
        Check(pw::ValidateConfig(loaded.config) != pw::Status::Ok,
              "an out-of-range Mode makes the saved layout invalid, so the defaults stay active");
    }
}

// (f) An ini without a [PeripheralWarp] section leaves every default in place.
void TestDefaults()
{
    MemoryIni ini("SomeoneElsesKey=7\n");
    pw_addon::AddonPersisted loaded;
    Check(!pw_addon::LoadFromStore(ini, loaded), "an ini without our keys reports no saved layout");
    const pw_addon::AddonPersisted defaults;
    Check(SameConfig(loaded.config, defaults.config) && SameConfig(loaded.config, pw::DefaultConfigV2()),
          "the layout stays at pw::DefaultConfigV2()");
    Check(SameSettings(loaded, defaults), "outlines, colour, temporal and takeover stay at their defaults");
    Check(defaults.optiTakeover && !defaults.showCenterOutline && !defaults.showWorkOutline &&
              !defaults.workShiftEnabled && Near(defaults.brightnessPercent, 0.0f) && Near(defaults.gamma, 1.0f),
          "the defaults mirror AddonState");
}

} // namespace

int main()
{
    TestSavedKeySet();
    TestRoundTrip();
    TestLegacySection();
    TestTemporalMode();
    TestClamps();
    TestDefaults();
    if (failures == 0) std::cout << "addon ini schema tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
