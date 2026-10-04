// SetCarriedGrid (core/frame/carried_grid.h): which of a feature's frames get the model's grid, and the one log
// line per change of the chosen size. Part of ofps_temporal_grid_tests (test_temporal_grid.cpp calls it).
#include "core/context.h"
#include "core/frame/carried_grid.h"
#include "core/log.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_lines;
int g_failures = 0;

void Capture(bool, const char *message) { g_lines.emplace_back(message); }

void Check(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

// Peripheral 80/90 at Global scale 50 on 3840x2160: model 1728x972 from raw work 3456x1944, density 0.5.
ofps::sdk::LayoutV2 Peripheral50()
{
    ofps::sdk::ConfigV2 config = ofps::sdk::DefaultConfigV2();
    config.globalScalePercent = 50.0f;
    ofps::sdk::LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok, "frame grid: the layout builds");
    return layout;
}

bool Grid(ofps::core::FeatureState &st, std::uint32_t width, std::uint32_t height)
{
    ofps::core::temporal::FrameInputs tin;
    tin.gridWidth = tin.gridHeight = 7; // overwritten either way
    ofps::core::SetCarriedGrid(st, tin);
    return tin.gridWidth == width && tin.gridHeight == height;
}
} // namespace

int TestCarriedGridFrame()
{
    using ofps::core::Ctx;
    ofps::core::SetLog(Capture);
    const bool saved = Ctx().temporal.grid;
    const int savedMode = Ctx().temporal.mode;
    // The grid applies only where the three grid shaders are loaded: stand-in bytes for them here, restored below.
    using ShaderBytes = std::vector<char> ofps::core::gpu::Shaders::*;
    const ShaderBytes gridShaderMembers[] = {&ofps::core::gpu::Shaders::temporalReprojectGrid,
                                             &ofps::core::gpu::Shaders::temporalComposeGrid,
                                             &ofps::core::gpu::Shaders::temporalCellsGrid};
    std::vector<char> savedShaders[3];
    for (int i = 0; i < 3; ++i) {
        savedShaders[i] = Ctx().shaders.*gridShaderMembers[i];
        Ctx().shaders.*gridShaderMembers[i] = std::vector<char>(1, 1);
    }
    Check(Ctx().shaders.TemporalGridLoaded(), "frame grid: the stand-in grid shaders count as loaded");
    Ctx().temporal.grid = true;
    ofps::core::FeatureState st;
    st.layout = Peripheral50();
    st.warped = true;

    Ctx().temporal.mode = 0;
    Check(Grid(st, 1920, 1080) && g_lines.empty(), "every-frame mode: the inputs carry the grid, but nothing is logged");
    Ctx().temporal.mode = 1;
    Check(Grid(st, 1920, 1080), "a warped frame gets the model's grid");
    Check(g_lines.size() == 1 && g_lines.back() == "Optimizer FPS core: temporal grid 1920x1080 for native 3840x2160 (model 1728x972)",
          "the chosen grid is logged with the fixed line");
    Check(Grid(st, 1920, 1080) && g_lines.size() == 1, "the same grid again is not logged again");

    st.modelResolution = true;
    Check(Grid(st, 0, 0), "a model-resolution host stays native");
    Check(g_lines.size() == 2 && g_lines.back() == "Optimizer FPS core: temporal grid 0x0 for native 3840x2160 (model 1728x972)",
          "the change back to native is logged once");
    st.modelResolution = false;
    st.warped = false;
    Check(Grid(st, 0, 0) && g_lines.size() == 2, "an unwarped frame stays native, whatever the stored layout");
    st.warped = true;
    Ctx().temporal.grid = false;
    Check(Grid(st, 0, 0) && g_lines.size() == 2, "TemporalGrid off keeps the native reprojection");
    Ctx().temporal.grid = true;
    Check(Grid(st, 1920, 1080) && g_lines.size() == 3, "switched back on between frames, the grid returns (logged once)");

    // An install without the grid shaders (the core falls back to the native reprojection): the inputs stay native
    // and the one line reports 0x0, never a grid that did not run. Whichever of the three is missing.
    for (int i = 0; i < 3; ++i) {
        const std::size_t before = g_lines.size();
        Ctx().shaders.*gridShaderMembers[i] = std::vector<char>();
        Check(!Ctx().shaders.TemporalGridLoaded(), "frame grid: a missing grid shader means the grid is not loaded");
        Check(Grid(st, 0, 0), "grid shaders not loaded: native although TemporalGrid is on");
        Check(g_lines.size() == before + 1 &&
                  g_lines.back() == "Optimizer FPS core: temporal grid 0x0 for native 3840x2160 (model 1728x972)",
              "grid shaders not loaded: one line, 0x0");
        Check(Grid(st, 0, 0) && g_lines.size() == before + 1, "grid shaders not loaded: not logged again");
        Ctx().shaders.*gridShaderMembers[i] = std::vector<char>(1, 1);
        Check(Grid(st, 1920, 1080) && g_lines.size() == before + 2, "the shaders back: the grid returns (logged once)");
    }

    Ctx().temporal.grid = saved;
    Ctx().temporal.mode = savedMode;
    for (int i = 0; i < 3; ++i) Ctx().shaders.*gridShaderMembers[i] = savedShaders[i];
    ofps::core::SetLog(nullptr);
    if (g_failures == 0) std::cout << "PASS: SetCarriedGrid\n";
    return g_failures;
}
