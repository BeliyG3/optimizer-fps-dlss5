#include "hosts/reshade/menu_flow_switch.h"

namespace ofps::reshade {

MenuFlowSwitch::Outcome MenuFlowSwitch::Set(bool on, const Setter &setter) {
    if (on == on_) return Outcome::Unchanged;
    if (!setter) {
        if (!on) { on_ = false; owed_ = false; } // no core any more: nothing runs the flow
        return Outcome::NoSwitch;
    }
    if (setter(on ? 1u : 0u) != 0) {
        if (!on) owed_ = true;
        return Outcome::Refused;
    }
    on_ = on;
    if (!on) owed_ = false;
    return Outcome::Switched;
}

MenuFlowSwitch::Gate MenuFlowSwitch::BeforeHostEvaluate(const Setter &setter) {
    if (!on_) return Gate::Call;
    return Set(false, setter) == Outcome::Refused ? Gate::Withhold : Gate::Restored;
}

} // namespace ofps::reshade
