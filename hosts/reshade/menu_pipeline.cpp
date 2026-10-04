#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_core_pass.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_fence.h"
#include "hosts/reshade/menu_settings.h"
#include <mutex>
#include <utility>

// Menu mode's pipeline: the one pipeline and its lock, the present entry, the swap chain drain, the queue events and the
// tab's status. The host's side (evaluates, release, the proof that passes finished): menu_pipeline_host.cpp; one
// present's decisions: menu_pipeline_step.cpp; GPU objects: menu_pipeline_gpu.cpp; one D3D12 menu present:
// menu_submit.cpp; D3D11 swap chains: menu_pipeline_d3d11.cpp.
namespace ofps::reshade {
using namespace menu_detail;
namespace {
constexpr unsigned kCpuEvery = 60;
constexpr double kMenuDrainMs = 500; // destroy_swapchain: the whole drain, every fence of both backends

// ms: the whole present (BMP writes excluded); the pipeline's own share excludes the NGX evaluate recording.
void RecordCpu(Pipeline &p, unsigned long long index, double ms) {
    const double eval = p.phase[kEvaluate], pipe = ms - eval;
    p.cpuSum += ms; p.cpuTotal += ms; ++p.cpuCount; p.pipeSum += pipe; p.pipeTotal += pipe;
    p.cpuMax = ms > p.cpuMax ? ms : p.cpuMax; p.cpuTotalMax = ms > p.cpuTotalMax ? ms : p.cpuTotalMax;
    p.pipeMax = pipe > p.pipeMax ? pipe : p.pipeMax; p.pipeTotalMax = pipe > p.pipeTotalMax ? pipe : p.pipeTotalMax;
    // Per present: total, evaluate recording, the pipeline's own share, the run's present (percentiles from the log).
    MenuEvent("t %llu %.4f %.4f %.4f %u", index, ms, eval, pipe, p.runPresents);
    if (pipe >= 1.0) { // which part of the pipeline's own work was slow
        const double *f = p.phase;
        MenuEvent("slowparts %llu %u poll %.4f step %.4f log %.4f capture %.4f record %.4f submit %.4f tail %.4f", index, p.runPresents,
                  f[kPoll], f[kStep], f[kLog], f[kCapture], f[kRecord], f[kSubmit], f[kTail]);
    }
    if (p.menuPresents % kCpuEvery != 0) return;
    MenuEvent("cpu %llu %.4f %.4f %.4f %.4f", index, p.cpuSum / p.cpuCount, p.cpuMax, p.pipeSum / p.cpuCount, p.pipeMax);
    Log("menu pipeline: cpu per menu present avg %.4f ms, max %.4f ms; without the evaluate recording avg %.4f ms, max %.4f ms "
        "(last %u, BMP writes excluded, %.1f ms so far)", p.cpuSum / p.cpuCount, p.cpuMax, p.pipeSum / p.cpuCount, p.pipeMax, p.cpuCount, p.ioTotal);
    p.cpuSum = p.cpuMax = p.pipeSum = p.pipeMax = 0; p.cpuCount = 0;
}

void LogStatusChange(Pipeline &p) { // an "unavailable" reason reaches ReShade.log once per change
    if (p.state.Status() != MenuStatus::Unavailable) { p.loggedStatus.clear(); return; }
    std::string line = p.state.StatusLine();
    if (line == p.loggedStatus) return;
    p.loggedStatus = std::move(line);
    Log("%s", p.loggedStatus.c_str());
}
} // namespace

Pipeline &menu_detail::P() { static Pipeline *pipeline = new Pipeline(); return *pipeline; } // never destroyed from DllMain

std::mutex &menu_detail::PipelineMutex() {
    static std::mutex *mutex = new std::mutex(); // never destroyed from DllMain
    return *mutex;
}

MenuHostQueue &menu_detail::HostQueue() {
    static MenuHostQueue *queue = new MenuHostQueue(); // never destroyed from DllMain
    return *queue;
}

void MenuPipelinePresent(::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain) {
    if (!queue || !swapchain) return;
    const double t0 = NowMs();
    std::lock_guard lock(PipelineMutex());
    auto &p = P();
    const bool on = MenuModeOn();
    FollowCheckbox(p, on);
    if (!on && !g_built.load(std::memory_order_relaxed)) { ++p.presents; return; } // nothing to retire; the index still counts
    p.state.SetEntryMs(double(CurrentShellSettings().debugMenuEntryMs));
    const unsigned long long index = p.presents++;
    HostQueue().NotePresent();
    for (double &part : p.phase) part = 0;
    double io = 0;
    if (p.gpu) { // a removed device's fences read 0 here (menu_fence.h); the graveyard then keeps everything
        io = MenuDumpPoll(MenuFenceProgress(p.gpu->f3.Get()));
        MenuGpuTimePoll(MenuFenceProgress(p.gpu->f2.Get()), index);
        ReleaseGraveyard(*p.gpu);
    }
    p.outstanding.ForgetCompletedLast(); // review M4: a pass seen complete is never waited for again
    if (p.bridge) io += PollD3D11(p, index);
    if (!p.lateBridges.empty()) PollLateD3D11(p);
    if (g_listQuarantine.exchange(false)) {
        // The queue event already waited 100 ms: later evaluates withhold without waiting again (unless an outstanding
        // pass, which already covers the later one, is held).
        if (p.outstanding.LastFence() && !p.outstanding.Pending()) {
            MenuPassProof slow;
            slow.fence = p.outstanding.LastFence();
            slow.value = p.outstanding.LastValue();
            p.outstanding.Settle(slow, MenuPassWait::TimedOut);
            p.loggedWithhold = false;
        }
        Quarantine(p, index, "the game's NR list ran on a queue that could not wait for the last menu pass, which did not complete within 100 ms");
    }
    if (p.state.Active() && MenuGpuTimeSlow()) {
        p.state.SuspendRun("the model needs more than 100 ms per menu frame on this GPU");
        EndRun(p, index, "left");
        Log("menu mode: three menu passes of 100 ms or more in a row; this menu stays without NR");
    }
    p.ioTotal += io;
    p.phase[kPoll] = NowMs() - t0 - io;
    const bool wasMenu = p.state.Active();
    Step(p, queue, swapchain, index);
    g_built.store(p.gpu || p.bridge || !p.lateBridges.empty(), std::memory_order_relaxed);
    LogStatusChange(p);
    if (!Dumping() || !(wasMenu || p.state.Active())) return;
    const double ms = NowMs() - t0 - io;
    // Step's own share (guards, entry, logging) is what the timed parts do not cover.
    p.phase[kStep] = ms - p.phase[kPoll] - p.phase[kLog] - p.phase[kCapture] - p.phase[kRecord] - p.phase[kEvaluate] - p.phase[kSubmit] - p.phase[kTail];
    RecordCpu(p, index, ms);
}

// Final review (M1, M2, Codex minor): only the destruction of the swap chain menu mode serves drains, with one deadline
// for every fence of both backends. The objects are detached under the lock, waited for with it released (host
// evaluates, releases and the tab never wait behind the drain; the swap chain stays bound meanwhile, so no present of
// another swap chain builds new objects), then released or kept under the lock again.
void MenuPipelineDrain(::reshade::api::swapchain *swapchain) {
    Gpu *gpu = nullptr;
    MenuBridgeD3D11 *bridge = nullptr;
    {
        std::lock_guard lock(PipelineMutex());
        auto &p = P();
        if (!p.lateBridges.empty()) PollLateD3D11(p);
        if (!swapchain || swapchain != p.bound) {
            g_built.store(p.gpu || p.bridge || !p.lateBridges.empty(), std::memory_order_relaxed);
            return; // another swap chain (a launcher, a video window): nothing of menu mode's is on it
        }
        if (p.state.Active()) { p.state.Interrupt(); EndRun(p, p.presents, "drain"); }
        gpu = DetachGpu(p);
        bridge = DetachD3D11(p);
    }
    const double deadline = NowMs() + kMenuDrainMs;
    const bool bridgeDrained = bridge && WaitD3D11(*bridge, deadline);
    const bool gpuDrained = gpu && WaitGpu(*gpu, deadline);
    std::lock_guard lock(PipelineMutex());
    auto &p = P();
    if (bridge) FinishD3D11(p, bridge, bridgeDrained);
    if (gpu) FinishGpu(p, gpu, gpuDrained);
    p.bound = nullptr; p.loggedUnbound = false; // the next swap chain that builds menu mode's objects is served
    g_built.store(p.gpu || p.bridge || !p.lateBridges.empty(), std::memory_order_relaxed);
}

MenuStatusView MenuStatusNow() {
    std::lock_guard lock(PipelineMutex());
    auto &p = P();
    FollowCheckbox(p, MenuModeOn()); // the tab follows the checkbox at once, before the next present
    MenuStatusView view{p.state.Status(), p.state.StatusLine()};
    // C6: without optical flow the sync cadence is not used in menus; Mode Off runs the model on every menu frame (a
    // compressed model shows the blocker instead).
    if (MenuTemporalMode() == 1 && (view.status == MenuStatus::Active || view.status == MenuStatus::Waiting))
        if (const char *flow = MenuFlowProblem()) view.line += std::string("; temporal cadence off: ") + flow + ", the model runs on every menu frame";
    return view;
}

void MenuListExecuting(ID3D12CommandQueue *queue, ID3D12CommandList *list) {
    if (!MenuModeOn() && !g_built.load(std::memory_order_relaxed)) return;
    // Only a Quarantine needs action: the exit list could not be ordered and cannot be withheld here (log, off).
    if (HostQueue().ListExecuting(queue, list) == MenuExitOrder::Quarantine) g_listQuarantine.store(true);
}

void MenuQueueDestroyed(ID3D12CommandQueue *queue) { HostQueue().QueueDestroyed(queue); }

void MenuQueueInitialized() { g_reshadeQueueInits.fetch_add(1); }

} // namespace ofps::reshade
