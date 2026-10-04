#include "bench_scene.h"

#include <cmath>
#include <cstdio>
#include <cstring>

void BuildBoxes(BenchScene &s)
{
    // ---- the 3D scene: instances (boxes + the figure at the origin) ----
    auto addBox = [&](float x, float y, float z, float hx, float hy, float hz, float material) {
        if (s.instanceCount >= 64) return;
        s.inst.pos[s.instanceCount][0] = x; s.inst.pos[s.instanceCount][1] = y; s.inst.pos[s.instanceCount][2] = z;
        s.inst.size[s.instanceCount][0] = hx; s.inst.size[s.instanceCount][1] = hy; s.inst.size[s.instanceCount][2] = hz; s.inst.size[s.instanceCount][3] = material;
        ++s.instanceCount;
    };
    // The figure: legs, torso, head, arms.
    addBox(-0.18f, 0.45f, 0.0f, 0.14f, 0.45f, 0.14f, 2); addBox(0.18f, 0.45f, 0.0f, 0.14f, 0.45f, 0.14f, 2);
    addBox(0.0f, 1.25f, 0.0f, 0.34f, 0.36f, 0.2f, 0); addBox(0.0f, 1.82f, 0.0f, 0.16f, 0.18f, 0.16f, 3);
    addBox(-0.48f, 1.25f, 0.0f, 0.11f, 0.34f, 0.11f, 5); addBox(0.48f, 1.25f, 0.0f, 0.11f, 0.34f, 0.11f, 5);
    // Boxes and columns on rings around the origin (deterministic pseudo-random sizes).
    unsigned seed = 12345u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    for (int ring = 0; ring < 4 && s.instanceCount < 62; ++ring) {
        const float radius = 6.0f + ring * 7.0f;
        const int count = 8 + ring * 4;
        for (int k = 0; k < count && s.instanceCount < 62; ++k) {
            const float a = (k + 0.37f * ring) / count * 6.2831853f;
            const float hx = 0.4f + rnd() * 1.2f, hz = 0.4f + rnd() * 1.2f, hy = 0.6f + rnd() * 2.4f;
            addBox(std::sin(a) * radius + (rnd() - 0.5f) * 2.0f, hy, std::cos(a) * radius + (rnd() - 0.5f) * 2.0f, hx, hy, hz, (float) (k % 6));
        }
    }
    s.movingA = s.instanceCount - 1; s.movingB = s.instanceCount - 2; // the last two boxes may move
    if (GetEnvironmentVariableA("PW_BENCH_LIST_BOXES", nullptr, 0)) {
        for (int i = 0; i < s.instanceCount; ++i)
            std::printf("[box] %2d pos %7.2f %5.2f %7.2f half %5.2f %5.2f %5.2f colour %g\n", i, s.inst.pos[i][0], s.inst.pos[i][1], s.inst.pos[i][2], s.inst.size[i][0], s.inst.size[i][1], s.inst.size[i][2], s.inst.size[i][3]);
        for (int i = 0; i < s.instanceCount; ++i) for (int j = i + 1; j < s.instanceCount; ++j)
            if (std::fabs(s.inst.pos[i][0] - s.inst.pos[j][0]) < s.inst.size[i][0] + s.inst.size[j][0] && std::fabs(s.inst.pos[i][2] - s.inst.pos[j][2]) < s.inst.size[i][2] + s.inst.size[j][2])
                std::printf("[box] overlap %d (colour %g) with %d (colour %g)\n", i, s.inst.size[i][3], j, s.inst.size[j][3]);
    }
    for (int i = 0; i < s.instanceCount; ++i) for (int c = 0; c < 4; ++c) s.inst.prev[i][c] = s.inst.pos[i][c];
}

void AnimateBoxes(BenchScene &s, int frame)
{
    const float t = frame / 60.0f, tp = (frame - 1) / 60.0f;
    auto place = [&](int i, float time, float (*dst)[4]) {
        const float baseX = s.inst.pos[i][0], baseZ = s.inst.pos[i][2];
        dst[i][0] = baseX + std::sin(time * 1.3f) * 1.5f; dst[i][1] = s.inst.size[i][1] + 0.5f + 0.5f * std::sin(time * 2.1f); dst[i][2] = baseZ + std::cos(time * 1.3f) * 1.5f;
    };
    static float basePos[2][4] = {}; static bool baseSaved = false;
    if (!baseSaved) { memcpy(basePos[0], s.inst.pos[s.movingA], 16); memcpy(basePos[1], s.inst.pos[s.movingB], 16); baseSaved = true; }
    memcpy(s.inst.pos[s.movingA], basePos[0], 16); memcpy(s.inst.pos[s.movingB], basePos[1], 16);
    place(s.movingA, tp, s.inst.prev); place(s.movingB, tp, s.inst.prev);
    place(s.movingA, t, s.inst.pos); place(s.movingB, t, s.inst.pos);
}
