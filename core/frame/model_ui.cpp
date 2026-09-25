#include "core/frame/model_ui.h"

namespace ofps::core {
bool WithholdModelUi(const OfpsFrameInputs &frame, uint32_t width, uint32_t height) {
    const auto matches = [width, height](const OfpsResource &input) {
        if (!input.res) return true;
        const auto desc = input.res->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Width == width && desc.Height == height &&
            input.rect.x == 0 && input.rect.y == 0 &&
            input.rect.w == width && input.rect.h == height;
    };
    return !matches(frame.ui) || !matches(frame.uiAlpha) || !matches(frame.backbuffer);
}
} // namespace ofps::core
