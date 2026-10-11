// DebugTemporalMarkerRule (id 49, int 0..4) and DebugTemporalMarkerNeighbours (id 50, int 0..25): read-only diagnostic
// ints of the Diagnostics group, 0 by default, read from the ini. TemporalModelMarker (id 51, 2026.10.2): the persisted
// user choice of the Temporal group, its ini round trip and its mapping onto the rule (core/temporal/model_marker.h).
// Part of test_settings_schema_tests (test_settings_schema.cpp is at its size limit).
#include "hosts/reshade/addon/ini_schema.h"
#include "core/temporal/model_marker.h"

#include <map>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {
struct OneKey {
    const char *key;
    int value;
    bool GetInt(const char *name, int &out) const
    {
        if (std::strcmp(name, key) != 0) return false;
        out = value;
        return true;
    }
    bool GetFloat(const char *, float &) const { return false; }
};
struct MapIni {
    std::map<std::string, int> ints;
    std::map<std::string, float> floats;
    bool GetInt(const char *k, int &out) const { auto i = ints.find(k); if (i == ints.end()) return false; out = i->second; return true; }
    bool GetFloat(const char *k, float &out) const { auto i = floats.find(k); if (i == floats.end()) return false; out = i->second; return true; }
    void SetInt(const char *k, int v) { ints[k] = v; }
    void SetFloat(const char *k, float v) { floats[k] = v; }
};
using ofps::core::temporal::ModelMarker;
using ofps::core::temporal::ResolveModelMarker;
constexpr bool Is(ModelMarker m, bool sync, bool background, int rule, int neighbours) {
    return m.sync == sync && m.background == background && m.rule == rule && m.neighbours == neighbours;
}
// 0 = no markers, 1 = rule 0, 2 = rule 4 at 8 of 25; the Debug* keys override (diagnostics keep working).
static_assert(Is(ResolveModelMarker(0, 0, 0, false, false), false, false, 0, 0));
static_assert(Is(ResolveModelMarker(1, 0, 0, false, false), true, true, 0, 0));
static_assert(Is(ResolveModelMarker(2, 0, 0, false, false), true, true, 4, 8));
static_assert(Is(ResolveModelMarker(1, 4, 0, false, false), true, true, 4, 0));   // phase 2's t1r4 under TemporalModelMarker=1
static_assert(Is(ResolveModelMarker(2, 3, 0, false, false), true, true, 3, 0));   // a debug rule drops the narrow threshold
static_assert(Is(ResolveModelMarker(2, 0, 6, false, false), true, true, 4, 6));
static_assert(Is(ResolveModelMarker(1, 4, 8, false, false), true, true, 4, 8));
static_assert(Is(ResolveModelMarker(0, 2, 0, false, false), true, true, 2, 0));   // a debug rule turns markers on
static_assert(Is(ResolveModelMarker(2, 0, 0, true, false), false, true, 4, 8));   // DebugTemporalNoModelMotion
static_assert(Is(ResolveModelMarker(2, 0, 0, false, true), true, false, 4, 8));   // DebugTemporalNoBackgroundModelMotion
} // namespace

int TestModelMarkerKey()
{
    const OfpsSettingDesc &d = kOfpsSettings[OFPS_SET_TEMPORAL_MODEL_MARKER];
    int labels = 0;
    while (d.enumLabels != nullptr && d.enumLabels[labels] != nullptr) ++labels;
    auto values = ofps::reshade::SchemaDefaults();
    bool ok = std::strcmp(d.iniKey, "TemporalModelMarker") == 0 && d.type == OFPS_TYPE_ENUM && d.group == OFPS_GROUP_TEMPORAL &&
              d.minValue == 0.0f && d.maxValue == 2.0f && d.defaultValue.i == 2 && d.flags == OFPS_FLAG_PERSISTED &&
              d.visibleIf.settingId == OFPS_SET_TEMPORAL_MODE && d.visibleIf.op == OFPS_VIS_NE && labels == 3 &&
              values.v[OFPS_SET_TEMPORAL_MODEL_MARKER].i == 2 &&
              ofps::reshade::LoadValuesFromStore(OneKey{"TemporalModelMarker", 1}, values) &&
              values.v[OFPS_SET_TEMPORAL_MODEL_MARKER].i == 1;
    if (!ok) std::cerr << "FAIL: TemporalModelMarker is a persisted enum 0..2 of the Temporal group, 2 by default, from the ini\n";
    // The add-on's own persisted set: absent = 2, a save writes it, a load reads it back.
    MapIni ini;
    ofps::reshade::AddonPersisted persisted, loaded;
    const bool absent = persisted.temporalModelMarker == 2 && (ofps::reshade::LoadFromStore(ini, loaded), loaded.temporalModelMarker == 2);
    for (int v : {0, 1, 2}) {
        persisted.temporalModelMarker = v;
        ofps::reshade::SaveToStore(ini, persisted);
        ofps::reshade::AddonPersisted back;
        ofps::reshade::LoadFromStore(ini, back);
        ok = ok && ini.ints.count("TemporalModelMarker") == 1 && ini.ints["TemporalModelMarker"] == v && back.temporalModelMarker == v;
    }
    ini.ints["TemporalModelMarker"] = 7;
    ofps::reshade::LoadFromStore(ini, loaded);
    ok = ok && absent && loaded.temporalModelMarker == 2;
    if (!ok) std::cerr << "FAIL: TemporalModelMarker round-trips through the add-on's ini (absent = 2, clamped)\n";
    return ok ? 0 : 1;
}


int TestMarkerRuleKey()
{
    const OfpsSettingDesc &d = kOfpsSettings[OFPS_SET_DEBUG_TEMPORAL_MARKER_RULE];
    auto values = ofps::reshade::SchemaDefaults();
    const bool ok = std::strcmp(d.iniKey, "DebugTemporalMarkerRule") == 0 && d.type == OFPS_TYPE_INT &&
                    d.group == OFPS_GROUP_DIAGNOSTICS && d.minValue == 0.0f && d.maxValue == 4.0f && d.defaultValue.i == 0 &&
                    (d.flags & OFPS_FLAG_DIAGNOSTIC) != 0 && (d.flags & OFPS_FLAG_PERSISTED) == 0 &&
                    values.v[OFPS_SET_DEBUG_TEMPORAL_MARKER_RULE].i == 0 &&
                    ofps::reshade::LoadValuesFromStore(OneKey{"DebugTemporalMarkerRule", 3}, values) &&
                    values.v[OFPS_SET_DEBUG_TEMPORAL_MARKER_RULE].i == 3;
    if (!ok) std::cerr << "FAIL: DebugTemporalMarkerRule is a read-only diagnostic int 0..4, 0 by default, read from the ini\n";
    const OfpsSettingDesc &n = kOfpsSettings[OFPS_SET_DEBUG_TEMPORAL_MARKER_NEIGHBOURS];
    const bool okN = std::strcmp(n.iniKey, "DebugTemporalMarkerNeighbours") == 0 && n.type == OFPS_TYPE_INT &&
                     n.group == OFPS_GROUP_DIAGNOSTICS && n.minValue == 0.0f && n.maxValue == 25.0f && n.defaultValue.i == 0 &&
                     (n.flags & OFPS_FLAG_DIAGNOSTIC) != 0 && (n.flags & OFPS_FLAG_PERSISTED) == 0 &&
                     values.v[OFPS_SET_DEBUG_TEMPORAL_MARKER_NEIGHBOURS].i == 0 &&
                     ofps::reshade::LoadValuesFromStore(OneKey{"DebugTemporalMarkerNeighbours", 8}, values) &&
                     values.v[OFPS_SET_DEBUG_TEMPORAL_MARKER_NEIGHBOURS].i == 8;
    if (!okN) std::cerr << "FAIL: DebugTemporalMarkerNeighbours is a read-only diagnostic int 0..25, 0 by default, from the ini\n";
    return ok && okN ? 0 : 1;
}
