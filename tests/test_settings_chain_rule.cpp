// TemporalChainRule (id 56, phase 3 step 1 of the camera-motion carry): the persisted user choice of the Temporal group,
// its ini round trip and its mapping onto the two chains' rules (core/temporal/chain_rule.h). Part of
// test_settings_schema_tests (test_settings_schema.cpp is at its size limit).
#include "hosts/reshade/addon/ini_schema.h"
#include "core/temporal/chain_rule.h"

#include <map>

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
using ofps::core::temporal::ChainRules;
using ofps::core::temporal::ResolveChainRules;
constexpr bool Is(ChainRules r, int main, int carry) { return r.main == main && r.carry == carry; }
// 0 = classic (one chain, rule 0), 1 = coherent (the model's chain rule 0, the carry chain rule 5).
static_assert(Is(ResolveChainRules(0, 0, 0), 0, 0));
static_assert(Is(ResolveChainRules(1, 0, 0), 0, 5));
// A Debug* key above 0 replaces the mapping: both rules come from the keys, an unset carry key = no carry chain.
static_assert(Is(ResolveChainRules(1, 3, 0), 3, 0));   // step 1's C3 under the default
static_assert(Is(ResolveChainRules(0, 3, 0), 3, 0));
static_assert(Is(ResolveChainRules(0, 0, 5), 0, 5));   // step 1c's S5 under classic
static_assert(Is(ResolveChainRules(1, 0, 6), 0, 6));
static_assert(Is(ResolveChainRules(1, 0, 2), 0, 2));
static_assert(Is(ResolveChainRules(0, 2, 4), 2, 4));
} // namespace

int TestChainRuleKey()
{
    const OfpsSettingDesc &d = kOfpsSettings[OFPS_SET_TEMPORAL_CHAIN_RULE];
    int labels = 0;
    while (d.enumLabels != nullptr && d.enumLabels[labels] != nullptr) ++labels;
    auto values = ofps::reshade::SchemaDefaults();
    bool ok = d.id == OFPS_SET_TEMPORAL_CHAIN_RULE && std::strcmp(d.iniKey, "TemporalChainRule") == 0 &&
              d.type == OFPS_TYPE_ENUM && d.group == OFPS_GROUP_TEMPORAL && d.minValue == 0.0f && d.maxValue == 1.0f &&
              d.defaultValue.i == 1 && d.flags == OFPS_FLAG_PERSISTED && d.rangeFrom == 0u &&
              d.visibleIf.settingId == OFPS_SET_TEMPORAL_MODE && d.visibleIf.op == OFPS_VIS_NE && labels == 2 &&
              values.v[OFPS_SET_TEMPORAL_CHAIN_RULE].i == 1 &&
              ofps::reshade::LoadValuesFromStore(OneKey{"TemporalChainRule", 0}, values) &&
              values.v[OFPS_SET_TEMPORAL_CHAIN_RULE].i == 0;
    if (!ok) std::cerr << "FAIL: TemporalChainRule is a persisted enum 0..1 of the Temporal group, 1 by default, from the ini\n";
    // The add-on's own persisted set: absent = 1, a save writes it, a load reads it back, out of range clamps.
    MapIni ini;
    ofps::reshade::AddonPersisted persisted, loaded;
    const bool absent = persisted.temporalChainRule == 1 && (ofps::reshade::LoadFromStore(ini, loaded), loaded.temporalChainRule == 1);
    for (int v : {0, 1}) {
        persisted.temporalChainRule = v;
        ofps::reshade::SaveToStore(ini, persisted);
        ofps::reshade::AddonPersisted back;
        ofps::reshade::LoadFromStore(ini, back);
        ok = ok && ini.ints.count("TemporalChainRule") == 1 && ini.ints["TemporalChainRule"] == v && back.temporalChainRule == v;
    }
    ini.ints["TemporalChainRule"] = 7;
    ofps::reshade::LoadFromStore(ini, loaded);
    ok = ok && absent && loaded.temporalChainRule == 1;
    if (!ok) std::cerr << "FAIL: TemporalChainRule round-trips through the add-on's ini (absent = 1, clamped)\n";
    return ok ? 0 : 1;
}
