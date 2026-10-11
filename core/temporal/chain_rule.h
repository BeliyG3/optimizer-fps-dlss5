#pragma once

// TemporalChainRule (phase 3 step 1 of the camera-motion carry): which chains the carried frames reproject along.
// The user setting picks 0 = classic (one chain, rule 0, as before) or 1 = coherent (two chains: the model's chain
// keeps rule 0, the carried frames reproject along a carry chain by rule 5, which restarts on a depth change or a
// colour change shared by at least 18 of the 25 pixels around; carry_chain.cpp). The diagnostic keys still override
// it: DebugTemporalChainRule > 0 or DebugTemporalCarryChainRule > 0 replace the mapping, both rules then come from
// the two keys as before the setting existed (an unset carry key = no carry chain).
namespace ofps::core::temporal {

struct ChainRules {
    int main = 0;  // FrameInputs::chainRule: the model's chain (PSAccumulate's link test, 0..4)
    int carry = 0; // FrameInputs::carryChainRule: the carry chain (0 = none, 1..6)
};

constexpr int kCoherentCarryChainRule = 5;

constexpr ChainRules ResolveChainRules(int setting, int debugChainRule, int debugCarryChainRule)
{
    const bool debug = debugChainRule > 0 || debugCarryChainRule > 0;
    ChainRules r;
    r.main = debugChainRule > 0 ? debugChainRule : 0;
    r.carry = debug ? (debugCarryChainRule > 0 ? debugCarryChainRule : 0) : (setting == 1 ? kCoherentCarryChainRule : 0);
    return r;
}

} // namespace ofps::core::temporal
