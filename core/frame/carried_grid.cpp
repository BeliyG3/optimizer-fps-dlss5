#include "core/frame/carried_grid.h"

#include "core/context.h"
#include "core/temporal/grid.h"

namespace ofps::core {

void SetCarriedGrid(FeatureState &st, ofps::core::temporal::FrameInputs &tin)
{
    // An install without the three grid shaders (an older one, kept by the installer) reprojects natively whatever
    // the setting says; the grid that is reported and passed on is the one that really runs, so it is native then.
    const bool gridShaders = Ctx().shaders.TemporalGridLoaded();
    const ofps::core::temporal::CarriedGrid grid = ofps::core::temporal::CarriedGridFor(
        st.layout, st.warped && !st.modelResolution, Ctx().temporal.grid && gridShaders);
    tin.gridWidth = grid.width;
    tin.gridHeight = grid.height;
    if (Ctx().temporal.mode == 0) return; // no carried frames: the line would only mislead
    if (grid.width == st.carriedGrid.width && grid.height == st.carriedGrid.height) return;
    st.carriedGrid = grid;
    Log(false, "Optimizer FPS core: temporal grid %ux%u for native %ux%u (model %ux%u)", grid.width, grid.height,
        st.layout.nativeWidth, st.layout.nativeHeight, st.layout.workWidth, st.layout.workHeight);
}

} // namespace ofps::core
