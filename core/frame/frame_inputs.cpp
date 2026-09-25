#include "core/frame/frame_inputs.h"
#include "core/frame/feature_state.h"
namespace ofps::core
{
void ResolveFrameViews(OfpsFrameInputs &frame)
{
    auto resolve = [](OfpsResource &r, bool depth) {
        if (r.res == nullptr)
        {
            r.view = DXGI_FORMAT_UNKNOWN;
            return;
        }
        if (r.view == DXGI_FORMAT_UNKNOWN)
            r.view = TypedView(r.res->GetDesc().Format, depth);
    };
    resolve(frame.color, false);
    resolve(frame.depth, true);
    resolve(frame.motion, false);
    resolve(frame.output, false);
    resolve(frame.ui, false);
    resolve(frame.uiAlpha, false);
    resolve(frame.backbuffer, false);
    for (OfpsResource &c : frame.codecInputs)
        resolve(c, false);
}
bool FrameHasModelInputs(const OfpsFrameInputs &frame)
{
    return frame.color.res != nullptr && frame.depth.res != nullptr && frame.motion.res != nullptr &&
           frame.output.res != nullptr;
}
OfpsResource MakeResource(ID3D12Resource *res, DXGI_FORMAT view, const OfpsRect &rect, D3D12_RESOURCE_STATES restState,
                          UINT subresource)
{
    OfpsResource r{};
    r.size = sizeof(r);
    r.res = res;
    r.view = view;
    r.rect = rect;
    r.restState = restState;
    r.subresource = subresource;
    return r;
}
OfpsModelInputs ModelInputsFrom(const OfpsFrameInputs &frame, std::uint32_t width, std::uint32_t height)
{
    OfpsModelInputs m{};
    m.size = sizeof(m);
    m.color = frame.color;
    m.depth = frame.depth;
    m.motion = frame.motion;
    m.output = frame.output;
    m.ui = frame.ui;
    m.uiAlpha = frame.uiAlpha;
    m.backbuffer = frame.backbuffer;
    m.mvScaleX = frame.mvScaleX;
    m.mvScaleY = frame.mvScaleY;
    m.depthInverted = frame.depthInverted;
    m.reset = frame.hostReset;
    m.withholdUi = 0;
    m.width = width;
    m.height = height;
    return m;
}
} // namespace ofps::core

namespace ofps::core
{
OfpsResource FrameMotion(const FeatureState &st, const OfpsResource &source)
{
    OfpsResource motion = source;
    if (st.frameMotion && st.frameMotion != source.res)
    {
        motion.res = st.frameMotion;
        motion.view = TypedView(st.frameMotion->GetDesc().Format, false);
        motion.restState = kModelInputState;
        motion.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    return motion;
}
} // namespace ofps::core
