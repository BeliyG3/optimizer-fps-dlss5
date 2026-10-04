#pragma once
// Menu mode (the MenuMode checkbox): while the game keeps presenting without calling the NR evaluate of the watched
// feature (a pause menu, a map), the add-on runs the NR model on the presented frame at ReShade's present event, ordered
// on the GPU only:
//   present queue:  capture list  back buffer -> capture;                Signal(f1)
//   private queue (the host list's device, NGX's): Wait(f1); pass;       Signal(f2)
//   present queue:  Wait(f2); write-back list: output -> back buffer;    Signal(f3)
// A pass that refuses enqueues no wait: the frame stays untouched. A failed Signal or Wait stops menu mode for the
// session; objects the GPU may still use are then never released. D3D11 swap chains go through menu_bridge_d3d11.h.
#include "hosts/reshade/menu_colour.h"
#include "hosts/reshade/menu_param_book.h"
#include "hosts/reshade/menu_state.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>

namespace reshade::api {
struct command_queue;
struct swapchain;
} // namespace reshade::api

namespace ofps::reshade {
class MenuExitReset;

// Recorded on the private list between capture and write-back. On entry and on return the capture is in COPY_SOURCE and
// the output in COPY_DEST; both have the back buffer's size and format.
struct MenuFrame {
    ID3D12Resource *capture, *output;
    ID3D12GraphicsCommandList *list;
    UINT width, height;
    DXGI_FORMAT format;
    // Set false by a pass whose list must run but that left `output` as it was (MenuCoreFrame::LastShownAgain): the
    // pipeline then writes back only an output of an earlier pass of the same run, else the frame stays untouched.
    bool wrote = true;
};
using MenuPass = bool (*)(MenuFrame &frame);

// DebugMenuPass (shell_settings.h): the model is the product; marker / forced refusal / forced f2 failure are checks.
MenuPassKind MenuPassSetting();
bool MenuPassRunsModel(); // Model or ForcedSignalFailure

// Host thread, one host evaluate (ngx_hook_api.cpp HookEvaluate), constructed before the core's evaluate (and before
// featureCallMutex) and destroyed after it. For the watched feature (the first one whose evaluate succeeds with menu mode
// on, until it is released) it ends a menu run and marks this thread's evaluate for the parameter trace.
// Exit reset (ruling 2026-09-29, a latch in menu_state.h): the first host evaluate of the watched feature after ANY run
// that submitted a pass - with menu mode on or off, however the run ended - resets NR history: Params() is then a wrapper
// that answers DLSSNR.Reset=1 (MenuExitReset, menu_params.h), which the shell hands to ReadFrameInputs and
// ModelHostNgx::BeginFrame in place of the host's block. Before that model call, outside the pipeline's lock, the host's
// NR queue is ordered after the last menu pass (MenuHostQueue::OrderExitNow: a GPU Wait, or a CPU wait of at most
// 100 ms), and (fix round 2) the model is called only once any menu pass that may use it is proven complete on the CPU
// (at most 100 ms): a re-creation inside this evaluate never frees a model a pass still uses. Without that proof
// Withheld() is true: the shell returns kMenuWithheldResult for this evaluate without calling the model. A pass still
// running on a live device (a slow GPU) keeps menu mode on (final review I2); a removed device turns it off for the
// session and keeps the pipeline's objects (C2). The following evaluates of that feature are withheld without waiting
// while such a pass (or one whose f2 Signal failed after it was submitted) may still run (menu_outstanding.h; for the
// session when nothing can prove it finished), and while the feature's release is under way.
// An exit reset this evaluate took is owed again unless Evaluated() reports a successful model call (the destructor).
// Evaluated(), after the core's evaluate, whatever it did:
//  - a current model other than the bound one, or a failed model call, drops the served snapshot at once;
//  - a successful model call binds the book to (feature, currentModel) and takes the snapshot of this evaluate, unless
//    a feature was released since this evaluate began (the release wins; the next evaluate binds again).
inline constexpr int kMenuWithheldResult = static_cast<int>(NVSDK_NGX_Result_Fail);
class MenuHostEvaluate {
public:
    MenuHostEvaluate(void *hostHandle, void *params, ID3D12GraphicsCommandList *hostList);
    ~MenuHostEvaluate();
    bool Withheld() const { return withheld_; } // do not call the model; return kMenuWithheldResult
    void *Params() const; // the block the core's evaluate reads: the host's, or the exit-reset wrapper around it
    void Evaluated(void *currentModel, bool modelCalled, bool succeeded);
    MenuHostEvaluate(const MenuHostEvaluate &) = delete;
    MenuHostEvaluate &operator=(const MenuHostEvaluate &) = delete;

private:
    void *hostHandle_;
    void *params_;
    ID3D12GraphicsCommandList *list_;
    bool candidate_ = false;   // menu mode on and not another feature than the watched one
    bool resetTaken_ = false;  // this evaluate took the owed exit reset
    bool resetRead_ = false;   // ... and its model call succeeded (else the destructor owes it again)
    bool withheld_ = false;
    bool leased_ = false;      // holds a pipeline evaluate lease (no menu pass submitted) until destroyed
    // The watched feature's recordings went unreported for kMenuSubmitHookPresents: the destructor (after the core's
    // evaluate, before the host submits this list, no lock held) installs the submit hook (addon/queue_events.h).
    bool hookWanted_ = false;
    std::uint64_t releases_ = 0; // MenuParamBook::Releases() when this evaluate began
    std::uint64_t settingsEpoch_ = 0; // MenuSettingsEpoch() before the core's evaluate (the snapshot carries it)
    std::unique_ptr<MenuExitReset> reset_;
};
// HookRelease, before featureCallMutex: every feature, watched or not. For the watched feature it sets a release
// tombstone (its evaluates are withheld until MenuFeatureReleaseDone) and waits (off the present path, without the
// pipeline's lock, at most 500 ms) for a menu pass that may still run. False: that pass is not proven complete (a timeout,
// or a pass that was never fenced): HookRelease must not release the feature (leaked for the session).
bool MenuFeatureReleased(void *hostHandle);
void MenuFeatureReleaseDone(void *hostHandle); // the end of HookRelease, on every path


// From the six-argument addon_event::present adapter / destroy_swapchain (not a resize; outside DllMain). Menu mode
// serves one swap chain at a time (the first it builds its GPU objects for); the drain runs only when that swap chain is
// destroyed, waits at most 500 ms in all (every fence, both backends) without the pipeline's lock, and releases only
// what passed on a live device.
void MenuPipelinePresent(::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain);
void MenuPipelineDrain(::reshade::api::swapchain *swapchain);

// The tab (overlay_schema.cpp): menu mode's one-line status (off / waiting / active / unavailable: reason).
struct MenuStatusView {
    MenuStatus status;
    std::string line;
};
MenuStatusView MenuStatusNow();
// ReShade's execute_command_list (before the native call) and destroy_command_queue (queue_events.cpp); native pointers.
void MenuListExecuting(ID3D12CommandQueue *queue, ID3D12CommandList *list);
void MenuQueueDestroyed(ID3D12CommandQueue *queue);
// ReShade's init_command_queue for a D3D12 queue: counted, so the D3D11 bridge can tell that ReShade reported its private
// queue (a queue of ReShade's wrapped device, menu_pipeline_d3d11_gpu.cpp).
void MenuQueueInitialized();

// Model pass (menu_model_pass.cpp): capture -> modelIn (the host's colour format), the watched feature's model evaluated
// on the private list with the own block (menu_param_book.h), modelOut -> output.
bool MenuModelBuild(ID3D12Device *device, const D3D12_RESOURCE_DESC &backBuffer, const MenuShape &shape); // size-bound
void MenuModelBind(std::shared_ptr<const MenuParamSnapshot> snapshot); // the snapshot the next pass evaluates with
bool MenuModelPass(MenuFrame &frame);
double MenuModelTakeEvaluateMs(); // CPU of the last pass's evaluate call; then 0
std::vector<Microsoft::WRL::ComPtr<IUnknown>> MenuModelRetire(); // for the graveyard
void MenuModelReleaseDevice(); // after a completed drain: the root signature and PSO of that device
std::string MenuShaderCode(const wchar_t *file); // optimizer-fps-dlss5\<file> next to the add-on (empty when missing)

// GPU time of the private list (menu_gpu_time.cpp): timestamps per private ring slot, read once f2 passed.
bool MenuGpuTimeBuild(ID3D12Device *device, ID3D12CommandQueue *queue, unsigned slots);
void MenuGpuTimeBegin(ID3D12GraphicsCommandList *list, unsigned slot);
void MenuGpuTimeEnd(ID3D12GraphicsCommandList *list, unsigned slot);
void MenuGpuTimeSubmitted(unsigned slot, UINT64 value2);
void MenuGpuTimePoll(UINT64 completed2, unsigned long long presentIndex);
void MenuGpuTimeTotals(double *avgMs, double *maxMs, unsigned *samples);
void MenuGpuTimeRelease(); // only after a completed drain
bool MenuGpuTimeSlow();     // the last three passes each took >= 100 ms
void MenuGpuTimeRunStart(); // a run starts: the count of slow passes restarts

} // namespace ofps::reshade
