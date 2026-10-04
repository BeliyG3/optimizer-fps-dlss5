#include "hosts/reshade/menu_param_book.h"
#include <string>
#include <vector>

namespace ofps::reshade {
namespace {
// A frame-old UI or back-buffer texture would be wrong in a menu: null is copied, a texture refuses the snapshot.
const char *const kAuxKeys[] = {"DLSSNR.UI", "DLSSNR.UIAlpha", "DLSSNR.Backbuffer", "DLSSNR.ControlMask"};
const char *const kSubrectParts[] = {"SubrectBaseX", "SubrectBaseY", "SubrectWidth", "SubrectHeight"};

// The runtime reads an aux input's sub-rect only when the host answers that input (even with null): the snapshot
// carries the input and its four sub-rect keys whenever the host answers it, traced or not.
void CopyAux(NVSDK_NGX_Parameter *host, const char *aux, OwnParams &block) {
    void *value = nullptr;
    if (host->Get(aux, &value) != NVSDK_NGX_Result_Success) return;
    block.Put(aux, static_cast<void *>(nullptr));
    for (const char *part : kSubrectParts) {
        const std::string name = std::string(aux) + part;
        if (block.Knows(name.c_str())) continue;
        NgxValue read;
        const NVSDK_NGX_Result result = ReadAs(host, name.c_str(), NgxType::Int, &read);
        if (result == NVSDK_NGX_Result_Success) block.Put(name, read);
        else block.PutAbsent(name, result);
    }
}

void ClearPointers(OwnParams &block) {
    std::vector<std::string> pointers;
    for (const auto &[name, value] : block.Values())
        if (IsPointerValue(value)) pointers.push_back(name);
    for (const std::string &name : pointers) block.Put(name, static_cast<void *>(nullptr));
}
} // namespace

// One generation's trace. Evaluates hold it (inflight_) until they end, so a release or a new generation only replaces
// the book's pointer; the old evaluate goes on reading through, and counting into, its own discarded object.
struct MenuParamBook::Trace {
    TraceParams trace;
    unsigned successful = 0;
    bool tracing = false;    // an evaluate is between Begin and the end of its finalization
    std::uint64_t id = 0;    // that evaluate's identity
    std::shared_ptr<const std::vector<TracedKey>> frozen; // set once, when the key set is complete
};

MenuParamBook::MenuParamBook() : trace_(std::make_shared<Trace>()) {}

void MenuParamBook::Rebind(void *hostHandle, void *realHandle) {
    if (hostHandle == tag_.hostHandle && realHandle == tag_.realHandle) return;
    tag_.hostHandle = hostHandle;
    tag_.realHandle = realHandle;
    ++tag_.generation;
    trace_ = std::make_shared<Trace>();
    newest_.reset();
}

MenuTag MenuParamBook::Bind(void *hostHandle, void *realHandle) {
    std::lock_guard lock(mutex_);
    Rebind(hostHandle, realHandle);
    return tag_;
}

std::uint64_t MenuParamBook::Releases() const {
    std::lock_guard lock(mutex_);
    return releases_;
}

bool MenuParamBook::BindIfNoRelease(void *hostHandle, void *realHandle, std::uint64_t releases, MenuTag *bound) {
    std::lock_guard lock(mutex_);
    if (releases != releases_) return false;
    Rebind(hostHandle, realHandle);
    if (bound) *bound = tag_;
    return true;
}

void MenuParamBook::AfterHostEvaluate(void *hostHandle, void *currentModel, bool modelFailed) {
    std::lock_guard lock(mutex_);
    if (!hostHandle || hostHandle != tag_.hostHandle) return;
    if (currentModel != tag_.realHandle || modelFailed) newest_.reset();
}

MenuEvaluate MenuParamBook::BeginEvaluate(NVSDK_NGX_Parameter *host) {
    std::lock_guard lock(mutex_);
    if (!host || trace_->frozen || trace_->tracing) return {host, 0};
    trace_->tracing = true;
    trace_->id = nextEvaluateId_++;
    trace_->trace.Bind(host);
    inflight_.push_back(trace_);
    return {&trace_->trace, trace_->id};
}

void MenuParamBook::EndEvaluate(const MenuEvaluate &evaluate, bool succeeded) {
    if (evaluate.id == 0) return;
    std::shared_ptr<Trace> ended;
    bool freeze = false;
    {
        std::lock_guard lock(mutex_);
        for (auto it = inflight_.begin(); it != inflight_.end(); ++it) {
            if ((*it)->id != evaluate.id) continue;
            ended = std::move(*it);
            inflight_.erase(it);
            break;
        }
        if (!ended) return;
        freeze = succeeded && ++ended->successful >= kTracedEvaluates;
        if (!freeze) { ended->trace.Bind(nullptr); ended->tracing = false; }
    }
    if (!freeze) return;
    // The key list is copied outside the lock (`tracing` stays set, so no other trace starts meanwhile).
    auto keys = std::make_shared<const std::vector<TracedKey>>(ended->trace.Keys());
    std::lock_guard lock(mutex_);
    ended->trace.Bind(nullptr);
    ended->frozen = std::move(keys);
    ended->tracing = false;
}

bool MenuParamBook::Traced() const {
    std::lock_guard lock(mutex_);
    return trace_->frozen != nullptr;
}

MenuTake MenuParamBook::Build(NVSDK_NGX_Parameter *host, const MenuShape &shape, std::unique_ptr<MenuParamSnapshot> *out) const {
    std::shared_ptr<const std::vector<TracedKey>> keys;
    MenuTag tag;
    {
        std::lock_guard lock(mutex_);
        if (host && out && trace_->frozen) { keys = trace_->frozen; tag = tag_; }
    }
    if (!keys) return MenuTake::NotTraced;
    for (const char *aux : kAuxKeys) {
        void *value = nullptr;
        if (host->Get(aux, &value) == NVSDK_NGX_Result_Success && value) return MenuTake::AuxInUse;
    }
    auto snapshot = std::make_unique<MenuParamSnapshot>();
    snapshot->tag = tag;
    snapshot->shape = shape;
    CopyKeys(*keys, host, snapshot->block);
    for (const char *aux : kAuxKeys) CopyAux(host, aux, snapshot->block);
    ClearPointers(snapshot->block);
    *out = std::move(snapshot);
    return MenuTake::Taken;
}

bool MenuParamBook::Publish(std::unique_ptr<MenuParamSnapshot> snapshot) {
    std::lock_guard lock(mutex_);
    if (!snapshot || snapshot->tag.generation != tag_.generation || snapshot->tag.hostHandle != tag_.hostHandle) return false;
    newest_ = std::shared_ptr<const MenuParamSnapshot>(std::move(snapshot));
    return true;
}

void MenuParamBook::Withdraw(const MenuTag &tag) {
    std::lock_guard lock(mutex_);
    if (tag.generation == tag_.generation && tag.hostHandle == tag_.hostHandle) newest_.reset();
}

std::shared_ptr<const MenuParamSnapshot> MenuParamBook::Newest() const {
    std::lock_guard lock(mutex_);
    return newest_ && newest_->tag.generation == tag_.generation ? newest_ : nullptr;
}

MenuTag MenuParamBook::Tag() const {
    std::lock_guard lock(mutex_);
    return tag_;
}

void MenuParamBook::Clear(void *hostHandle) {
    std::lock_guard lock(mutex_);
    ++releases_; // an evaluate that began before this release binds nothing (BindIfNoRelease)
    if (!hostHandle || hostHandle != tag_.hostHandle) return;
    tag_.hostHandle = tag_.realHandle = nullptr;
    ++tag_.generation;
    trace_ = std::make_shared<Trace>();
    newest_.reset();
}

void MenuPassBlock(const MenuParamSnapshot &snapshot, ID3D12Resource *colour, ID3D12Resource *output, ID3D12Resource *depth,
                   ID3D12Resource *motion, OwnParams &out) {
    out = snapshot.block;
    out.Set("DLSSNR.Color", colour);
    out.Set("DLSSNR.Output", output);
    out.Set("DLSSNR.Depth", depth);
    out.Set("DLSSNR.MVec", motion);
    out.Set("DLSSNR.Reset", 0u);
}

const char *MenuShapeProblem(const MenuShape &shape, UINT width, UINT height) {
    const auto whole = [&](const UINT (&r)[4]) { return r[0] == 0 && r[1] == 0 && r[2] == width && r[3] == height; };
    if (!whole(shape.colourRect) || !whole(shape.outputRect))
        return "the game's NR input is not the whole frame (a scaled or padded NR input)";
    return nullptr;
}

bool MenuUsesCore(const MenuShape &shape, UINT width, UINT height, int temporalMode) {
    const bool compressed = shape.modelWidth != width || shape.modelHeight != height;
    return temporalMode == 3 ? compressed : compressed || temporalMode == 1;
}

const char *MenuTemporalProblem(const MenuShape &shape, UINT width, UINT height, int temporalMode) {
    const bool compressed = shape.modelWidth != width || shape.modelHeight != height;
    return temporalMode == 3 && compressed ? "the background temporal mode is not used in menus: use Every frame or Interpolate (sync)" : nullptr;
}

} // namespace ofps::reshade
