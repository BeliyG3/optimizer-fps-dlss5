#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

// --switch N: every N frames the add-on's layout is cycled Off -> Uniform -> Peripheral through its
// PeripheralWarpSetLayoutV1 export, so a run re-creates the model and retires GPU objects while
// frames are in flight (the path that waits on every registered queue).
class NrLayoutSwitch {
public:
    void Configure(int every) { every_ = every; }
    void Tick(int frame)
    {
        if (every_ <= 0 || frame <= 0) return;
        if (!resolved_) {
            resolved_ = true;
            HMODULE addon = GetModuleHandleW(L"optimizer-fps-dlss5.addon64");
            set_ = addon ? reinterpret_cast<SetLayout>(GetProcAddress(addon, "PeripheralWarpSetLayoutV1")) : nullptr;
            std::printf("[switch] every %d frames: export %s\n", every_, set_ ? "found" : "MISSING");
        }
        if (set_ == nullptr) return;
        if (frame % every_ == 0) pending_ = true;
        if (!pending_) return;
        State s{sizeof(State), kCycle[index_ % 3], 1, 80.0f, 90.0f, 80.0f, 90.0f, 100.0f, 0};
        const std::uint32_t status = set_(&s);
        std::printf("[switch] frame %d: mode %u (status %u)\n", frame, s.mode, status);
        if (status == 0) { pending_ = false; ++index_; } // NotReady: retried on the next frame
    }
private:
    struct State { std::uint32_t size, mode, filter; float cx, wx, cy, wy, scale; std::uint32_t generation; };
    using SetLayout = std::uint32_t (*)(const State *);
    static constexpr std::uint32_t kCycle[3] = {0, 1, 2};
    int every_ = 0;
    bool resolved_ = false, pending_ = false;
    unsigned index_ = 0;
    SetLayout set_ = nullptr;
};
