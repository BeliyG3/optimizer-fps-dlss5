#pragma once

// Remote overlay IPC (stage 26.11).
//
// 32-bit games whose Neural Rendering runs in a separate 64-bit host process (the DLSS 5 x86 kit:
// the game gets ReShade x86 + dlss5-feed.addon32, the model runs in host64\dlss5-feed-host64.exe
// with ReShade x64 + renodx + optimizer-fps-dlss5.addon64) cannot show our tab: the x64 add-on's
// overlay lives in the host's hidden window. `optimizer-fps-dlss5-remote.addon32` draws the same tab in
// the game's ReShade overlay and talks to the x64 add-on through this shared block.
//
// One writer per direction, no locks: the UI side writes `settings` and then publishes it by
// bumping `settingsGeneration`; the host side writes `status` / `applied` and publishes them with
// `statusGeneration`. Both sides copy whole sub-structs, so a torn read is at worst one frame of a
// mixed value that the next generation corrects. The layout is fixed POD, asserted below, and both
// sides must be built from this header.

#include <cstddef>
#include <cstdint>

// The section name carries the protocol version: a block of one version must never be mapped
// through another version's struct, and two halves of different ages can be live at the same
// moment (a game already running while the next one starts). Versions 1 and 2 shared the name
// below, which is why a version-3 host takes a new one and the tab falls back to the old.
#define PW_REMOTE_MAPPING_NAME_W L"Local\\PeripheralWarpRemoteV3"
#define PW_REMOTE_MAPPING_NAME_OLD_W L"Local\\PeripheralWarpRemoteV1"
#define PW_REMOTE_MAGIC 0x31525750u // 'PWR1'
// 1 = the original block. 2 adds the optical-flow fields at the end of the settings and of the
// status; 3 adds the model passes. Every addition moves the layout of everything after `settings`,
// so the versions are not readable through each other's structs and this header keeps the older
// layouts verbatim below. The host writes the newest one; a tab built against an older header
// simply refuses that block, so the two halves are shipped together.
#define PW_REMOTE_VERSION 3u
#define PW_REMOTE_VERSION_OFA 2u
#define PW_REMOTE_VERSION_LEGACY 1u

#pragma pack(push, 8)

// What the overlay sends to the host. Percent values, exactly the units of the x64 tab.
struct PwRemoteSettingsV1 {
    std::int32_t mode;        // pw::WarpMode: 0 Off, 1 Uniform, 2 Peripheral
    std::int32_t colorFilter; // pw::ColorFilter: 0 Bilinear, 1 Auto
    float centerX, centerY;
    float workX, workY;
    float globalScale;
    float offsetX, offsetY;
    float workShiftX, workShiftY;
    float brightness; // percent, -20..20
    float gamma;      // 0.7..1.4
    std::int32_t workShiftEnabled;
    std::int32_t showCenterOutline;
    std::int32_t showWorkOutline;
    std::int32_t temporalMode;     // 0 every frame, 1 interpolate (sync), 3 interpolate (async)
    std::int32_t temporalEvery;    // 1..8
    std::int32_t temporalMaxQueue; // 0..8
    // Version 2. The optical flow that computes the motion vectors inside the 64-bit host itself
    // (dlss5-feed-host64.cfg next to the host exe, keys mv_source / ofa_grid / ofa_perf). 0 means
    // "no opinion", which is what a version-1 writer leaves here and what the host ignores.
    std::int32_t ofaSource; // 0 unset, 1 ofa (driver optical flow), 2 shader (ReShade estimate)
    std::int32_t ofaGrid;   // 0 unset, else 1, 2 or 4
    std::int32_t ofaPerf;   // 0 unset, else 5 slow, 10 medium, 20 fast
    // Version 3. How many times the model runs over its own output, and whether those passes are
    // spread one per frame. Zero is "no opinion" - what an older writer leaves here - so the flag
    // carries its two states as 1 and 2 rather than 0 and 1.
    std::int32_t modelPasses;  // 0 unset, else 1..3
    std::int32_t spreadPasses; // 0 unset, 1 off, 2 on
};

// PwRemoteSettingsV1::ofaSource, and the same values in the tab's combo order minus the "auto"
// entry. Unset is what a version-1 writer leaves behind and means "do not touch the host's file".
enum PwRemoteOfaSource : std::int32_t {
    PwRemoteOfaUnset = 0,
    PwRemoteOfaOfa = 1,
    PwRemoteOfaShader = 2,
};

// A yes/no setting that also has to say "an older writer left this alone" (PwRemoteSettingsV1).
enum PwRemoteFlag : std::int32_t {
    PwRemoteFlagUnset = 0,
    PwRemoteFlagOff = 1,
    PwRemoteFlagOn = 2,
};

// Why the model is (not) warped, in the priority the x64 tab's banner uses.
enum PwRemoteHookState : std::int32_t {
    PwRemoteHook_None = 0,
    PwRemoteHook_ModuleNotLoaded = 1,  // nvngx_dlssnr.dll is not in the host process
    PwRemoteHook_NotHooked = 2,
    PwRemoteHook_WaitingForFeature = 3,
    PwRemoteHook_ModeOff = 4,
    PwRemoteHook_Active = 5,
    PwRemoteHook_PassThrough = 6,
};

// What the host reports back.
struct PwRemoteStatusV1 {
    std::int32_t active;
    std::int32_t hookState; // PwRemoteHookState
    std::uint32_t nativeW, nativeH;
    std::uint32_t modelW, modelH;
    std::uint32_t temporalMode; // the mode actually running in the interposer
    float modelMs;              // full-pass GPU time (0 when timing is off)
    std::uint64_t fullFrames, interpFrames;
    char reason[128];
    // Version 2. What the host's config file says right now; the host add-on mirrors the file
    // rather than the engine's live state, which only the host exe knows.
    std::int32_t ofaAvailable; // 1 when the add-on runs inside the Feed host and found the cfg
    std::int32_t ofaActive;    // 1 mv_source=ofa, 0 mv_source=shader, -1 unknown
    std::int32_t ofaGrid;      // 1, 2 or 4; 0 unknown
    std::int32_t ofaPerf;      // 5, 10 or 20; 0 unknown
    // Version 3. The passes the host is set to and the number it actually runs; they differ while
    // a second model is still being built, or when the host refused the extra pass (VRAM). The two
    // texts are the host's own, at the width the hook keeps them (pw_ngx::Status).
    std::int32_t modelPasses;
    std::int32_t modelPassesRunning;
    char modelPassReason[192];  // empty unless the host has something to say about the passes
    char temporalReason[160];   // why the temporal mode is not the one asked for, if it is not
};

struct PwRemoteBlockV1 {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t settingsGeneration; // written last by the UI side
    PwRemoteSettingsV1 settings;
    std::uint32_t statusGeneration;   // written last by the host side
    std::uint64_t hostHeartbeatTick;  // GetTickCount64() of the host's last present
    PwRemoteStatusV1 status;
    PwRemoteSettingsV1 applied;       // what the host currently runs
};

#pragma pack(pop)

static_assert(sizeof(PwRemoteSettingsV1) == 96, "PwRemoteSettingsV1 layout is part of the protocol");
static_assert(sizeof(PwRemoteStatusV1) == 552, "PwRemoteStatusV1 layout is part of the protocol");
static_assert(offsetof(PwRemoteBlockV1, settings) == 16, "block layout");
static_assert(offsetof(PwRemoteBlockV1, statusGeneration) == 112, "block layout");
static_assert(offsetof(PwRemoteBlockV1, hostHeartbeatTick) == 120, "block layout");
static_assert(offsetof(PwRemoteBlockV1, status) == 128, "block layout");
static_assert(offsetof(PwRemoteBlockV1, applied) == 680, "block layout");
static_assert(sizeof(PwRemoteBlockV1) == 776, "block layout");

#pragma pack(push, 8)

// The version-2 wire layout, kept verbatim for the same reason as the version-1 one below: a
// version-3 tab can still read the block of a host that has not been updated yet. The model-pass
// group is hidden on such a connection.
struct PwRemoteSettingsOfaV2 {
    std::int32_t mode;
    std::int32_t colorFilter;
    float centerX, centerY;
    float workX, workY;
    float globalScale;
    float offsetX, offsetY;
    float workShiftX, workShiftY;
    float brightness;
    float gamma;
    std::int32_t workShiftEnabled;
    std::int32_t showCenterOutline;
    std::int32_t showWorkOutline;
    std::int32_t temporalMode;
    std::int32_t temporalEvery;
    std::int32_t temporalMaxQueue;
    std::int32_t ofaSource;
    std::int32_t ofaGrid;
    std::int32_t ofaPerf;
};

struct PwRemoteStatusOfaV2 {
    std::int32_t active;
    std::int32_t hookState;
    std::uint32_t nativeW, nativeH;
    std::uint32_t modelW, modelH;
    std::uint32_t temporalMode;
    float modelMs;
    std::uint64_t fullFrames, interpFrames;
    char reason[128];
    std::int32_t ofaAvailable;
    std::int32_t ofaActive;
    std::int32_t ofaGrid;
    std::int32_t ofaPerf;
};

struct PwRemoteBlockOfaV2 {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t settingsGeneration;
    PwRemoteSettingsOfaV2 settings;
    std::uint32_t statusGeneration;
    std::uint64_t hostHeartbeatTick;
    PwRemoteStatusOfaV2 status;
    PwRemoteSettingsOfaV2 applied;
};

// The version-1 wire layout, kept verbatim so a later build can still read a block written by
// a version-1 peer (an older host, or an older 32-bit tab) instead of refusing to connect. The
// optical-flow group is simply hidden on such a connection.
struct PwRemoteSettingsLegacyV1 {
    std::int32_t mode;
    std::int32_t colorFilter;
    float centerX, centerY;
    float workX, workY;
    float globalScale;
    float offsetX, offsetY;
    float workShiftX, workShiftY;
    float brightness;
    float gamma;
    std::int32_t workShiftEnabled;
    std::int32_t showCenterOutline;
    std::int32_t showWorkOutline;
    std::int32_t temporalMode;
    std::int32_t temporalEvery;
    std::int32_t temporalMaxQueue;
};

struct PwRemoteStatusLegacyV1 {
    std::int32_t active;
    std::int32_t hookState;
    std::uint32_t nativeW, nativeH;
    std::uint32_t modelW, modelH;
    std::uint32_t temporalMode;
    float modelMs;
    std::uint64_t fullFrames, interpFrames;
    char reason[128];
};

struct PwRemoteBlockLegacyV1 {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t settingsGeneration;
    PwRemoteSettingsLegacyV1 settings;
    std::uint32_t statusGeneration;
    std::uint64_t hostHeartbeatTick;
    PwRemoteStatusLegacyV1 status;
    PwRemoteSettingsLegacyV1 applied;
};

#pragma pack(pop)

static_assert(sizeof(PwRemoteSettingsOfaV2) == 88, "the version-2 layout is frozen");
static_assert(sizeof(PwRemoteStatusOfaV2) == 192, "the version-2 layout is frozen");
static_assert(offsetof(PwRemoteBlockOfaV2, statusGeneration) == 104, "the version-2 layout is frozen");
static_assert(offsetof(PwRemoteBlockOfaV2, hostHeartbeatTick) == 112, "the version-2 layout is frozen");
static_assert(offsetof(PwRemoteBlockOfaV2, status) == 120, "the version-2 layout is frozen");
static_assert(offsetof(PwRemoteBlockOfaV2, applied) == 312, "the version-2 layout is frozen");
static_assert(sizeof(PwRemoteBlockOfaV2) == 400, "the version-2 layout is frozen");

static_assert(sizeof(PwRemoteSettingsLegacyV1) == 76, "the version-1 layout is frozen");
static_assert(sizeof(PwRemoteStatusLegacyV1) == 176, "the version-1 layout is frozen");
static_assert(offsetof(PwRemoteBlockLegacyV1, statusGeneration) == 92, "the version-1 layout is frozen");
static_assert(offsetof(PwRemoteBlockLegacyV1, hostHeartbeatTick) == 96, "the version-1 layout is frozen");
static_assert(offsetof(PwRemoteBlockLegacyV1, status) == 104, "the version-1 layout is frozen");
static_assert(offsetof(PwRemoteBlockLegacyV1, applied) == 280, "the version-1 layout is frozen");
static_assert(sizeof(PwRemoteBlockLegacyV1) == 360, "the version-1 layout is frozen");

// The host is considered alive while its last present is younger than this.
constexpr unsigned long long kPwRemoteHeartbeatTimeoutMs = 3000;
