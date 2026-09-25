#pragma once
#include "test_core_api_fakes.h"
namespace coretest {
inline int failures = 0;
inline void Check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
inline void SetInt(OfpsSettingsValues &v, std::uint32_t id, std::int32_t value) {
    v.v[id].i = value;
    v.explicitMask[id / 64] |= 1ull << (id % 64);
}
inline void SetFloat(OfpsSettingsValues &v, std::uint32_t id, float value) {
    v.v[id].f = value;
    v.explicitMask[id / 64] |= 1ull << (id % 64);
}
inline OfpsSettingsValues CurrentSettings(IOfpsCore *core) {
    OfpsSettingsValues v{};
    v.size = sizeof(v);
    core->GetSettings(&v);
    return v;
}
inline bool SameRect(const OfpsRect &r, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}
inline bool Inside(const OfpsRectF &a, const OfpsRectF &b) {
    constexpr float eps = 1.0e-4f;
    return a.x >= b.x - eps && a.y >= b.y - eps && a.x + a.w <= b.x + b.w + eps && a.y + a.h <= b.y + b.h + eps;
}
inline OfpsStatus CurrentStatus(IOfpsCore *core) {
    OfpsStatus s{};
    s.size = sizeof(s);
    core->Status(&s);
    return s;
}
inline int CreateFeatureOnList(WarpDevice &w, IOfpsCore *core, IOfpsModelHost *model, IOfpsFeature **feature,
                               std::uint32_t width = kW, std::uint32_t height = kH) {
    if (!coretest::BeginList(w))
        return OFPS_E_DEVICE;
    OfpsFeatureDesc desc{};
    desc.size = sizeof(desc);
    desc.width = width;
    desc.height = height;
    desc.resourceDevice = w.device.Get();
    desc.adapterLuid = w.device->GetAdapterLuid();
    const int r = core->CreateFeature(w.list.Get(), &desc, model, feature);
    if (!coretest::SubmitList(w))
        return OFPS_E_DEVICE;
    core->OnCommandListExecuted(w.queue.Get(), w.list.Get());
    if (!coretest::WaitForQueue(w.device.Get(), w.queue.Get()))
        return OFPS_E_DEVICE;
    return r;
}
inline void DrainReleasedModels(WarpDevice &w, IOfpsCore *core) {
    Check(coretest::BeginList(w), "begin retirement submission");
    Check(coretest::SubmitList(w), "submit retirement list");
    core->OnCommandListExecuted(w.queue.Get(), w.list.Get());
    core->Housekeeping();
    core->Housekeeping();
    Check(coretest::WaitForQueue(w.device.Get(), w.queue.Get()), "retirement fence completes");
    core->Housekeeping();
}
} // namespace coretest
