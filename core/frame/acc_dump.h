#pragma once

// DebugTemporalDumpAcc=1: the file a full pass of temporal mode 1 leaves next to the host executable,
// ofps_acc_<eval>.bin (OFAC v2). The header below, then width*height float pairs of the motion the model
// got (PSModelMotion: the chain, or the "none" marker (4*width, 0) where its tests failed), then
// width*height float pairs of the chain itself, then width*height bytes of why each texel was marked
// (PwTMarkerReason: 0 kept, 1 off screen, 2 depth test, 3 colour gate, 4 non-finite). Units: motion-texture
// pixels, current -> residual frame, the host's mvScale and sign already applied. v1 had no reason plane. v3 (written
// under DebugTemporalCarryChainRule above 0) adds width*height float pairs of the chain the carried frames reproject along
// (the carry chain; the main chain on a full pass, which records none).
// DebugTemporalDumpAccFrom=N (> 0): instead of every full pass, every evaluate of N..N+63 (AccDumpWanted), carried
// frames included. A carried frame calls no model: its model plane holds the chain too and its reasons are all 0.
// Read by tools/bench12/foliage (f16io.read_ofac).

#include <cstdint>
#include <filesystem>
#include <span>

namespace ofps::core {

struct AccDumpHeader {
    char magic[4] = {'O', 'F', 'A', 'C'};
    std::uint32_t version = 2;          // WriteAccDump sets 2, or 3 with a carry plane
    std::uint32_t width = 0, height = 0; // the motion texture
    std::uint32_t eval = 0;              // the core's evaluate counter on the dumped frame
    std::uint32_t fullPassIndex = 0;     // full passes before this frame
    float mvScaleX = 1.0f, mvScaleY = 1.0f; // the host's scale the chain was built with
};
static_assert(sizeof(AccDumpHeader) == 32, "OFAC header is 32 bytes");

// Evaluates DebugTemporalDumpAccFrom's window covers.
constexpr std::uint32_t kAccDumpWindow = 64;

// Whether evaluate `eval` is dumped: every full pass without a window (`from` 0), else every evaluate of the window.
inline bool AccDumpWanted(std::uint32_t eval, bool fullPass, std::uint32_t from)
{
    return from == 0 ? fullPass : eval >= from && eval - from < kAccDumpWindow;
}

// `model` and `chain` (and `carry` when not empty: OFAC v3) must hold 2*width*height floats, `reason` width*height bytes.
// Writes `path`.tmp and renames it over `path`; false on any failure (nothing is left behind).
bool WriteAccDump(const std::filesystem::path &path, const AccDumpHeader &header, std::span<const float> model,
                  std::span<const float> chain, std::span<const std::uint8_t> reason, std::span<const float> carry = {});

} // namespace ofps::core
