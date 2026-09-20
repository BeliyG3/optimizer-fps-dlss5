#include "../adapters/reshade/ngx/temporal_phase.h"

#include <cstdio>

int main()
{
    pwtemporal::PhaseIn phase;
    auto check = [&](float expected) {
        if (phase.Share() == expected) return true;
        std::fprintf(stderr, "phase share: expected %.3f, got %.3f\n", expected, phase.Share());
        return false;
    };
    // Sync's full frame consumes 1/4 through Apply, then its carried frames start at 2/4.
    phase.Start(3, false);
    if (!check(0.25f)) return 1;
    phase.Reproject();
    if (!check(0.5f)) return 1;
    phase.Reproject();
    if (!check(0.75f)) return 1;
    phase.Reproject();
    if (!check(1.0f)) return 1;

    // The first background reprojection is adoption itself; it must not skip the first share.
    phase.Start(3, true);
    phase.Reproject();
    if (!check(0.25f)) return 1;
    phase.Reproject();
    if (!check(0.5f)) return 1;
    const float interruptedShare = phase.Share();
    phase.Start(3, true);
    phase.Reproject();
    if (!check(0.25f) || interruptedShare != 0.5f) return 1;
    phase.Reproject();
    if (!check(0.5f)) return 1;
    phase.Reproject();
    if (!check(0.75f)) return 1;
    phase.Reproject();
    if (!check(1.0f)) return 1;

    phase.Start(1, true);
    phase.Reproject();
    if (!check(0.5f)) return 1;
    phase.Reproject();
    if (!check(1.0f)) return 1;
    phase.Start(0, true);
    for (int i = 0; i < 100; ++i) {
        phase.Reproject();
        if (!check(1.0f)) return 1;
    }
    phase.Start(3, true);
    phase.Start(0, false); // invalidation drops every part of an interrupted ramp
    phase.Reproject();
    return check(1.0f) ? 0 : 1;
}
