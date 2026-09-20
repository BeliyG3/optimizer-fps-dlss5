#pragma once

#include <cstdint>

namespace pwtemporal {

// Sync shows the first share through Apply; background shows it through its adoption reprojection.
struct PhaseIn {
    std::uint32_t frames = 0, age = 0;
    bool firstReproject = false;

    void Start(std::uint32_t count, bool background)
    {
        frames = count;
        age = 0;
        firstReproject = background;
    }

    void Reproject()
    {
        if (!firstReproject && age < frames) ++age;
        firstReproject = false;
    }

    float Share() const
    {
        if (frames == 0 || age >= frames) return 1.0f;
        return static_cast<float>(age + 1) / static_cast<float>(frames + 1);
    }
};

} // namespace pwtemporal
