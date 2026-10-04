#include "hosts/reshade/menu_params.h"
#include "hosts/reshade/menu_guides.h"
#include "hosts/reshade/ngx_forwarder_calls.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <cstdio>

namespace ofps::reshade {
namespace {
thread_local void *t_evaluatingHost = nullptr;
thread_local OwnParams t_ownAtHost;      // DebugMenuOwnBlock: the block of this thread's host model evaluate
std::uint64_t g_loggedGeneration = 0;     // host thread: the last generation whose trace was logged
bool g_loggedOwnAtHost = false;

void Log(const char *text) { Host().Log(OFPS_LOG_INFO, text); }

// DebugMenuOwnBlock=1: this evaluate's keys as menu mode would hand them over, with the host's own four inputs and Reset.
NVSDK_NGX_Parameter *OwnBlockAtHost(NVSDK_NGX_Parameter *host) {
    std::unique_ptr<MenuParamSnapshot> snapshot;
    if (MenuBook().Build(host, MenuShape{}, &snapshot) != MenuTake::Taken) return nullptr;
    MenuPassBlock(*snapshot, GetResource(host, "DLSSNR.Color"), GetResource(host, "DLSSNR.Output"),
                  GetResource(host, "DLSSNR.Depth"), GetResource(host, "DLSSNR.MVec"), t_ownAtHost);
    const auto &values = snapshot->block.Values();
    const auto &absent = snapshot->block.Absent();
    if (const auto v = values.find("DLSSNR.Reset"); v != values.end()) t_ownAtHost.Put("DLSSNR.Reset", v->second);
    else if (const auto a = absent.find("DLSSNR.Reset"); a != absent.end()) t_ownAtHost.PutAbsent("DLSSNR.Reset", a->second);
    if (!g_loggedOwnAtHost) {
        g_loggedOwnAtHost = true;
        char text[200];
        std::snprintf(text, sizeof(text), "menu params: DebugMenuOwnBlock=1: host model evaluates run with menu mode's own block (%zu keys, %zu absent)",
                      t_ownAtHost.Values().size(), t_ownAtHost.Absent().size());
        Log(text);
    }
    return &t_ownAtHost;
}
} // namespace

MenuParamBook &MenuBook() {
    static MenuParamBook *book = new MenuParamBook(); // never destroyed from DllMain
    return *book;
}

void MenuSetEvaluatingHost(void *hostHandle) { t_evaluatingHost = hostHandle; }

int MenuTracedEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback) {
    void *hostHandle = t_evaluatingHost;
    if (!hostHandle || !handle || !params) return CallEvaluate(cmd, handle, params, callback); // menu mode off, or not watched
    // params: the host's block, or on the exit evaluate the MenuExitReset wrapper around it (the trace then records what
    // the runtime reads; Build reads the host's block itself).
    auto *host = static_cast<NVSDK_NGX_Parameter *>(params);
    MenuParamBook &book = MenuBook();
    const MenuTag tag = book.Tag(); // bound at the watched feature's successful evaluates (MenuHostEvaluate::Evaluated)
    if (tag.hostHandle != hostHandle || tag.realHandle != handle) return CallEvaluate(cmd, handle, params, callback);
    if (CurrentShellSettings().debugMenuOwnBlock != 0 && book.Traced())
        if (NVSDK_NGX_Parameter *own = OwnBlockAtHost(host)) return CallEvaluate(cmd, handle, own, callback);
    const MenuEvaluate evaluate = book.BeginEvaluate(host);
    int result = 0;
    try {
        result = CallEvaluate(cmd, handle, evaluate.block, callback);
    } catch (...) {
        book.EndEvaluate(evaluate, false);
        throw;
    }
    book.EndEvaluate(evaluate, result == kNgxSuccess);
    if (book.Traced() && g_loggedGeneration != tag.generation) {
        g_loggedGeneration = tag.generation;
        char text[160];
        std::snprintf(text, sizeof(text), "menu params: generation %llu of feature %p (model %p) traced", static_cast<unsigned long long>(tag.generation),
                      hostHandle, handle);
        Log(text);
    }
    return result;
}

bool MenuHostShapeOf(void *params, MenuShape *out) {
    if (!params || !out) return false;
    ID3D12Resource *colour = GetResource(params, "DLSSNR.Color"), *output = GetResource(params, "DLSSNR.Output");
    if (!colour || !output) return false;
    *out = MenuShape{};
    out->colour = colour->GetDesc();
    out->output = output->GetDesc();
    const Subrect c = ReadSubrect(params, "Color", UINT(out->colour.Width), out->colour.Height);
    const Subrect o = ReadSubrect(params, "Output", UINT(out->output.Width), out->output.Height);
    const UINT cr[4] = {c.x, c.y, c.w, c.h}, orr[4] = {o.x, o.y, o.w, o.h};
    for (int i = 0; i < 4; ++i) { out->colourRect[i] = cr[i]; out->outputRect[i] = orr[i]; }
    if (IOfpsCore *core = Core()) { // Mode Uniform/Peripheral shrink the model below the frame
        OfpsStatus status{};
        status.size = sizeof(status);
        core->Status(&status);
        out->modelWidth = status.workW;
        out->modelHeight = status.workH;
    }
    return true;
}

const char *MenuSnapshotHostEvaluate(ID3D12GraphicsCommandList *hostList, void *params, const MenuTag &tag,
                                     std::uint64_t settingsEpoch) {
    MenuParamBook &book = MenuBook();
    // No stale pair of this evaluate's generation stays valid; another generation (a newer feature or model) is not ours.
    const auto refuse = [&book, &tag](const char *problem) { book.Withdraw(tag); return problem; };
    MenuShape shape;
    if (!MenuHostShapeOf(params, &shape)) return refuse("the game's NR block has no colour or output");
    std::unique_ptr<MenuParamSnapshot> snapshot;
    switch (book.Build(static_cast<NVSDK_NGX_Parameter *>(params), shape, &snapshot)) {
    case MenuTake::Taken: break;
    case MenuTake::AuxInUse: return refuse("the game gives NR a UI or back-buffer texture (not supported in menus)");
    case MenuTake::NotTraced: return nullptr;
    }
    // Built for the book's current binding: from this evaluate's block only when that is still this evaluate's tag.
    if (snapshot->tag.generation != tag.generation || snapshot->tag.hostHandle != tag.hostHandle) return nullptr;
    const char *problem = nullptr;
    const int set = MenuGuidesCopy(hostList, params, &problem);
    if (set == kMenuGuidesBusy) return nullptr;
    if (set < 0) return refuse(problem);
    snapshot->guideSet = set;
    snapshot->settingsEpoch = settingsEpoch;
    book.Publish(std::move(snapshot)); // false: the feature was released or re-bound meanwhile (its guides go with it)
    return nullptr;
}

MenuExitReset::MenuExitReset(void *params) : wrapper_(static_cast<NVSDK_NGX_Parameter *>(params)) {
    unsigned value = 0;
    const bool had = params && GetUInt(params, "DLSSNR.Reset", &value);
    char text[200], kept[40] = "no Reset key";
    if (had) std::snprintf(kept, sizeof(kept), "Reset=%u", value);
    std::snprintf(text, sizeof(text), "menu mode: the first host evaluate after menu passes reads DLSSNR.Reset=1 (the game's block keeps %s)", kept);
    Log(text);
}

} // namespace ofps::reshade
