#pragma once

// Optimizer FPS for DLSS5 - the settings schema (core/api/ofps_settings_schema.h).
//
// One table drives the ini of every host, the two ImGui menus and the remote tab. Ids are append-only
// (they are part of the ABI). In plan 1 the table carries exactly the keys the add-on persists or reads
// today; plan 4 adds the UI generator (ofps_ui.inl) and the hosts' generic ini load/save.

#include "ofps_core.h"

enum OfpsSettingGroup {
    OFPS_GROUP_MODE = 0, OFPS_GROUP_ZONE_SIZE, OFPS_GROUP_ZONE_POSITION, OFPS_GROUP_OUTLINES,
    OFPS_GROUP_OUTPUT_COLOUR, OFPS_GROUP_TEMPORAL, OFPS_GROUP_MODEL_PASSES, OFPS_GROUP_MOTION_SOURCE,
    OFPS_GROUP_DIAGNOSTICS, OFPS_GROUP_COUNT
};

enum OfpsSettingType { OFPS_TYPE_BOOL = 0, OFPS_TYPE_INT = 1, OFPS_TYPE_FLOAT = 2, OFPS_TYPE_ENUM = 3 };

enum OfpsSettingFlag {
    OFPS_FLAG_PERSISTED = 1u << 0,     // read from and written to the ini
    OFPS_FLAG_DIAGNOSTIC = 1u << 1,    // read from the ini, never written; shown read-only in Diagnostics
    OFPS_FLAG_REBUILDS_MODEL = 1u << 2,
    OFPS_FLAG_ADVANCED = 1u << 3,
    OFPS_FLAG_COLLAPSED_GROUP = 1u << 4,
};

enum OfpsVisibleOp { OFPS_VIS_ALWAYS = 0, OFPS_VIS_EQ = 1, OFPS_VIS_NE = 2, OFPS_VIS_GT = 3 };

// Append-only. The order below is the order the plan-1 ini adapter reads today's keys in.
enum OfpsSettingId {
    OFPS_SET_MODE = 0,               // 0 Off, 1 Uniform, 2 Peripheral
    OFPS_SET_COLOR_FILTER,           // 0 bilinear, 1 adaptive four-tap
    OFPS_SET_CENTER_X, OFPS_SET_CENTER_Y, OFPS_SET_WORK_X, OFPS_SET_WORK_Y, OFPS_SET_GLOBAL_SCALE,
    OFPS_SET_FLAGS,
    OFPS_SET_OFFSET_X, OFPS_SET_OFFSET_Y, OFPS_SET_WORK_SHIFT_X, OFPS_SET_WORK_SHIFT_Y, OFPS_SET_WORK_SHIFT_ENABLED,
    OFPS_SET_SHOW_CENTER_OUTLINE, OFPS_SET_SHOW_WORK_OUTLINE,
    OFPS_SET_BRIGHTNESS, OFPS_SET_GAMMA,
    OFPS_SET_TEMPORAL_MODE, OFPS_SET_TEMPORAL_EVERY, OFPS_SET_TEMPORAL_MAX_QUEUE,
    OFPS_SET_MODEL_PASSES, OFPS_SET_SPREAD_PASSES,
    // Diagnostics (today's Debug* keys, read-only)
    OFPS_SET_DEBUG_TIMING, OFPS_SET_DEBUG_TEMPORAL_READBACK, OFPS_SET_DEBUG_ASYNC_LOG, OFPS_SET_DEBUG_ASYNC_SHOW_PASS,
    OFPS_SET_DEBUG_ASYNC_COMPUTE, OFPS_SET_DEBUG_ASYNC_NORMAL_PRIORITY, OFPS_SET_DEBUG_ASYNC_NO_REALTIME,
    OFPS_SET_DEBUG_HOOK_DELAY_MS, OFPS_SET_DEBUG_KEEP_BACKBUFFER, OFPS_SET_DEBUG_DEPTH_STATE,
    OFPS_SET_DEBUG_TEMPORAL_VIS, OFPS_SET_DEBUG_TEMPORAL_KEEP_OUTPUT, OFPS_SET_DEBUG_TEMPORAL_BLEND,
    OFPS_SET_DEBUG_TEMPORAL_DEPTH, OFPS_SET_DEBUG_TEMPORAL_SMOOTH, OFPS_SET_DEBUG_MOTION_SMOOTH,
    OFPS_SET_DEBUG_PASS_NO_HISTORY, OFPS_SET_DEBUG_TEMPORAL_NO_MODEL_MOTION, OFPS_SET_DEBUG_TEMPORAL_NO_EXPECT,
    OFPS_SET_DEBUG_TEMPORAL_NO_CELLS, OFPS_SET_DEBUG_TEMPORAL_PHASE_IN, OFPS_SET_DEBUG_LAYER,
    OFPS_SET_DEBUG_WARP_PATH,        // 0 auto, 1 compute, 2 pixel (plan 5)
    OFPS_SET_COUNT
};

typedef struct OfpsVisibleIf { uint32_t settingId; uint32_t op; int32_t value; } OfpsVisibleIf;

typedef struct OfpsSettingDesc {
    uint32_t id;
    const char *iniKey;
    uint32_t group;
    const char *label;
    const char *help;
    uint32_t type;
    float minValue, maxValue;
    OfpsSettingValue defaultValue;
    uint32_t flags;
    OfpsVisibleIf visibleIf;
    uint32_t hostCap;                // OfpsHostCap required to show/accept, 0 = none
    uint32_t rangeFrom;              // 1 = the range comes from IOfpsCore::GetSettingRange
    const char *const *enumLabels;   // null-terminated, for OFPS_TYPE_ENUM
    uint32_t customWidget;           // 0 = generic widget
} OfpsSettingDesc;

// The table lives in core/settings/schema.cpp; hosts get it through the core (plan 4 adds
// IOfpsCore::Schema()). Plan 1 exposes it to the shell as a header-only constexpr array so the ini
// adapter and the tests can iterate it without the core object.
extern const OfpsSettingDesc kOfpsSettings[OFPS_SET_COUNT];
