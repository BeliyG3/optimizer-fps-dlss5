// The NR add-on's control exports (--switch, --temporal) and the chain's log verdict at the end.
#pragma once
#include "bench_d3d.h"

struct BenchOptions;
struct LayoutStateV1;

using PFN_SetLayout = unsigned int(__cdecl *)(const LayoutStateV1 *);
using PFN_SetTemporal = unsigned int(__cdecl *)(unsigned int, unsigned int);

struct AddonControl {
    PFN_SetLayout setLayout = nullptr;
    PFN_SetTemporal setTemporal = nullptr;
    int cycleIndex = 0;
    bool switchPending = false;
    int switches = 0;
};

// Looks the exports up in the loaded add-on; false when --switch is given and the add-on is missing.
bool FindAddonExports(const BenchOptions &o, AddonControl &a);
// Per frame, before rendering: the layout switch every --switch frames, the temporal mode at frame 10.
void AddonFrame(const BenchOptions &o, AddonControl &a, int frame);
// Exit code 3 when the bridge/ReShade logs show the bridge stopped or the D3D12 device was removed, else 0.
int VerdictFromLogs();
