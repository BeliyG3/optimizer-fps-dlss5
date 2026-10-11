// DebugTemporalDumpAcc's file writer (core/frame/acc_dump.h): the OFAC v2 layout f16io.read_ofac reads - the header,
// the model plane, the chain plane and the marker reasons' plane - and v3 with the carry plane after them; and
// AccDumpWanted, DebugTemporalDumpAccFrom's window.
// argv[1]: a directory in the build tree the test may write into.
#include "core/frame/acc_dump.h"

#include <fstream>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace ofps::core;

int main(int argc, char **argv)
{
    if (argc < 2) return 10;
    // No window: full passes only. A window of 64 evaluates from `from`: every frame in it, none outside.
    if (!AccDumpWanted(5, true, 0) || AccDumpWanted(5, false, 0)) return 11;
    if (!AccDumpWanted(600, false, 600) || !AccDumpWanted(663, true, 600) || !AccDumpWanted(630, false, 600)) return 12;
    if (AccDumpWanted(599, true, 600) || AccDumpWanted(664, false, 600) || AccDumpWanted(664, true, 600)) return 13;
    if (AccDumpWanted(5, false, 0xFFFFFFF0u) || !AccDumpWanted(0xFFFFFFFFu, false, 0xFFFFFFF0u)) return 14; // no wrap-around
    AccDumpHeader h{};
    h.width = 4; h.height = 2; h.eval = 17; h.fullPassIndex = 2; h.mvScaleX = 1; h.mvScaleY = 1;
    std::vector<float> model(16, 1.5f), chain(16, -2.f);
    const std::vector<std::uint8_t> reason{0, 1, 2, 3, 4, 0, 2, 3};
    const std::filesystem::path p = std::filesystem::path(argv[1]) / "ofps_acc_test.bin";
    if (!WriteAccDump(p, h, model, chain, reason)) return 1;
    std::ifstream f(p, std::ios::binary);
    if (!f) return 5;
    constexpr std::size_t kSize = 32 + 2 * 16 * 4 + 8;
    unsigned char bytes[kSize + 1] = {};
    f.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
    const auto size = static_cast<std::size_t>(f.gcount());
    if (std::memcmp(bytes, "OFAC", 4) != 0) return 2;
    if (size != kSize) return 3;
    std::uint32_t version = 0, eval = 0;
    float firstModel = 0, firstChain = 0;
    std::memcpy(&version, bytes + 4, 4);
    std::memcpy(&eval, bytes + 16, 4);
    std::memcpy(&firstModel, bytes + 32, 4);
    std::memcpy(&firstChain, bytes + 32 + 16 * 4, 4);
    if (version != 2 || eval != 17 || firstModel != 1.5f || firstChain != -2.f) return 6;
    if (std::memcmp(bytes + 32 + 2 * 16 * 4, reason.data(), reason.size()) != 0) return 8; // the reason plane, last
    if (WriteAccDump(p, h, model, std::span<const float>(chain).first(8), reason)) return 4; // size mismatch must fail
    if (WriteAccDump(p, h, model, chain, std::span<const std::uint8_t>(reason).first(4))) return 9;
    if (std::filesystem::exists(std::filesystem::path(p) += ".tmp")) return 7;      // nothing left behind
    // v3 (DebugTemporalCarryChainRule): the same planes, then the carry chain's.
    f.close(); // the rename below replaces the file
    const std::vector<float> carry(16, 0.25f);
    if (!WriteAccDump(p, h, model, chain, reason, carry)) return 15;
    std::ifstream f3(p, std::ios::binary);
    unsigned char v3[kSize + 16 * 4 + 1] = {};
    f3.read(reinterpret_cast<char *>(v3), sizeof(v3));
    float firstCarry = 0, lastCarry = 0;
    std::memcpy(&version, v3 + 4, 4);
    std::memcpy(&firstCarry, v3 + kSize, 4);
    std::memcpy(&lastCarry, v3 + kSize + 15 * 4, 4);
    if (static_cast<std::size_t>(f3.gcount()) != kSize + 16 * 4 || version != 3 || std::memcmp(v3 + 8, bytes + 8, kSize - 8) != 0 ||
        firstCarry != 0.25f || lastCarry != 0.25f)
        return 16;
    if (WriteAccDump(p, h, model, chain, reason, std::span<const float>(carry).first(8))) return 17; // a short carry plane must fail
    return 0;
}
