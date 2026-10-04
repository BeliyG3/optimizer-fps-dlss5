// Menu mode's parameter book (menu_param_book.h): generations, the union key set, exact result codes, aux refusal,
// tags and release, the pass block and the shape check. "The runtime" reads through the block BeginEvaluate hands out.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hosts/reshade/menu_param_book.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace ofps::reshade;
namespace {
int failures = 0;
void Check(bool condition, const char *what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
ID3D12Resource *Fake(std::uintptr_t v) { return reinterpret_cast<ID3D12Resource *>(v); }
void *const kHost = reinterpret_cast<void *>(std::uintptr_t(0x100)), *const kOther = reinterpret_cast<void *>(std::uintptr_t(0x200));
void *const kReal1 = reinterpret_cast<void *>(std::uintptr_t(0x11)), *const kReal2 = reinterpret_cast<void *>(std::uintptr_t(0x12));

// A host block: the four inputs, sub-rects, floats, and Reset absent with the host's own code.
OwnParams HostBlock() {
    OwnParams h;
    h.Set("DLSSNR.Color", Fake(0x1000)); h.Set("DLSSNR.Output", Fake(0x2000));
    h.Set("DLSSNR.Depth", Fake(0x3000)); h.Set("DLSSNR.MVec", Fake(0x4000));
    h.Set("DLSSNR.ColorSubrectWidth", 1920u); h.Set("DLSSNR.ColorSubrectHeight", 1080u);
    h.Set("DLSSNR.MVecScaleX", -0.5f); h.Set("DLSSNR.Intensity", 1.0f);
    h.PutAbsent("DLSSNR.Reset", NVSDK_NGX_Result_FAIL_InvalidParameter);
    return h;
}

// One evaluate of the NR runtime: its reads through `block`.
void Evaluate(NVSDK_NGX_Parameter *block, bool readIntensity) {
    ID3D12Resource *r = nullptr; unsigned u = 0; float f = 0; int i = 0;
    block->Get("DLSSNR.Color", &r); block->Get("DLSSNR.Output", &r); block->Get("DLSSNR.Depth", &r); block->Get("DLSSNR.MVec", &r);
    block->Get("DLSSNR.ColorSubrectWidth", &i); block->Get("DLSSNR.ColorSubrectHeight", &i);
    block->Get("DLSSNR.MVecScaleX", &f); block->Get("DLSSNR.Reset", &u);
    if (readIntensity) block->Get("DLSSNR.Intensity", &f);
}

// The first kTracedEvaluates evaluates of a generation; Intensity is read by the last one only when asked.
void TraceGeneration(MenuParamBook &book, OwnParams &host, void *real, bool intensityOnLast) {
    book.Bind(kHost, real);
    for (unsigned n = 0; n < MenuParamBook::kTracedEvaluates; ++n) {
        const MenuEvaluate evaluate = book.BeginEvaluate(&host);
        Check(evaluate.block != &host, "the first evaluates of a generation read through the trace");
        Evaluate(evaluate.block, intensityOnLast && n + 1 == MenuParamBook::kTracedEvaluates);
        book.EndEvaluate(evaluate, true);
    }
}

MenuShape Shape(UINT w, UINT h) {
    MenuShape s;
    s.colourRect[2] = s.outputRect[2] = w;
    s.colourRect[3] = s.outputRect[3] = h;
    s.modelWidth = w; s.modelHeight = h;
    return s;
}

std::unique_ptr<MenuParamSnapshot> Take(MenuParamBook &book, OwnParams &host) {
    std::unique_ptr<MenuParamSnapshot> snapshot;
    Check(book.Build(&host, Shape(1920, 1080), &snapshot) == MenuTake::Taken && snapshot, "a snapshot after the trace");
    return snapshot;
}

void TestGenerations() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    std::unique_ptr<MenuParamSnapshot> snapshot;
    const MenuTag first = book.Bind(kHost, kReal1);
    Check(!book.Traced() && book.Build(&host, Shape(1920, 1080), &snapshot) == MenuTake::NotTraced, "no snapshot before the trace is complete");
    TraceGeneration(book, host, kReal1, false);
    Check(book.Traced(), "traced after kTracedEvaluates evaluates");
    const MenuEvaluate untraced = book.BeginEvaluate(&host);
    Check(untraced.block == &host && untraced.id == 0, "after the trace the runtime reads the host's block itself");
    book.EndEvaluate(untraced, true); // not a traced evaluate: changes nothing
    book.EndEvaluate(MenuEvaluate{&host, 12345}, true); // a token the book never handed out
    Check(book.Traced(), "an End of an untraced or foreign token changes nothing");
    book.Publish(Take(book, host));
    const auto newest = book.Newest();
    Check(newest && newest->tag.generation == first.generation && newest->tag.hostHandle == kHost && newest->tag.realHandle == kReal1,
          "a snapshot is tagged with its generation and both handles");
    const MenuTag second = book.Bind(kHost, kReal2); // the core re-created the model
    Check(second.generation == first.generation + 1 && !book.Newest() && !book.Traced(),
          "a new real handle starts a generation: the snapshot is dropped, the trace restarts");
    auto stale = std::make_unique<MenuParamSnapshot>();
    stale->tag = first;
    Check(!book.Publish(std::move(stale)), "Publish itself rejects a snapshot of an older generation");
    Check(!book.Newest(), "a snapshot of an older generation is never published");
    Check(book.Bind(kHost, kReal2).generation == second.generation, "the same pair keeps its generation");
}

void TestUnionKeys() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, true);
    auto snapshot = Take(book, host);
    float intensity = 0;
    Check(snapshot && snapshot->block.Get("DLSSNR.Intensity", &intensity) == NVSDK_NGX_Result_Success && intensity == 1.0f,
          "a key read by any traced evaluate of the generation is in the snapshot (union)");
    TraceGeneration(book, host, kReal2, false); // a new generation whose evaluates never read Intensity
    snapshot = Take(book, host);
    Check(snapshot && snapshot->block.Get("DLSSNR.Intensity", &intensity) == NVSDK_NGX_Result_FAIL_UnsupportedParameter,
          "the union key set is rebuilt per generation, not frozen");
}

void TestResultCodes() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    auto snapshot = Take(book, host);
    unsigned reset = 7;
    Check(snapshot && snapshot->block.Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_FAIL_InvalidParameter && reset == 7,
          "an absent key answers the host's own code, the value untouched");
    unsigned never = 0;
    Check(snapshot && snapshot->block.Get("DLSSNR.NeverRead", &never) == NVSDK_NGX_Result_FAIL_UnsupportedParameter,
          "a key nobody read answers unsupported parameter, as the hosts do");
}

void TestAux() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    host.Set("DLSSNR.UI", static_cast<ID3D12Resource *>(nullptr)); // present and null (public OptiScaler)
    host.Set("DLSSNR.UISubrectWidth", 640);
    TraceGeneration(book, host, kReal1, false);
    auto snapshot = Take(book, host);
    void *ui = Fake(1);
    int width = 0, height = 0;
    Check(snapshot && snapshot->block.Get("DLSSNR.UI", &ui) == NVSDK_NGX_Result_Success && ui == nullptr,
          "an aux input the host answers with null stays present and null");
    Check(snapshot && snapshot->block.Get("DLSSNR.UISubrectWidth", &width) == NVSDK_NGX_Result_Success && width == 640,
          "its sub-rect keys are copied although the trace never read them");
    Check(snapshot && snapshot->block.Get("DLSSNR.UISubrectHeight", &height) == NVSDK_NGX_Result_FAIL_UnsupportedParameter,
          "a sub-rect key the host lacks answers the host's code");
    host.Set("DLSSNR.Backbuffer", Fake(0x5000));
    std::unique_ptr<MenuParamSnapshot> refused;
    Check(book.Build(&host, Shape(1920, 1080), &refused) == MenuTake::AuxInUse && !refused,
          "a UI / back-buffer texture refuses the snapshot (a frame-old texture is wrong in a menu)");
}

void TestPassBlockAndPointers() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    auto snapshot = Take(book, host);
    ID3D12Resource *colour = Fake(9);
    Check(snapshot && snapshot->block.Get("DLSSNR.Color", &colour) == NVSDK_NGX_Result_Success && colour == nullptr,
          "no host resource survives in a snapshot");
    OwnParams pass;
    MenuPassBlock(*snapshot, Fake(0xA0), Fake(0xB0), Fake(0xC0), Fake(0xD0), pass);
    ID3D12Resource *c = nullptr, *o = nullptr, *d = nullptr, *m = nullptr;
    unsigned reset = 9;
    pass.Get("DLSSNR.Color", &c); pass.Get("DLSSNR.Output", &o); pass.Get("DLSSNR.Depth", &d); pass.Get("DLSSNR.MVec", &m);
    Check(c == Fake(0xA0) && o == Fake(0xB0) && d == Fake(0xC0) && m == Fake(0xD0), "the pass block holds the pass's four inputs");
    Check(pass.Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_Success && reset == 0, "and Reset 0");
    Check(snapshot->block.Get("DLSSNR.Color", &colour) == NVSDK_NGX_Result_Success && colour == nullptr, "the snapshot itself is never written");
}

void TestClear() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    book.Publish(Take(book, host));
    const std::uint64_t generation = book.Tag().generation;
    book.Clear(kOther);
    Check(book.Newest() != nullptr, "another feature's release changes nothing");
    book.Clear(nullptr);
    Check(book.Newest() != nullptr && book.Tag().generation == generation, "a release without a handle changes nothing");
    book.Clear(kHost);
    Check(!book.Newest() && book.Tag().hostHandle == nullptr && !book.Traced(), "the watched feature's release drops everything");
    Check(book.Bind(kHost, kReal1).generation > generation, "a feature bound again starts a new generation");
}

// A release or a new generation while a traced evaluate is running: the evaluate keeps a live trace, and nothing it
// read or counted reaches the new generation.
void TestInflightEvaluate() {
    for (const bool release : {true, false}) {
        MenuParamBook book;
        OwnParams host = HostBlock();
        book.Bind(kHost, kReal1);
        const MenuEvaluate evaluate = book.BeginEvaluate(&host);
        Check(evaluate.block != &host, "the evaluate reads through a trace");
        ID3D12Resource *r = nullptr;
        evaluate.block->Get("DLSSNR.Color", &r);
        if (release) book.Clear(kHost); else book.Bind(kHost, kReal2);
        Evaluate(evaluate.block, true); // the runtime keeps reading after the release / the new generation began
        book.EndEvaluate(evaluate, true);
        book.EndEvaluate(evaluate, true); // a second End of the same evaluate
        Check(!book.Traced() && !book.Newest(), "the aborted evaluate is not credited to the next generation");
        book.Bind(kHost, kReal2);
        for (unsigned n = 0; n < MenuParamBook::kTracedEvaluates; ++n) {
            Check(!book.Traced(), "the new generation needs all its own evaluates");
            const MenuEvaluate own = book.BeginEvaluate(&host);
            Evaluate(own.block, false); // never reads Intensity
            book.EndEvaluate(own, true);
        }
        auto snapshot = Take(book, host);
        float intensity = 0;
        Check(snapshot && snapshot->block.Get("DLSSNR.Intensity", &intensity) == NVSDK_NGX_Result_FAIL_UnsupportedParameter,
              "the aborted evaluate's reads are not in the new generation's key set");
    }
}

// The evaluate whose block was handed out is the only one that can end it.
void TestNestedBegin() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    book.Bind(kHost, kReal1);
    const MenuEvaluate first = book.BeginEvaluate(&host);
    const MenuEvaluate second = book.BeginEvaluate(&host);
    Check(first.block != &host && second.block == &host && second.id == 0, "a second Begin while one is traced reads the host's block");
    book.EndEvaluate(second, true);
    float f = 0;
    Check(first.block->Get("DLSSNR.MVecScaleX", &f) == NVSDK_NGX_Result_Success && f == -0.5f, "its End does not end the first evaluate's trace");
    book.EndEvaluate(first, true);
}

// A late or duplicate End of a finished evaluate must not touch the trace that is active now, even when the finished
// trace's address was reused: only the token identifies an evaluate.
void TestStaleToken() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    book.Bind(kHost, kReal1);
    const MenuEvaluate a = book.BeginEvaluate(&host);
    book.EndEvaluate(a, true);
    book.Bind(kHost, kReal2); // generation B
    const MenuEvaluate b = book.BeginEvaluate(&host);
    Check(b.id != a.id && b.block != &host, "the next evaluate has its own token");
    book.EndEvaluate(a, true);                      // duplicate End of A while B is active
    book.EndEvaluate(MenuEvaluate{b.block, a.id}, true); // A's identity on B's block (a reused address)
    float f = 0;
    Check(b.block->Get("DLSSNR.MVecScaleX", &f) == NVSDK_NGX_Result_Success && f == -0.5f, "B's trace is still bound to the host");
    const MenuEvaluate again = book.BeginEvaluate(&host);
    Check(again.block == &host && again.id == 0, "B is still the traced evaluate in flight");
    book.EndEvaluate(b, true);
    for (unsigned n = 1; n < MenuParamBook::kTracedEvaluates; ++n) {
        const MenuEvaluate next = book.BeginEvaluate(&host);
        Evaluate(next.block, false);
        book.EndEvaluate(next, true);
    }
    Check(book.Traced(), "B needed exactly its own kTracedEvaluates successful evaluates");
}

// A release racing the trace reads and evaluates of another thread: no crash, no assert (short, joined loops).
void TestConcurrentRelease() {
    MenuParamBook book;
    std::atomic<bool> stop{false};
    std::thread releaser([&] {
        while (!stop.load()) {
            book.Clear(kHost);
            book.Newest();
            book.Tag();
        }
    });
    OwnParams host = HostBlock();
    for (int i = 0; i < 2000; ++i) {
        book.Bind(kHost, (i / 8) % 2 ? kReal1 : kReal2);
        const MenuEvaluate evaluate = book.BeginEvaluate(&host);
        Evaluate(evaluate.block, true);
        book.EndEvaluate(evaluate, i % 3 != 0);
        std::unique_ptr<MenuParamSnapshot> snapshot;
        if (book.Build(&host, Shape(1920, 1080), &snapshot) == MenuTake::Taken) book.Publish(std::move(snapshot));
    }
    stop = true;
    releaser.join();
    book.Clear(kHost);
    Check(!book.Newest(), "after the last release nothing survives");
}

// A failed NGX evaluate is not one of the traced evaluates.
void TestFailedEvaluates() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    book.Bind(kHost, kReal1);
    for (unsigned n = 0; n < 2 * MenuParamBook::kTracedEvaluates; ++n) {
        const MenuEvaluate evaluate = book.BeginEvaluate(&host);
        Check(evaluate.block != &host, "tracing goes on after failed evaluates");
        book.EndEvaluate(evaluate, false);
    }
    Check(!book.Traced(), "failed evaluates do not complete the trace");
    for (unsigned n = 0; n < MenuParamBook::kTracedEvaluates; ++n) {
        const MenuEvaluate evaluate = book.BeginEvaluate(&host);
        Evaluate(evaluate.block, false);
        book.EndEvaluate(evaluate, true);
    }
    Check(book.Traced(), "kTracedEvaluates successful ones do");
    std::unique_ptr<MenuParamSnapshot> snapshot;
    unsigned reset = 0;
    Check(book.Build(&host, Shape(1920, 1080), &snapshot) == MenuTake::Taken && snapshot &&
              snapshot->block.Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_FAIL_InvalidParameter,
          "the snapshot has the keys the successful evaluates read");
}

// A build that raced a release must not come back: build, release, publish (and the same with the feature bound again).
void TestBuildReleasePublish() {
    for (const bool rebind : {false, true}) {
        MenuParamBook book;
        OwnParams host = HostBlock();
        TraceGeneration(book, host, kReal1, false);
        auto snapshot = Take(book, host); // built ...
        book.Clear(kHost);                // ... the feature is released ...
        if (rebind) book.Bind(kHost, kReal1);
        Check(!book.Publish(std::move(snapshot)), "Publish rejects it"); // ... then published
        Check(!book.Newest(), rebind ? "a snapshot of the released generation is not published after a re-bind"
                                     : "a snapshot of a released feature is not published");
    }
    // Same host, another model: only the generation differs.
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    auto snapshot = Take(book, host);
    book.Bind(kHost, kReal2);
    Check(!book.Publish(std::move(snapshot)), "Publish rejects a snapshot of the previous model");
    Check(!book.Newest(), "a snapshot of the previous model is not published");
    // Another host handle with a matching generation number cannot be published either.
    TraceGeneration(book, host, kReal2, false);
    snapshot = Take(book, host);
    snapshot->tag.hostHandle = kOther;
    Check(!book.Publish(std::move(snapshot)), "Publish rejects a snapshot tagged with another feature's handle");
    Check(!book.Newest(), "a snapshot tagged with another feature's handle is not published");
}

// AuxInUse: the caller withdraws the newest snapshot; the generation and its trace stay.
void TestWithdraw() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    book.Publish(Take(book, host));
    Check(book.Newest() != nullptr, "a valid snapshot");
    const MenuTag tag = book.Bind(kHost, kReal1);
    host.Set("DLSSNR.UI", Fake(0x6000)); // the HUD comes on
    std::unique_ptr<MenuParamSnapshot> refused;
    Check(book.Build(&host, Shape(1920, 1080), &refused) == MenuTake::AuxInUse, "the build is refused");
    book.Withdraw(tag);
    Check(!book.Newest(), "the stale snapshot is withdrawn");
    Check(book.Tag().generation == tag.generation && book.Traced(), "the generation and its trace stay");
    host.Set("DLSSNR.UI", static_cast<ID3D12Resource *>(nullptr)); // the HUD goes off
    book.Publish(Take(book, host));
    Check(book.Newest() != nullptr, "a later valid snapshot is published again");
    MenuTag stale = tag;
    ++stale.generation;
    book.Withdraw(stale);
    Check(book.Newest() != nullptr, "a withdrawal of another generation changes nothing");
    MenuTag foreign = tag;
    foreign.hostHandle = kOther; // this generation number, another feature's handle
    book.Withdraw(foreign);
    Check(book.Newest() != nullptr, "a withdrawal for another feature's handle changes nothing");
}

// Codex C1: a host evaluate that began before a release must not bind (and so never publish) after it; any feature's
// release counts, and a release after the bind is caught by Publish.
void TestReleaseDuringEvaluate() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    book.Publish(Take(book, host));
    const std::uint64_t atStart = book.Releases(); // the host evaluate begins
    book.Clear(kHost);                              // HookRelease runs meanwhile
    Check(!book.BindIfNoRelease(kHost, kReal1, atStart), "a bind after a release of the feature is refused");
    Check(!book.Tag().hostHandle && !book.Newest(), "the released feature stays unbound, nothing is served");
    std::unique_ptr<MenuParamSnapshot> late;
    Check(book.Build(&host, Shape(1920, 1080), &late) == MenuTake::NotTraced, "nothing can be built for it");
    const std::uint64_t beforeOther = book.Releases();
    book.Clear(kOther); // another (unbound) feature's release also invalidates the evaluate's token
    Check(!book.BindIfNoRelease(kHost, kReal1, beforeOther), "a bind after any release is refused (retried next evaluate)");
    MenuTag bound;
    Check(book.BindIfNoRelease(kHost, kReal1, book.Releases(), &bound) && book.Tag().realHandle == kReal1, "a bind with a current token works");
    Check(bound.hostHandle == kHost && bound.realHandle == kReal1 && bound.generation == book.Tag().generation,
          "the bind hands the evaluate its tag");
    // An old evaluate's refusal withdraws only its own tag: a newer generation's snapshot stays (Codex re-review).
    {
        MenuParamBook other;
        OwnParams otherHost = HostBlock();
        MenuTag old;
        Check(other.BindIfNoRelease(kHost, kReal1, other.Releases(), &old), "old evaluate bound");
        other.Clear(kHost);                // released ...
        TraceGeneration(other, otherHost, kReal2, false); // ... a new model of the feature traced ...
        other.Publish(Take(other, otherHost));            // ... and published
        other.Withdraw(old);               // the old evaluate's refusal comes late
        Check(other.Newest() != nullptr, "a late refusal of an old generation leaves the new snapshot served");
        std::unique_ptr<MenuParamSnapshot> built;
        Check(other.Build(&otherHost, Shape(1920, 1080), &built) == MenuTake::Taken && built->tag.generation != old.generation,
              "a late build carries the current tag, which the old evaluate compares with its own and drops");
    }
    // Released between the bind and the publish: Publish drops it.
    for (unsigned n = 0; n < MenuParamBook::kTracedEvaluates; ++n) {
        const MenuEvaluate evaluate = book.BeginEvaluate(&host);
        Evaluate(evaluate.block, false);
        book.EndEvaluate(evaluate, true);
    }
    auto snapshot = Take(book, host);
    book.Clear(kHost);
    Check(!book.Publish(std::move(snapshot)) && !book.Newest(), "a snapshot built before the release is dropped at publish");
}

// Codex C2 / reviewer I2: after every host evaluate of the bound feature, a model other than the bound one (re-created)
// or a failed model call drops the served snapshot at once; the generation is re-bound only by a successful call.
void TestModelChangedOrFailed() {
    MenuParamBook book;
    OwnParams host = HostBlock();
    TraceGeneration(book, host, kReal1, false);
    book.Publish(Take(book, host));
    book.AfterHostEvaluate(kHost, kReal1, false);
    Check(book.Newest() != nullptr, "the same model without a failure keeps the snapshot (a carried frame)");
    book.AfterHostEvaluate(kOther, kReal2, true);
    Check(book.Newest() != nullptr, "another feature's evaluate changes nothing");
    book.AfterHostEvaluate(kHost, kReal2, false);
    Check(!book.Newest(), "a re-created model drops the snapshot of the old one at once");
    Check(book.Tag().realHandle == kReal1, "the new model is not bound before a successful call");
    book.Publish(Take(book, host));
    Check(book.Newest() != nullptr, "(published again for the next case)");
    book.AfterHostEvaluate(kHost, kReal1, true);
    Check(!book.Newest(), "a failed model call drops the snapshot");
}

void TestShapeProblems() {
    Check(MenuShapeProblem(Shape(1920, 1080), 1920, 1080) == nullptr, "the whole frame with the model at frame size runs");
    MenuShape reduced = Shape(1920, 1080); // not `small`: rpcndr.h defines it as char
    reduced.colourRect[2] = 960; reduced.colourRect[3] = 540;
    Check(MenuShapeProblem(reduced, 1920, 1080) != nullptr, "a 960x540 colour (renodx 5.2.1's first feature) is refused");
    MenuShape padded = Shape(1920, 1080);
    padded.outputRect[0] = 64;
    Check(MenuShapeProblem(padded, 1920, 1080) != nullptr, "an output region that is not at 0,0 is refused");
    MenuShape compressed = Shape(1920, 1080);
    compressed.modelWidth = 1728; compressed.modelHeight = 972;
    Check(MenuShapeProblem(compressed, 1920, 1080) == nullptr, "from stage 3 a compressed model runs through the core");
    Check(MenuUsesCore(compressed, 1920, 1080, 0) && !MenuUsesCore(Shape(1920, 1080), 1920, 1080, 0),
          "the core runs a compressed model's menu frame, the direct pass the rest");
    Check(MenuShapeProblem(Shape(1920, 1080), 3840, 2160) != nullptr, "a snapshot of another frame size is refused");
}

void TestTemporalPaths() {
    const MenuShape whole = Shape(1920, 1080);
    MenuShape compressed = Shape(1920, 1080);
    compressed.modelWidth = 1728; compressed.modelHeight = 972;
    Check(!MenuUsesCore(whole, 1920, 1080, 0) && MenuUsesCore(whole, 1920, 1080, 1) && !MenuUsesCore(whole, 1920, 1080, 3),
          "Mode Off: the core only for the sync cadence; every-frame and background run the direct pass");
    Check(MenuUsesCore(compressed, 1920, 1080, 0) && MenuUsesCore(compressed, 1920, 1080, 1), "a compressed model always goes through the core");
    Check(MenuTemporalProblem(compressed, 1920, 1080, 3) != nullptr && !MenuTemporalProblem(whole, 1920, 1080, 3) &&
              !MenuTemporalProblem(compressed, 1920, 1080, 1),
          "the background mode is refused only where the core would have to run it");
}
} // namespace

int main() {
    TestGenerations();
    TestUnionKeys();
    TestResultCodes();
    TestAux();
    TestPassBlockAndPointers();
    TestClear();
    TestInflightEvaluate();
    TestNestedBegin();
    TestStaleToken();
    TestConcurrentRelease();
    TestFailedEvaluates();
    TestBuildReleasePublish();
    TestWithdraw();
    TestReleaseDuringEvaluate();
    TestModelChangedOrFailed();
    TestShapeProblems();
    TestTemporalPaths();
    if (failures == 0) std::printf("menu param book: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
