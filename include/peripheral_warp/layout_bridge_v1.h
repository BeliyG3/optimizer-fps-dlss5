#pragma once

// PeripheralWarp layout bridge, ABI v1.
//
// A consumer that applies the warp itself (for example the OptiScaler DLSS-NR integration) can
// export a function named "PeripheralWarpLayoutBridgeV1" returning this table. The ReShade add-on
// looks it up with GetProcAddress across the modules loaded in the game process; when it is found,
// the add-on's overlay edits are forwarded with Set() and the consumer's own edits are pulled with
// Get() whenever the generation changes, so both overlays show and persist the same layout.
//
// Plain C: no C++ types cross the boundary, and the consumer owns the values.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PeripheralWarpLayoutStateV1 {
    uint32_t structSize;      /* sizeof(PeripheralWarpLayoutStateV1) */
    uint32_t mode;            /* 0 off, 1 uniform, 2 peripheral (pw::WarpMode) */
    uint32_t filter;          /* 0 bilinear, 1 adaptive four-tap (pw::ColorFilter) */
    float centerX;            /* percent, 1..99 */
    float workX;              /* percent, 25..100 */
    float centerY;
    float workY;
    float globalScalePercent; /* percent, 25..100 */
    uint32_t generation;      /* changes whenever the layout changes, from either side */
} PeripheralWarpLayoutStateV1;

enum PeripheralWarpLayoutBridgeStatusV1 {
    PeripheralWarpLayoutBridge_Ok = 0,
    PeripheralWarpLayoutBridge_InvalidArgument = 1,
    PeripheralWarpLayoutBridge_Rejected = 2, /* the consumer's validation rules refused the layout */
    PeripheralWarpLayoutBridge_NotReady = 3
};

typedef struct PeripheralWarpLayoutBridgeV1 {
    uint32_t structSize;
    uint32_t version; /* 1 */
    uint32_t (*Get)(PeripheralWarpLayoutStateV1 *outState);
    /* outGeneration receives the generation the accepted request was tagged with. */
    uint32_t (*Set)(const PeripheralWarpLayoutStateV1 *state, uint32_t *outGeneration);
} PeripheralWarpLayoutBridgeV1;

typedef const PeripheralWarpLayoutBridgeV1 *(*PeripheralWarpLayoutBridgeV1Fn)(void);

#define PW_LAYOUT_BRIDGE_V1_EXPORT_NAME "PeripheralWarpLayoutBridgeV1"

#ifdef __cplusplus
}
#endif
