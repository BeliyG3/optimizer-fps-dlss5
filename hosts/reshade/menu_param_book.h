#pragma once
// Menu mode's own NGX parameter block (pure CPU: no D3D calls; tests/test_menu_param_book.cpp).
//  - A generation is one (host handle, real model handle) pair of the watched feature. The NR runtime's reads during
//    the first kTracedEvaluates model evaluates of a generation are traced; their union is the generation's key set.
//    (Only successful evaluates count; the generation's key list is frozen once, so Build never touches a live trace.)
//  - After each successful host evaluate an immutable snapshot of those keys is built from the host's block: a key the
//    host does not answer keeps the host's result code, every pointer is cleared, the UI / UIAlpha / Backbuffer /
//    ControlMask keys are copied (with their sub-rects) when the host answers them with null and refuse the snapshot
//    when they hold a texture. menu_params.cpp publishes it together with the guide set of the same evaluate.
//  - The present thread only reads the newest published snapshot of the bound generation; a release clears it.
#include "hosts/reshade/ngx_param_shim.h"
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ofps::reshade {

struct MenuTag {
    void *hostHandle = nullptr; // the handle the host holds
    void *realHandle = nullptr; // the NGX feature the core evaluates behind it
    std::uint64_t generation = 0;
};

struct MenuShape {
    D3D12_RESOURCE_DESC colour{}, output{};
    UINT colourRect[4]{}, outputRect[4]{}; // x, y, w, h
    UINT modelWidth = 0, modelHeight = 0;  // the extent the core created the model at
};

struct MenuParamSnapshot {
    MenuTag tag;
    MenuShape shape;
    OwnParams block;   // never written once published
    int guideSet = -1; // menu_guides.h: the depth copied at the same host evaluate
    std::uint64_t settingsEpoch = 0; // MenuSettingsEpoch() before that host evaluate (menu_settings.h)
};

enum class MenuTake { Taken, NotTraced, AuxInUse };

// One model evaluate as the book sees it: the block for the NR runtime and the evaluate's identity (unique per book,
// never reused; 0 = not traced, nothing to end).
struct MenuEvaluate {
    NVSDK_NGX_Parameter *block = nullptr;
    std::uint64_t id = 0;
};

// Threads: Bind / BeginEvaluate / EndEvaluate / Build / Publish / Withdraw / AfterHostEvaluate run on the host's evaluate thread, Newest and
// Tag on the present thread, Clear on whichever thread the release hook runs. Every method takes the mutex only for
// pointer-sized work: no host getter and no snapshot allocation runs under it.
class MenuParamBook {
public:
    static constexpr unsigned kTracedEvaluates = 3; // successful evaluates traced per generation
    MenuParamBook();
    // At a model evaluate of the watched feature: a pair other than the bound one starts a new generation (a fresh
    // trace, the newest snapshot dropped). Returns the bound tag.
    MenuTag Bind(void *hostHandle, void *realHandle);
    // Releases so far (every Clear counts, bound feature or not). A host evaluate takes it when it begins and binds with
    // BindIfNoRelease: false (nothing bound) when any feature was released since, so an evaluate racing a release never
    // re-binds the released feature (a bind that lost the race is retried by the next evaluate).
    std::uint64_t Releases() const;
    // *bound (when given and bound): the tag this evaluate's snapshot must carry and the only one it may withdraw.
    bool BindIfNoRelease(void *hostHandle, void *realHandle, std::uint64_t releases, MenuTag *bound = nullptr);
    // After every host evaluate of a feature: when it is the bound one and the core's current model differs from the
    // bound model (re-created) or the model call failed, the newest snapshot is dropped at once. Binding the new model
    // is left to a successful evaluate (BindIfNoRelease).
    void AfterHostEvaluate(void *hostHandle, void *currentModel, bool modelFailed);
    // Around the NR runtime's evaluate: the token whose `block` is what to hand it (a trace of `host` until the
    // generation has traced kTracedEvaluates successful evaluates, else `host` itself with id 0). The evaluate keeps its
    // trace alive until EndEvaluate, whatever Bind / Clear do meanwhile. EndEvaluate(token, ok) counts the evaluate when
    // it succeeded (a failed one is not counted, its reads stay in the union). A token the book does not have in flight
    // (id 0, already ended, foreign) changes nothing, whatever address its block has; an evaluate of an older generation
    // only ever touches its own discarded trace. While the key list of a finished trace is being frozen (outside the
    // lock) no other trace starts: BeginEvaluate answers `host`.
    MenuEvaluate BeginEvaluate(NVSDK_NGX_Parameter *host);
    void EndEvaluate(const MenuEvaluate &evaluate, bool succeeded);
    bool Traced() const; // the bound generation's key set is complete
    // After a successful host evaluate: a snapshot of the generation's keys, not yet published. Taken: *out is set;
    // AuxInUse: the caller withdraws the newest snapshot (Withdraw) since the host now feeds a texture the snapshot lacks.
    MenuTake Build(NVSDK_NGX_Parameter *host, const MenuShape &shape, std::unique_ptr<MenuParamSnapshot> *out) const;
    // True: it is now the newest. False: dropped, its generation (or feature handle) is no longer bound.
    bool Publish(std::unique_ptr<MenuParamSnapshot> snapshot);
    void Withdraw(const MenuTag &tag); // drops the newest snapshot of that generation; the generation and its trace stay
    std::shared_ptr<const MenuParamSnapshot> Newest() const;   // null: none of the bound generation
    MenuTag Tag() const;
    void Clear(void *hostHandle); // that feature was released: nothing of it survives

private:
    struct Trace; // one generation's trace, shared with the evaluates that use it
    void Rebind(void *hostHandle, void *realHandle); // under mutex_: a new pair starts a generation
    mutable std::mutex mutex_;
    MenuTag tag_;
    std::shared_ptr<Trace> trace_;
    std::vector<std::shared_ptr<Trace>> inflight_; // traced evaluates between Begin and End
    std::uint64_t nextEvaluateId_ = 1;
    std::uint64_t releases_ = 0;
    std::shared_ptr<const MenuParamSnapshot> newest_;
};

// The menu evaluate's block: the snapshot's keys, the pass's own four inputs, Reset 0.
void MenuPassBlock(const MenuParamSnapshot &snapshot, ID3D12Resource *colour, ID3D12Resource *output, ID3D12Resource *depth,
                   ID3D12Resource *motion, OwnParams &out);

// Can a snapshot's pass run on a back buffer of width x height? Null: yes; else the reason for the tab.
const char *MenuShapeProblem(const MenuShape &shape, UINT width, UINT height);
// Stage 3: the core runs the menu frame when the model is compressed or the user's sync temporal mode is on; the direct
// pass otherwise (Mode Off with every-frame or background mode: menus never use the background mode).
bool MenuUsesCore(const MenuShape &shape, UINT width, UINT height, int temporalMode);
// Null: a menu pass can run with this temporal mode; else the reason (the background mode with a compressed model).
const char *MenuTemporalProblem(const MenuShape &shape, UINT width, UINT height, int temporalMode);

} // namespace ofps::reshade
