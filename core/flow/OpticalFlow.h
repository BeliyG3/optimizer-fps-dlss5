#pragma once
// Optimizer FPS: NVIDIA's hardware optical flow (NVOFA, nvofapi64.dll of the driver) on D3D12.
//
// Why: the temporal mode moves the model's contribution along a chain of the game's per-frame motion
// vectors, and a chain drifts (render-resolution field, one interpolation per link, nothing behind glass
// or in reflections). The optical flow engine measures the displacement between two real frames
// directly - this frame against the frame the model last saw - so the chain is replaced by one
// measurement plus a single link of the game's vectors.
//
// The engine runs on its own hardware queue and is synchronised with fences. The pass records into the
// game's command list and does not own the queue that list is submitted on, so a fence cannot be placed
// behind the recorded work in the same frame. The session therefore runs one frame late: the luminance
// images are written by list t, and at the start of frame t+1 (list t has been submitted by then)
// Execute() signals the input fence on the game's queue, starts the engine, and makes the queue wait for
// the result before list t+1 runs. The engine works while the GPU renders frame t+1, so the wait is
// short or none.
//
// Sessions live as long as the process. The temporal machine is rebuilt whenever the game changes its
// buffers and the replaced one is destroyed some frames later; destroying its session there took the
// driver down inside the release of the session's objects (nvwgf2umx, a null read), on every frame from
// then on (007 First Light, 0.5 s a frame while Streamline wrote a dump for each). A session is a few
// megabytes: Acquire() hands out the one that fits and nothing is torn down while the game runs.
//
// Owns three textures, all resting in COMMON (what the engine expects): two R8 luminance images at the
// session's size and the R16G16_SINT flow field (S10.5 fixed point, one vector per Grid() x Grid() block).

#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <string>

namespace ofps::core::flow
{

class Session
{
  public:
    ~Session();

    // The process-wide session for this device and size, created on first request. The caller borrows it.
    // nullptr with a reason: not an NVIDIA GPU, no engine on this GPU, a driver without the D3D12 entry.
    static Session* Acquire(ID3D12Device* device, std::uint32_t width, std::uint32_t height, std::string& reason);

    std::uint32_t Width() const;
    std::uint32_t Height() const;
    // Pixels of the session per flow vector: 1 or 2 where the engine offers it (Ampere/Ada), else 4.
    std::uint32_t Grid() const;
    ID3D12Resource* Now() const;  // luminance of the frame being measured (input)
    ID3D12Resource* Then() const; // luminance of the frame it is measured against (reference)
    ID3D12Resource* Field() const; // the result: for a pixel of Now, where it is in Then

    // Start of the frame after the one whose list wrote Now/Then. `queue` is the queue the game submits
    // the pass's lists on. False: the engine refused, the field must not be used this frame.
    bool Execute(ID3D12CommandQueue* queue);

  private:
    Session() = default;
    static std::unique_ptr<Session> Create(ID3D12Device* device, std::uint32_t width, std::uint32_t height,
                                           std::string& reason);
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace ofps::core::flow
