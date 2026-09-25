#pragma once

// The frame a host hands feature 18: where its colour sits and where the answer must go. The warp
// path needs the colour region and the output region at the feature's own size, the output at the
// top-left of its texture (a larger texture is fine: alignment padding, a maximum-size render
// target). Anything else - a model that upscales, a dynamic-resolution colour region, an output
// region elsewhere in its texture - runs the model untouched. The verdict is taken before the layout
// is applied, so a slider move on such a host never re-creates the model twice.

#include "core/frame/feature_state.h"

#include <cstdint>

namespace ofps::core {

struct HostShape {
    bool complete = false; // the block carried both colour and output
    OfpsRect color, output; // the host's regions (the whole texture when it gave none)
    std::uint32_t colorTexW = 0, colorTexH = 0, outputTexW = 0, outputTexH = 0;
};

HostShape ReadHostShape(const OfpsFrameInputs &frame);

// Judges `shape` for the feature and records the verdict in st.hostFits / st.hostReason. An unfit
// verdict is latched until the user changes the layout (a host whose regions move every frame would
// otherwise re-create the model on every flip). Returns true when the verdict changed, so the caller
// re-applies the layout.
bool JudgeHostShape(FeatureState &st, const HostShape &shape);

} // namespace ofps::core
