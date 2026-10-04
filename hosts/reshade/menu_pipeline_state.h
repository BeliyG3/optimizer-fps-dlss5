#pragma once
// Internal to the menu pipeline: its state and the functions shared by menu_pipeline.cpp (the pipeline and its lock,
// present entry, status), menu_pipeline_host.cpp (host evaluates and release), menu_pipeline_step.cpp (one D3D12 present's decisions), menu_pipeline_gpu.cpp (GPU objects),
// menu_submit.cpp (one D3D12 menu present), menu_pipeline_d3d11.cpp (D3D11 swap chains) and menu_pipeline_d3d11_gpu.cpp (the bridge towards the core).
// Every function here runs under the pipeline's lock.
#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_guides.h"
#include "hosts/reshade/menu_host_queue.h"
#include "hosts/reshade/menu_outstanding.h"
#include "hosts/reshade/menu_state.h"
#include <reshade_api.hpp>
#include <d3d11.h>
#include <atomic>
#include <mutex>
#include <string>

namespace ofps::reshade {
class MenuBridgeD3D11;
}

namespace ofps::reshade::menu_detail {
using Microsoft::WRL::ComPtr;
constexpr unsigned kRing = 3, kDumpMenuPresent = 10;
constexpr unsigned kDumpAfterExit = 5;
constexpr unsigned long long kProbePresent = 30; // one-off dump outside a menu: present index == bench frame

struct NativeSlot { // present queue, native device: capture and write-back (or dump-only) lists
    ComPtr<ID3D12CommandAllocator> captureAllocator, tailAllocator;
    ComPtr<ID3D12GraphicsCommandList> captureList, tailList;
    UINT64 value3 = 0; // reusable once f3 passed it
};
struct PrivateSlot { // private queue, proxy device
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 value2 = 0; // reusable once f2 passed it
};
struct Retired { ComPtr<IUnknown> object; UINT64 value2 = 0, value3 = 0; };

// GPU objects; torn down only by the drain (FinishGpu, outside DllMain), never by a static destructor.
struct Gpu {
    ComPtr<ID3D12CommandQueue> presentQueue; // native: capture, write-back, dumps
    ComPtr<ID3D12Device> native;
    ComPtr<ID3D12CommandQueue> queue;        // private DIRECT queue on the proxy device
    ComPtr<ID3D12Fence> f1, f2, f3;
    UINT64 v1 = 0, v2 = 0, v3 = 0;           // advanced only by a Signal that succeeded
    NativeSlot nativeRing[kRing];
    PrivateSlot privateRing[kRing];
    unsigned nativeNext = 0, privateNext = 0;
    bool resources = false, dumps = false; // size-bound resources exist / the dump pool is ready
    // A Signal failed, the device was removed, or nothing proves the last pass finished (quarantine): submitted work may
    // never be fenced, nothing is released again.
    bool poisoned = false;
    bool registered = false; // the private queue is known to the core (C1): unregistered only after its last fence passed
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    MenuColourSpace space = MenuColourSpace::Unknown; // the swap chain's, when the resources were built
    DXGI_FORMAT modelIn = DXGI_FORMAT_UNKNOWN, modelOut = DXGI_FORMAT_UNKNOWN; // the snapshot formats the resources were built for
    ComPtr<ID3D12Resource> capture, output, marker;
    std::vector<Retired> graveyard;
};

// CPU of one present on the present thread, by part (ms): the pipeline's own work and the evaluate recording.
enum Phase : unsigned { kPoll, kStep, kLog, kCapture, kRecord, kEvaluate, kSubmit, kTail, kPhases }; // kLog: the entry's log lines

struct Pipeline {
    MenuStateMachine state;        // entry, exit, blockers, stop, the owed exit reset, the tab's line (menu_state.h)
    // Final review (Codex I3, review M3): menu mode serves one swap chain at a time, the first one it builds its GPU
    // objects for (the private queue on D3D12, the bridge on D3D11; always before that swap chain's first run), until
    // that swap chain is destroyed (MenuPipelineDrain). Presents of any other swap chain are ignored before the state
    // machine (one log line), so a D3D12 and a D3D11 build never replace each other's model heap and textures.
    const void *bound = nullptr;
    bool loggedUnbound = false;
    bool wasEnabled = false;       // the checkbox as last followed (FollowCheckbox)
    bool quarantined = false;      // Quarantine was logged
    ID3D12Device *proxy = nullptr; // the host list's device (NGX's); kept alive by the host
    // The host list's device equals the NR colour's (or the colour's is unknown): the signature of ReShade's wrapped
    // device (bench12; on the D3D11 bench the list answers the native device). D3D11 bridge keying (fix round 1).
    bool listOnWrapped = false;
    void *hostHandle = nullptr;    // the watched feature
    bool remembered = false;
    bool orderEntry = false;       // the run's first pass waits for the host's NR queue (menu_host_queue.h)
    bool hostQueueWaiting = false; // the entry found the host's NR queue unobserved: a retryable blocker until it is seen
    // A pass the game's next NR evaluates cannot be ordered after (a failed post-submit f2 Signal, an exit or queue-event
    // quarantine): the watched feature's evaluates are withheld while it may run (menu_outstanding.h).
    MenuOutstandingPass outstanding;
    bool loggedWithhold = false, loggedLease = false;
    MenuEvaluateLeases leases; // host evaluates between their proof and the core's evaluate: no menu pass meanwhile
    const char *snapshotProblem = nullptr; // why the last host evaluate left no snapshot (host thread)
    std::string buildProblem;      // resources that could not be built for buildKey
    unsigned long long buildKey = 0;
    std::string loggedStatus;      // the last "unavailable" status line written to the log
    bool menuDumpPending = false;  // the 10th menu present's dump stays requested until its copy is submitted
    double phase[kPhases] = {};
    double pipeSum = 0, pipeMax = 0, pipeTotal = 0, pipeTotalMax = 0; // CPU without the evaluate recording
    unsigned long long presents = 0, menuPresents = 0, failures = 0, skips = 0;
    unsigned runPresents = 0, afterExit = 0, runs = 0;
    bool runPassed = false;        // this run submitted a pass whose output the present queue waited for (stage 3 reuse)
    unsigned long long reused = 0; // stage 3: presents that showed the last pass again (the previous one still ran)
    bool loggedReuse = false;
    bool noteGameEvaluate = false; // DebugMenuDump=1: the next host evaluate records the core's motion source ("game" event)
    double enterMs = 0;
    double cpuSum = 0, cpuMax = 0, cpuTotal = 0, cpuTotalMax = 0, ioTotal = 0; unsigned cpuCount = 0;
    bool loggedRing = false, loggedFail = false, loggedQueue = false, loggedOther = false, loggedFirstRun = false, loggedHostQueue = false;
    Gpu *gpu = nullptr;
    MenuBridgeD3D11 *bridge = nullptr;          // D3D11 swap chains (menu_pipeline_d3d11.cpp), torn down like gpu
    std::vector<MenuBridgeD3D11 *> lateBridges; // drained past their 500 ms: released once their fence passed
    ID3D11DeviceContext *context11 = nullptr;   // the bridge's present context (dump polls); the device keeps it alive
    MenuColourSpace space11 = MenuColourSpace::Unknown;
    DXGI_FORMAT modelIn11 = DXGI_FORMAT_UNKNOWN, modelOut11 = DXGI_FORMAT_UNKNOWN;
    bool dumps11 = false;
    bool loggedOther11 = false; // a swap chain of a second D3D11 device was ignored
};

// menu_pipeline.cpp: the one pipeline, its lock, and the flags read without the lock. Lock order: PipelineMutex() ->
// featureCallMutex (try-lock only, WithFeatureForMenu) -> MenuHostQueue's / the parameter book's leaf locks -> the core;
// no CPU wait for the GPU runs under it (the destroy_swapchain drain waits with it released, MenuPipelineDrain).
Pipeline &P();
std::mutex &PipelineMutex();
inline std::atomic<bool> g_built{false};          // GPU objects exist: presents are processed while menu mode is off
inline std::atomic<bool> g_watching{false};       // a feature is watched: its evaluates are checked with menu mode off too
inline std::atomic<bool> g_listQuarantine{false}; // the queue event could not order an exit list: quarantine at the next present
inline MenuReleaseTombstone g_releasing;          // the watched feature's release is under way (menu_outstanding.h)
inline std::atomic<unsigned> g_reshadeQueueInits{0}; // D3D12 init_command_queue events seen (MenuQueueInitialized)

double NowMs();
void Log(const char *fmt, ...);
bool Dumping(); // DebugMenuDump=1: events log, BMP dumps, per-run log lines, CPU and GPU reports
MenuHostQueue &HostQueue(); // shared with the queue events and the host thread, which run without the pipeline's lock
void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
bool Begin(ID3D12CommandAllocator *allocator, ID3D12GraphicsCommandList *list);
// Advances the value only when the GPU was told to reach it: nothing may wait for a value never signalled.
bool Signal(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 &value);
bool SignalNative(Gpu &g, NativeSlot &slot); // retires the slot's lists (and dump copies) at a new f3 value
NativeSlot *PeekNative(Gpu &g);              // the next slot when the GPU is done with it, else null (skip, never wait)
PrivateSlot *PeekPrivate(Gpu &g);

// Correction C5: every way a run ends goes through EndRun (host evaluate "exit", checkbox off / blocker / snapshot gone /
// slow passes "left", "resize", "release", "drain" at destroy_swapchain, "stop" for a stop, device removal or bridge
// failure). It records a requested in-menu dump that was not copied as missed, switches the core's motion source back to
// the game's vectors if the run's sync cadence switched it to optical flow (Task 13), and writes the run's end event.
// False (fix round 1): the core refused the switch-back; it stays owed, and the watched feature's host evaluates retry
// it before the core runs them and are withheld while it fails (MenuFlowBeforeHostEvaluate).
bool EndRun(Pipeline &p, unsigned long long index, const char *why);
void OnEnter(Pipeline &p, unsigned long long index); // a run started on this present
// Final review C1: the checkbox as the present or the tab sees it. When it goes off, the newest snapshot is withdrawn,
// so turning it on again waits for a fresh host evaluate (a menu already open is picked up the next time it opens).
void FollowCheckbox(Pipeline &p, bool on);
// Final review (Codex I3): false for a swap chain other than the one menu mode is bound to (one log line).
bool BoundTo(Pipeline &p, const void *swapchain);
// Menu mode off for the session with `reason` in the tab; an active run ends through EndRun. False: already stopped.
bool EndSession(Pipeline &p, unsigned long long index, const char *reason);
// A failed Signal/Wait (or GPU objects that could not be made): the frame stays untouched, menu mode off for the session.
void Stop(Pipeline &p, unsigned long long index, const char *what,
          const char *reason = "stopped after a GPU fence error (see ReShade.log)");
// Correction C2: nothing can prove the last menu pass finished before the game's NR work: the pipeline's objects are
// never released (the GPU may still run that pass), menu mode off for the session, one log line. Since the final review
// (Claude I2) only for a removed device, a pass that was never fenced, or an exit list that ran unordered after a failed
// Wait; a pass that is merely slow is held as outstanding instead (menu_outstanding.h).
void Quarantine(Pipeline &p, unsigned long long index, const char *what);
MenuColourSpace SpaceOf(::reshade::api::color_space space);
unsigned long long BuildKey(UINT width, UINT height, DXGI_FORMAT format, MenuColourSpace space, const MenuParamSnapshot *snapshot);
// Why no pass may run on this present (null: nothing blocks); *retryable: the blocker may go away by itself.
const char *Blocker(Pipeline &p, const MenuParamSnapshot *snapshot, UINT width, UINT height, DXGI_FORMAT format, MenuColourSpace space,
                    bool *retryable);
bool LeaseSkips(Pipeline &p, unsigned long long index); // a host evaluate holds a lease: no menu pass on this present
// C2: a run's first pass waits on `privateQueue` for the host's last NR work (presentQueue: the D3D12 present queue, null
// on a D3D11 swap chain). False: no pass on this present (the blocker, or a stop).
bool OrderRunEntry(Pipeline &p, ID3D12CommandQueue *presentQueue, ID3D12CommandQueue *privateQueue, unsigned long long index);
// menu_pipeline_step.cpp: one D3D12 (or D3D11) present under the state machine.
void Step(Pipeline &p, ::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain, unsigned long long index);
// menu_submit.cpp
// capture -> pass -> write-back; corePass: the pass calls the core's evaluate (stage 3), which waits for the last pass
void MenuPresent(Pipeline &p, Gpu &g, ID3D12Resource *back, unsigned long long index, bool corePass);
bool HostPresent(Pipeline &p, Gpu &g, ID3D12Resource *back, unsigned long long index, bool probe); // a dump outside a menu
// menu_pipeline_gpu.cpp
bool Build(Pipeline &p, ID3D12CommandQueue *presentQueue);
bool BuildResources(Pipeline &p, Gpu &g, const D3D12_RESOURCE_DESC &back, MenuColourSpace space, const MenuShape &shape);
void RetireResources(Gpu &g);
void ReleaseGraveyard(Gpu &g); // a removed device (menu_fence.h) poisons: nothing is released again
// destroy_swapchain of the bound swap chain (MenuPipelineDrain), in three steps: detached under the lock, waited for
// WITHOUT the lock until one deadline shared by every fence and backend (500 ms in all), finished under the lock again
// (released only when every fence passed on a live device; else kept, and menu mode off for the session).
Gpu *DetachGpu(Pipeline &p);
bool WaitGpu(Gpu &g, double deadlineMs); // no lock; touches only g
void FinishGpu(Pipeline &p, Gpu *g, bool drained);
// D3D11 swap chains (menu_pipeline_d3d11.cpp).
void StepD3D11(Pipeline &p, ::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain, unsigned long long index,
               MenuColourSpace space);
double PollD3D11(Pipeline &p, unsigned long long index); // BMP writes (ms returned), GPU times, the graveyard
void ForgetD3D11(Pipeline &p);
MenuBridgeD3D11 *DetachD3D11(Pipeline &p); // under the lock, on the destroying thread (it flushes the context's signals)
// menu_pipeline_d3d11_gpu.cpp: the bridge's lifetime towards the core (C1).
void RegisterBridgeD3D11(Pipeline &p, MenuBridgeD3D11 &b); // the private queue, before its first submission
bool WaitD3D11(MenuBridgeD3D11 &b, double deadlineMs);     // no lock; touches only b
void FinishD3D11(Pipeline &p, MenuBridgeD3D11 *b, bool drained); // released, kept late (its fence polled) or leaked
void PollLateD3D11(Pipeline &p); // releases late-drained bridges whose fence passed (checked, never waited on)
void QuarantineD3D11(Pipeline &p); // Quarantine: the bridge's objects are never released (its queue stays registered)

} // namespace ofps::reshade::menu_detail
