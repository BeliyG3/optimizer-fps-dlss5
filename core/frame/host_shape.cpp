#include "core/frame/callback_guard.h"
#include "core/frame/host_shape.h"
#include "core/context.h"
#include <cstdio>
#include <cstring>
namespace ofps::core
{
HostShape ReadHostShape(const OfpsFrameInputs &frame)
{
    HostShape s;
    if (frame.color.res == nullptr || frame.output.res == nullptr)
        return s;
    const D3D12_RESOURCE_DESC cd = frame.color.res->GetDesc(), od = frame.output.res->GetDesc();
    s.colorTexW = static_cast<std::uint32_t>(cd.Width);
    s.colorTexH = cd.Height;
    s.outputTexW = static_cast<std::uint32_t>(od.Width);
    s.outputTexH = od.Height;
    s.color = frame.color.rect;
    s.output = frame.output.rect;
    s.complete = true;
    return s;
}
static bool InTexture(const OfpsRect &r, std::uint32_t w, std::uint32_t h)
{
    return r.w != 0 && r.h != 0 && r.x <= w && r.y <= h && r.w <= w - r.x && r.h <= h - r.y;
}
static void Verdict(const FeatureState &st, const HostShape &s, char *why, std::size_t size)
{
    why[0] = 0;
    const std::uint32_t fw = st.nativeWidth, fh = st.nativeHeight;
    const bool colorIsFeature = s.color.w == fw && s.color.h == fh && InTexture(s.color, s.colorTexW, s.colorTexH);
    const bool outputIsFeature =
        s.output.w == fw && s.output.h == fh && InTexture(s.output, s.outputTexW, s.outputTexH);
    if (colorIsFeature && outputIsFeature && s.output.x == 0 && s.output.y == 0)
        return;
    if (colorIsFeature && s.output.w > fw && s.output.h > fh)
    {
        std::snprintf(why, size,
                      "the host upscales inside NR (colour %ux%u -> output %ux%u, "
                      "x%.2f); this build compresses only same-size frames",
                      s.color.w, s.color.h, s.output.w, s.output.h, static_cast<double>(s.output.w) / fw);
        return;
    }
    if (outputIsFeature && !colorIsFeature)
    {
        std::snprintf(why, size, "host colour region is %ux%u, feature is %ux%u (dynamic resolution?)", s.color.w,
                      s.color.h, fw, fh);
        return;
    }
    std::snprintf(why, size,
                  "host output region is %ux%u at %u,%u in a %ux%u texture, "
                  "feature is %ux%u",
                  s.output.w, s.output.h, s.output.x, s.output.y, s.outputTexW, s.outputTexH, fw, fh);
}
bool JudgeHostShape(FeatureState &st, const HostShape &shape)
{
    if (!shape.complete)
        return false;
    if (!st.hostFits && st.hostLatched)
        return false;
    char why[sizeof(st.hostReason)];
    Verdict(st, shape, why, sizeof(why));
    const bool fits = why[0] == 0;
    const bool first = !st.hostJudged;
    st.hostJudged = true;
    if (fits == st.hostFits)
    {
        if (!fits)
        {
            st.hostLatched = true;
            if (std::strcmp(why, st.hostReason) != 0)
            {
                std::snprintf(st.hostReason, sizeof(st.hostReason), "%s", why);
                Log(true, "Optimizer FPS NGX hook: %s; the model still runs untouched", why);
            }
            return false;
        }
        if (first && fits && (shape.outputTexW != shape.output.w || shape.outputTexH != shape.output.h))
            Log(false,
                "Optimizer FPS NGX hook: host output region %ux%u sits in a %ux%u "
                "texture; a compressed frame is copied into that region",
                shape.output.w, shape.output.h, shape.outputTexW, shape.outputTexH);
        return false;
    }
    st.hostFits = fits;
    st.hostLatched = !fits;
    std::snprintf(st.hostReason, sizeof(st.hostReason), "%s", why);
    if (!fits)
    {
        char inputs[640] = {};
        if (st.modelHost != nullptr)
            GuardCallback([&] { return st.modelHost->DescribeInputs(inputs, sizeof(inputs)); }, 0);
        Log(true,
            "Optimizer FPS NGX hook: %s; the model runs untouched at the feature's "
            "size until the layout is changed. Host block: %s",
            why, inputs);
        OfpsEventData data{};
        data.size = sizeof(data);
        data.payload = nullptr;
        data.handle = st.hostHandle;
        data.text = st.hostReason;
        EmitEvent(OFPS_EVENT_HOST_SHAPE_REJECTED, data);
    }
    else
    {
        Log(false,
            "Optimizer FPS NGX hook: host frame fits again (colour %ux%u, output "
            "%ux%u at 0,0 in a %ux%u texture)",
            shape.color.w, shape.color.h, shape.output.w, shape.output.h, shape.outputTexW, shape.outputTexH);
    }
    return true;
}
} // namespace ofps::core
