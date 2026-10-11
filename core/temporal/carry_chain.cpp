// TemporalChainRule's carry chain (core/temporal/chain_rule.h): a second chain for the carried frames' reprojection. The
// main chain keeps its rule (0 under the setting, DebugTemporalChainRule otherwise) and feeds the model, its markers, the
// history link, the statistics and the chain dump; the carry chain restarts by its own rule (5 under Coherent, the
// default; DebugTemporalCarryChainRule otherwise) and feeds the shown machine's reprojection (and so its cells and
// compose; hidden spread stages and full passes record none: Quiet, FullPassInputs). Both reset together (RecordResidual,
// Invalidate); the carry chain adopts the main one wherever it has none of its own (after a reset of its own state, an
// adopted background pass, an optical-flow seed, a full pass). Carry rule 0 (Classic) creates and records nothing.
//
// Both chains run in one pass each (temporal_chain_pair.hlsli). AccumulatePair tests every link once, a link the two
// chains share once, votes (rules 3, 5, 6) from groupshared memory and writes both chains; RefinePair registers a chain
// they share once. Rules 0-4 give each chain byte for byte what the single chain's passes give it.
#include "core/temporal/machine.h"

#include "core/temporal/resources.h"

#include <atomic>

namespace ofps::core::temporal {
namespace {

using ofps::core::gpu::Barrier;
constexpr DXGI_FORMAT kChainFormat = DXGI_FORMAT_R16G16_FLOAT;

} // namespace

FrameInputs FullPassInputs(FrameInputs in)
{
    in.carryChainRule = 0;
    return in;
}

bool Machine::CarryChainCreated() const { return res_ != nullptr && res_->accCarry[0] != nullptr && res_->accCarry[1] != nullptr; }

ID3D12Resource *Machine::CarryAcc() const
{
    if (res_ == nullptr) return nullptr;
    return carryValid_ ? res_->accCarry[res_->accCarryCurrent] : res_->acc[res_->accCurrent];
}

bool Machine::RecordChainPair(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, Constants c, int adopt)
{
    Resources &m = *res_;
    c.sky[3] = static_cast<float>(in.carryChainRule); // PwTSky.w: the carry chain's rule, on the pair passes alone
    // Rule 6: last frame's main chain before its Refine (t8), still in acc[next], which AccumulatePair overwrites: a copy.
    const bool unrefined = unrefinedKept_ && in.carryChainRule == 6;
    if (!m.PairReady(c.hostModelGrid != 0) || !m.EnsureCarryChain(unrefined)) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true))
            Log(true, "Optimizer FPS core: TemporalChainRule / DebugTemporalCarryChainRule: the pair passes or the carry chain's textures are missing; "
                      "carried frames use the main chain");
        return false;
    }
    const int prev = m.accCurrent, next = 1 - prev;
    const int carryPrev = m.accCarryCurrent, carryNext = 1 - carryPrev;
    ID3D12Resource *const carryFrom = adopt >= 0 ? m.acc[adopt] : m.accCarry[carryPrev];
    if (adopt < 0) Barrier(cmd, carryFrom, m.accCarryState[carryPrev], kReadable);
    SlotKey key = m.BaseKey(in, m.acc[prev]);
    key.res[2] = carryFrom; key.fmt[2] = kChainFormat;
    key.res[8] = unrefined ? m.accUnrefined : nullptr; key.fmt[8] = kChainFormat;
    c.params[2] = unrefined ? 1.0f : 0.0f;
    if (unrefined) {
        Barrier(cmd, m.acc[next], m.accState[next], D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, m.accUnrefined, m.accUnrefinedState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(m.accUnrefined, m.acc[next]);
        Barrier(cmd, m.accUnrefined, m.accUnrefinedState, kReadable);
    }
    Barrier(cmd, m.acc[next], m.accState[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, m.accCarry[carryNext], m.accCarryState[carryNext], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const Resources::Output carryOut{m.accCarry[carryNext], kChainFormat};
    m.Dispatch(cmd, kAccumulatePair, m.Target(m.accTarget[next]), m.motionW, m.motionH, key, c, &carryOut);
    Barrier(cmd, m.acc[next], m.accState[next], kAccState);
    Barrier(cmd, m.accCarry[carryNext], m.accCarryState[carryNext], kAccState);
    m.accCurrent = next;
    m.accCarryCurrent = carryNext;
    if (m.colorF && in.refine) { // register both chains against the two real pictures, a chain they share once
        c.sky[2] = c.sky[3] = c.params[2] = 0.0f; // the rules and the unrefined chain are the link test's alone
        Barrier(cmd, m.acc[prev], m.accState[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmd, m.accCarry[carryPrev], m.accCarryState[carryPrev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SlotKey rk = m.BaseKey(in, m.acc[next]);
        rk.res[2] = m.accCarry[carryNext]; rk.fmt[2] = kChainFormat;
        const Resources::Output refinedCarry{m.accCarry[carryPrev], kChainFormat};
        m.Dispatch(cmd, kRefinePair, m.Target(m.accTarget[prev]), m.motionW, m.motionH, rk, c, &refinedCarry);
        Barrier(cmd, m.acc[prev], m.accState[prev], kAccState);
        Barrier(cmd, m.accCarry[carryPrev], m.accCarryState[carryPrev], kAccState);
        m.accCurrent = prev;
        m.accCarryCurrent = carryPrev;
    }
    carryValid_ = !m.exhausted;
    return true;
}

} // namespace ofps::core::temporal
