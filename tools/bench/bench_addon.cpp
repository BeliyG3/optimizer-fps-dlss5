#include "bench_addon.h"

#include "bench_options.h"

#include <cstdio>
#include <fstream>
#include <string>

struct LayoutStateV1 {
    unsigned int structSize, mode, filter;
    float centerX, workX, centerY, workY, globalScalePercent;
    unsigned int generation;
};

static bool LogContains(const char *path, const char *const *needles, int count, std::string *hit)
{
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        for (int i = 0; i < count; ++i) {
            if (line.find(needles[i]) != std::string::npos) { *hit = std::string(path) + ": " + line; return true; }
        }
    }
    return false;
}

// Scans the chain's own logs: the bridge does not remove the D3D11 device when its D3D12 side dies.
int VerdictFromLogs()
{
    static const char *const bad[] = {"stopped:", "DEVICE REMOVED", "device was removed", "during stage",
                                      "did not retire", "OBJECT_DELETED_WHILE_STILL_IN_USE"};
    std::string hit;
    if (LogContains("dlss5-dx11-bridge.log", bad, 6, &hit) || LogContains("ReShade.log", bad, 6, &hit)) {
        std::printf("[fail] %s\n", hit.c_str());
        return 3;
    }
    return 0;
}

bool FindAddonExports(const BenchOptions &o, AddonControl &a)
{
    if (o.switchEvery > 0) {
        HMODULE addon = GetModuleHandleW(L"optimizer-fps-dlss5.addon64");
        a.setLayout = addon ? (PFN_SetLayout) GetProcAddress(addon, "PeripheralWarpSetLayoutV1") : nullptr;
        std::printf("[info] layout switching every %d frames: export %s\n", o.switchEvery, a.setLayout ? "found" : "MISSING");
        if (!a.setLayout) return false;
    }
    if (o.temporalMode >= 0) {
        HMODULE addon = GetModuleHandleW(L"optimizer-fps-dlss5.addon64");
        a.setTemporal = addon ? (PFN_SetTemporal) GetProcAddress(addon, "PeripheralWarpSetTemporalV1") : nullptr;
        std::printf("[info] temporal mode %d every %d: export %s\n", o.temporalMode, o.temporalEvery, a.setTemporal ? "found" : "MISSING");
        if (!a.setTemporal) std::printf("[warn] add-on not loaded: the temporal mode is not applied (renodx-only run)\n");
    }
    return true;
}

static const unsigned int kCycle[] = {0, 1, 2}; // Off -> Uniform -> Peripheral

void AddonFrame(const BenchOptions &o, AddonControl &a, int frame)
{
    if (a.setLayout && frame > 0 && frame % o.switchEvery == 0) a.switchPending = true;
    if (a.switchPending) {
        LayoutStateV1 st{};
        st.structSize = sizeof(st); st.mode = kCycle[a.cycleIndex % 3]; st.filter = 1;
        st.centerX = st.centerY = 80.0f; st.workX = st.workY = 90.0f; st.globalScalePercent = 100.0f;
        const unsigned int status = a.setLayout(&st);
        if (status != 3) {
            std::printf("[info] frame %d: layout -> mode %u (status %u)\n", frame, st.mode, status);
            a.switchPending = false; ++a.cycleIndex; ++a.switches;
        }
    }
    if (a.setTemporal && frame == 10) {
        const unsigned int status = a.setTemporal((unsigned) o.temporalMode, (unsigned) o.temporalEvery);
        std::printf("[info] frame %d: temporal mode -> %d every %d (status %u)\n", frame, o.temporalMode, o.temporalEvery, status);
    }
}
