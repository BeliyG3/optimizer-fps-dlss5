#include "core/frame/callback_guard.h"
#include "core/gpu/graveyard.h"
#include "core/log.h"

namespace ofps::core::gpu {
void Graveyard::Add(Grave &&grave, std::uint64_t now)
{
    grave.dueEval = now + kDeferredReleaseEvaluates;
    graves_.push_back(std::move(grave));
}

void Graveyard::Drain(bool everything, std::uint64_t now, DWORD waitMs)
{
    for (std::size_t i = 0; i < graves_.size();) {
        Grave &g = graves_[i];
        if (g.gate.Empty()) {
            if (!everything && g.dueEval > now) { ++i; continue; }
            g.gate = SignalGateAll();
            if (g.gate.Empty()) { ++i; continue; } // No queue can confirm completion yet.
            if (everything) g.gate.Wait(waitMs);
        } else if (everything && !g.gate.Completed()) {
            g.gate.Wait(waitMs);
        }
        if (!g.gate.Completed()) {
            if (everything && !g.timeoutLogged) {
                Log(true, "Optimizer FPS core: retirement gate wait timed out; resources remain pending");
                g.timeoutLogged = true;
            }
            ++i;
            continue;
        }
        g.gate.Clear();
        g.adapter12.reset();
        g.disposables.clear();
        for (IUnknown *o : g.objects) if (o) o->Release();
        g.objects.clear();
        if (g.modelHandle && g.modelHost) GuardCallback([&] { return g.modelHost->ReleaseModel(g.modelHandle); });
        graves_.erase(graves_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

bool Graveyard::References(const IOfpsModelHost *host) const
{
    for (const auto &grave : graves_) if (grave.modelHost == host) return true;
    return false;
}
} // namespace ofps::core::gpu
