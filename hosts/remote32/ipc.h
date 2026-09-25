#pragma once

#include "core/api/ofps_core.h"
#include "core/api/ofps_settings_schema.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ofps::remote {

inline constexpr wchar_t kMappingName[] = L"Local\\OptimizerFpsRemoteV4";
inline constexpr wchar_t kOldMappingName[] = L"Local\\PeripheralWarpRemoteV3";
inline constexpr wchar_t kRetryName[] = L"Local\\OptimizerFpsRemoteRetryV4";
inline constexpr wchar_t kLeavingFormat[] = L"Local\\OptimizerFpsRemoteLeaving_%lu";
inline constexpr std::uint32_t kMagic = 0x3453464fu; // OFS4
inline constexpr std::uint32_t kVersion = 4u;
inline constexpr std::uint64_t kHeartbeatTimeoutMs = 3000u;
inline constexpr std::uint32_t kMaxRows = 32u;
inline constexpr std::uint32_t kLineLength = 96u;
inline constexpr std::uint32_t kReasonLength = 128u;

#pragma pack(push, 8)

struct WireRange {
    float lo;
    float hi;
    std::uint32_t valid;
};

struct WireStatus {
    std::uint32_t active;
    std::uint32_t featureCreated;
    std::uint32_t adopted;
    std::uint32_t directHost;
    std::uint32_t deviceRemoved;
    std::uint32_t nativeW, nativeH, workW, workH;
    std::uint32_t temporalMode, warpPath;
    std::uint64_t evaluations, passthroughs, fallbackFrames;
    std::uint64_t fullFrames, interpFrames;
    float modelMs;
    std::uint32_t modelSamples;
    std::uint32_t modelPassesRunning;
    std::int32_t lastModelResult;
    char reason[kReasonLength];
    char modelPassReason[kReasonLength];
    char fallbackReason[kReasonLength];
    char temporalReason[kReasonLength];
};

struct WireRow {
    std::uint32_t group;
    std::uint32_t severity;
    char label[kLineLength];
    char value[kLineLength];
};

struct WireShell {
    std::uint32_t hookState;
    std::uint32_t crashGuard;
    std::uint32_t retryAvailable;
    std::uint32_t feederAvailable;
    std::uint32_t feederActive;
    std::uint32_t feederSource;
    std::uint32_t feederGrid;
    std::uint32_t feederPerf;
    std::uint32_t shellVersion;
    char connection[kReasonLength];
};

struct WireSnapshot {
    OfpsSettingsValues applied;
    WireStatus status;
    std::uint32_t rowCount;
    WireRow rows[kMaxRows];
    OfpsLayoutPreview preview;
    WireRange ranges[OFPS_SETTINGS_MAX];
    std::uint32_t hostCaps;
    std::uint8_t available[OFPS_SETTINGS_MAX];
    char unavailableReason[OFPS_SETTINGS_MAX][kReasonLength];
    WireShell shell;
    std::uint64_t hostHeartbeatTick;
    std::uint32_t appliedSettingsGeneration;
};

// Feeder OFA belongs to the shell config, outside the core's 45 setting IDs.
// It shares the remote-to-host sequence with settings so one edit is coherent.
struct WireFeederEdit {
    std::uint32_t source;
    std::uint32_t grid;
    std::uint32_t perf;
};

struct BlockV4 {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t reserved;
    // Even generation means stable. Writer sets odd, copies, then sets even.
    volatile long settingsSequence;
    std::uint32_t settingsGeneration;
    OfpsSettingsValues settings;
    WireFeederEdit feederEdit;
    volatile long statusSequence;
    std::uint32_t statusGeneration;
    WireSnapshot snapshot;
};

#pragma pack(pop)

static_assert(std::is_standard_layout_v<WireStatus>);
static_assert(std::is_standard_layout_v<WireSnapshot>);
static_assert(std::is_standard_layout_v<BlockV4>);
static_assert(sizeof(WireStatus) == 616u);
static_assert(sizeof(WireRow) == 200u);
static_assert(sizeof(WireSnapshot) == 25920u);
static_assert(offsetof(WireSnapshot, rows) == 1156u);
static_assert(offsetof(WireSnapshot, ranges) == 7688u);
static_assert(offsetof(WireSnapshot, shell) == 25740u);
static_assert(alignof(BlockV4) == 8u);
static_assert(sizeof(BlockV4) == 26504u);
static_assert(offsetof(BlockV4, settings) == 24u);
static_assert(offsetof(BlockV4, feederEdit) == 560u);
static_assert(offsetof(BlockV4, statusSequence) == 572u);
static_assert(offsetof(BlockV4, snapshot) == 584u);
static_assert(offsetof(BlockV4, settings) > offsetof(BlockV4, settingsGeneration));
static_assert(offsetof(BlockV4, feederEdit) == offsetof(BlockV4, settings) + sizeof(OfpsSettingsValues));
static_assert(offsetof(BlockV4, snapshot) > offsetof(BlockV4, statusGeneration));
static_assert(sizeof(OfpsSettingsValues) <= 2048u);

inline bool ValidHeader(const BlockV4 &block, std::size_t mappedSize) {
    return mappedSize >= sizeof(BlockV4) && block.magic == kMagic &&
           block.version == kVersion && block.size == sizeof(BlockV4);
}

inline bool ValidSettings(const OfpsSettingsValues &settings) {
    return settings.size == sizeof(OfpsSettingsValues) &&
           settings.count == OFPS_SET_COUNT &&
           settings.count <= OFPS_SETTINGS_MAX;
}

inline bool FreshHeartbeat(std::uint64_t now, std::uint64_t tick) {
    return tick != 0 && now >= tick && now - tick < kHeartbeatTimeoutMs;
}

inline bool HostCapSupported(std::uint32_t available, std::uint32_t required) {
    return required == 0 || (available & required) == required;
}

inline bool ReadEdit(BlockV4 &block, std::uint32_t &seen,
                     OfpsSettingsValues &settings, WireFeederEdit &feeder) {
    for (int attempt = 0; attempt != 8; ++attempt) {
        const long before = InterlockedCompareExchange(&block.settingsSequence, 0, 0);
        if ((before & 1L) != 0) continue;
        std::memcpy(&settings, &block.settings, sizeof(settings));
        std::memcpy(&feeder, &block.feederEdit, sizeof(feeder));
        const std::uint32_t candidate = block.settingsGeneration;
        const long after = InterlockedCompareExchange(&block.settingsSequence, 0, 0);
        if (before == after && (after & 1L) == 0) {
            seen = candidate;
            return true;
        }
    }
    return false;
}

inline void PublishHostSnapshot(BlockV4 &block, const WireSnapshot &next);

// Exactly one writer per direction. The mapped block starts zeroed; publish
// the generation and payload under the same sequence to reject mixed frames.
template <class Payload>
inline void Publish(volatile long *sequence, std::uint32_t &generation,
                    Payload &destination, const Payload &source) {
    InterlockedIncrement(sequence); // odd: writer owns the payload
    std::memcpy(&destination, &source, sizeof(source));
    ++generation;
    InterlockedIncrement(sequence); // even: publish with a full barrier
}

template <class Payload>
inline bool ReadStable(volatile long *sequence, const std::uint32_t &generation,
                       const Payload &source, std::uint32_t &seen,
                       Payload &local) {
    for (int attempt = 0; attempt != 8; ++attempt) {
        const long before = InterlockedCompareExchange(sequence, 0, 0);
        if ((before & 1L) != 0) continue;
        std::memcpy(&local, &source, sizeof(local));
        const std::uint32_t candidate = generation;
        const long after = InterlockedCompareExchange(sequence, 0, 0);
        if (before == after && (after & 1L) == 0) {
            seen = candidate;
            return true;
        }
    }
    return false;
}

inline void PublishHostSnapshot(BlockV4 &block, const WireSnapshot &next) {
    Publish(&block.statusSequence, block.statusGeneration, block.snapshot, next);
}

inline void CopyText(char *out, std::size_t capacity, const char *input) {
    if (capacity == 0) return;
    if (input == nullptr) { out[0] = 0; return; }
    std::size_t n = 0;
    while (input[n] != 0 && n + 1 < capacity) ++n;
    // Keep the prefix at a code-point boundary when UTF-8 is truncated.
    if (input[n] != 0) {
        while (n > 0 && (static_cast<unsigned char>(input[n]) & 0xc0u) == 0x80u)
            --n;
    }
    std::memcpy(out, input, n);
    out[n] = 0;
}

inline WireStatus CopyStatus(const OfpsStatus &source) {
    WireStatus out{};
    out.active = source.active;
    out.featureCreated = source.featureCreated;
    out.adopted = source.adopted;
    out.directHost = source.directHost;
    out.deviceRemoved = source.deviceRemoved;
    out.nativeW = source.nativeW; out.nativeH = source.nativeH;
    out.workW = source.workW; out.workH = source.workH;
    out.temporalMode = source.temporalMode; out.warpPath = source.warpPath;
    out.evaluations = source.evaluations;
    out.passthroughs = source.passthroughs;
    out.fallbackFrames = source.fallbackFrames;
    out.fullFrames = source.fullFrames;
    out.interpFrames = source.interpFrames;
    out.modelMs = source.modelMs;
    out.modelSamples = source.modelSamples;
    out.modelPassesRunning = source.modelPassesRunning;
    out.lastModelResult = source.lastModelResult;
    CopyText(out.reason, sizeof(out.reason), source.reason);
    CopyText(out.temporalReason, sizeof(out.temporalReason), source.temporalReason);
    CopyText(out.modelPassReason, sizeof(out.modelPassReason), source.modelPassReason);
    CopyText(out.fallbackReason, sizeof(out.fallbackReason), source.fallbackReason);
    return out;
}

inline WireRow CopyRow(const OfpsStatusRow &source) {
    WireRow out{};
    out.group = source.group;
    out.severity = source.severity;
    CopyText(out.label, sizeof(out.label), source.label);
    CopyText(out.value, sizeof(out.value), source.value);
    return out;
}

} // namespace ofps::remote
