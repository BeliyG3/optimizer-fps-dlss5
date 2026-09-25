#include "remote_host_snapshot.h"

#include "addon_context.h"
#include "remote_host.h"
#include "../shell_host.h"
#include "../direct_host.h"
#include "../ngx_hook_api.h"

#include <cmath>

namespace ofps::reshade {

void FillRemoteSnapshot(ofps::remote::WireSnapshot &out) {
    using namespace ofps::remote;
    IOfpsCore *core = Core();
    const bool direct = DirectHostActive();
    out.applied.size = sizeof(out.applied);
    if (core != nullptr) {
        core->GetSettings(&out.applied);
        OfpsStatus status{};
        status.size = sizeof(status);
        core->Status(&status);
        out.status = CopyStatus(status);
        out.preview.size = sizeof(out.preview);
        core->LayoutPreview(&out.preview);
        OfpsStatusRow rows[kMaxRows]{};
        out.rowCount = core->StatusLines(rows, kMaxRows);
        if (out.rowCount > kMaxRows) out.rowCount = kMaxRows;
        for (std::uint32_t i = 0; i < out.rowCount; ++i)
            out.rows[i] = CopyRow(rows[i]);
    } else {
        out.applied = State().values;
        CopyText(out.status.reason, sizeof(out.status.reason), "core unavailable");
    }
    if (direct) {
        out.status.directHost = 1;
        if (State().directValuesReady) out.applied = State().directValues;
    }
    if (!ValidSettings(out.applied)) {
        out.applied = State().values;
    }
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
        const OfpsSettingDesc &desc = kOfpsSettings[id];
        const bool supported = HostCapSupported(out.hostCaps, desc.hostCap);
        out.available[id] = supported && core != nullptr;
        if (!out.available[id])
            CopyText(out.unavailableReason[id], kReasonLength,
                     core == nullptr ? "core unavailable" : "unsupported host capability");
        if (core != nullptr && desc.rangeFrom != 0) {
            float lo = 0, hi = 0;
            core->GetSettingRange(id, &lo, &hi);
            out.ranges[id] = {lo, hi,
                static_cast<std::uint32_t>(std::isfinite(lo) && std::isfinite(hi) && lo <= hi)};
        }
    }
    OfaRefresh();
    const auto hook = GetHookStatus();
    if (SafeMode()) out.shell.hookState = 7;
    else if (out.status.active) out.shell.hookState = 5;
    else if (!hook.moduleFound) out.shell.hookState = 1;
    else if (!hook.hooked) out.shell.hookState = 2;
    else if (!out.status.featureCreated) out.shell.hookState = 3;
    else if (out.applied.v[OFPS_SET_MODE].i == 0) out.shell.hookState = 4;
    else out.shell.hookState = 6;
    if (!out.status.active && out.status.reason[0] == 0) {
        const char *reason = out.shell.hookState == 1 ? "nvngx_dlssnr.dll is not loaded in the host process" :
            out.shell.hookState == 2 ? "hook not installed yet" :
            out.shell.hookState == 3 ? "waiting for the host to create feature 18" :
            out.shell.hookState == 4 ? "mode is Off" : "pass-through";
        CopyText(out.status.reason, sizeof(out.status.reason), reason);
    }
    out.shell.crashGuard = SafeMode() ? 1u : 0u;
    out.shell.retryAvailable = 1;
    out.shell.feederAvailable = OfaLoaded() ? 1u : 0u;
    if (OfaLoaded()) {
        out.shell.feederSource = OfaSettings().source == ofps::feeder::SourceShader ? 2u : 1u;
        out.shell.feederActive = out.shell.feederSource == 1u ? 1u : 0u;
        out.shell.feederGrid = static_cast<std::uint32_t>(OfaSettings().grid);
        out.shell.feederPerf = static_cast<std::uint32_t>(OfaSettings().perf);
    }
    out.shell.shellVersion = kVersion;
    out.hostHeartbeatTick = GetTickCount64();
}

} // namespace ofps::reshade
