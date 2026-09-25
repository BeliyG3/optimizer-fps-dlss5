#pragma once

// Optimizer FPS for DLSS5 - the core's ABI (core/api/ofps_core.h).
//
// The core (optimizer-fps-dlss5-core.dll, or the same sources linked statically) compresses the
// periphery of a frame before an NVIDIA DLSS Neural Rendering model, runs the model every N-th frame
// with reprojection in between, and never touches the model itself: a HOST hands it the frame
// (OfpsFrameInputs) and a MODEL HOST (IOfpsModelHost) creates, runs and releases the model. The
// ReShade shell implements the model host over the NGX parameter block and nvngx_dlssnr.dll; the
// OptiScaler shim implements it over its own NR objects.
//
// ABI rules (design spec section 4.1):
//  - only pure virtual interfaces (method order is fixed, new methods go last) and POD structs whose
//    first field is `size`; the caller fills `size` of the struct version it knows, the core reads
//    only the fields covered by it;
//  - strings returned by the core point into core memory and live until the next call of the same
//    method; arrays for the host are `count + pointer` in the caller's memory;
//  - nothing is allocated or freed across the boundary; no C++ exception crosses it; the core wraps
//    every host callback in a structured-exception guard;
//  - callbacks (IOfpsModelHost::*, IOfpsHost::OnEvent) run under the core's lock: a callback must not
//    call back into IOfpsCore/IOfpsFeature (Log is fine); everything a callback needs is in its
//    arguments;
//  - OFPS_ABI_VERSION grows with any change of a struct layout or a method order.

#include <d3d12.h>
#include <dxgiformat.h>

#include <stdint.h>

#define OFPS_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// ---- result codes ------------------------------------------------------------------------------
enum OfpsResult {
    OFPS_OK = 0,
    OFPS_S_EXISTING = 1,          // OfpsCreateCore: the core already existed; this host was added
    OFPS_S_FOREIGN = 2,           // CreateFeature/AdoptFeature: the core declines this feature (one warped
                                  // feature per process); the host forwards its calls untouched and later
                                  // calls NotifyForeignReleased
    OFPS_S_MODEL_NEXT_FRAME = 3,  // CreateModel: created, but must not be evaluated on this command list
    OFPS_S_IDENTITY = 4,          // PrepareModelInput/ResolveAnswer: the host has no codec of its own
    OFPS_E_ABI = -1,
    OFPS_E_ARG = -2,
    OFPS_E_DEVICE = -3,
    OFPS_E_STATE = -4,
};

enum OfpsLogLevel { OFPS_LOG_INFO = 0, OFPS_LOG_WARN = 1, OFPS_LOG_ERROR = 2 };

enum OfpsEvent {
    OFPS_EVENT_FIRST_WARPED_FRAME = 0,  // before the first warped evaluate is recorded (crash guard)
    OFPS_EVENT_SETTINGS_CHANGED = 1,    // payload = const OfpsSettingsValues* (effective values)
    OFPS_EVENT_FEATURE_CREATED = 2,     // handle = the host's handle
    OFPS_EVENT_FEATURE_RELEASED = 3,    // handle = the host's handle; the IOfpsModelHost may go now
    OFPS_EVENT_HOST_SHAPE_REJECTED = 4, // text = the reason; the host logs its own description
    OFPS_EVENT_DEVICE_REMOVED = 5,
    OFPS_EVENT_DIRECT_HOST_CHANGED = 6, // payload = const uint32_t* (1 = a direct host is registered)
};

enum OfpsColorDomain { OFPS_COLOR_DISPLAY_REFERRED = 0, OFPS_COLOR_LINEAR_HDR = 1 };

enum OfpsPath {           // OfpsEvalResult::path
    OFPS_PATH_PASSTHROUGH = 0,
    OFPS_PATH_WARPED = 1,
    OFPS_PATH_CARRIED = 2,
    OFPS_PATH_FALLBACK = 3,
    OFPS_PATH_CREATION_FRAME = 4,
};

enum OfpsWarpPath { OFPS_WARP_NONE = 0, OFPS_WARP_COMPUTE = 1, OFPS_WARP_PIXEL = 2 };

enum OfpsHostCap {
    OFPS_CAP_PRE_UPSCALE = 1u << 0,        // the host can call the core before its upscaler
    OFPS_CAP_MODEL_RESOLUTION = 1u << 1,   // the host shrinks the model input itself (GlobalScale hidden, 100)
    OFPS_CAP_DEFERRED_MODEL_CREATION = 1u << 2,
    OFPS_CAP_BACKGROUND_MODEL = 1u << 3,   // RunModel is allowed on the core's private list
    OFPS_CAP_OPTICAL_FLOW = 1u << 4,
    OFPS_CAP_QUEUES = 1u << 5,             // the host registers queues and reports submissions
};

// ---- POD ---------------------------------------------------------------------------------------
typedef struct OfpsVersion {
    uint32_t size;
    uint32_t abi;
    char release[16];      // "2026.9.1"
} OfpsVersion;

typedef struct OfpsRect { uint32_t x, y, w, h; } OfpsRect;
typedef struct OfpsRectF { float x, y, w, h; } OfpsRectF;

typedef struct OfpsResource {
    uint32_t size;
    ID3D12Resource *res;             // null = absent
    DXGI_FORMAT view;                // typed view format (DXGI_FORMAT_UNKNOWN = derive from the resource)
    OfpsRect rect;                   // the region the host means, in the texture's pixels
    D3D12_RESOURCE_STATES restState; // the state the resource rests in between the core's uses
    uint32_t subresource;            // barrier subresource (planar depth: the depth plane)
} OfpsResource;

typedef struct OfpsFrameInputs {
    uint32_t size;
    OfpsResource color, depth, motion, output;
    OfpsResource ui, uiAlpha, backbuffer;
    OfpsResource codecInputs[4];     // host codec inputs (e.g. exposure) the core snapshots for background frames
    float mvScaleX, mvScaleY;        // NGX semantics: texel value * scale = pixels of the motion texture's grid
    uint32_t depthInverted;
    uint32_t hostReset;              // the host asked for a history reset this frame
    uint32_t colorDomain;            // OfpsColorDomain
} OfpsFrameInputs;

typedef struct OfpsModelInputs {
    uint32_t size;
    OfpsResource color, depth, motion, output;
    OfpsResource ui, uiAlpha, backbuffer;   // res == null when withholdUi
    float mvScaleX, mvScaleY;        // NGX semantics on the grid the model reads (packed: the packed grid)
    uint32_t depthInverted;
    uint32_t reset;
    uint32_t withholdUi;             // UI inputs absent: UI correction must be off in the model
    uint32_t width, height;          // the model's extent (DLSSNR.Width/Height)
} OfpsModelInputs;

typedef struct OfpsFeatureDesc {
    uint32_t size;
    uint32_t width, height;          // the frame grid (DLSSNR.Width/Height the host created with)
    ID3D12Device *resourceDevice;    // the device the host's resources answer with (fences, queues)
    LUID adapterLuid;
} OfpsFeatureDesc;

typedef struct OfpsEvalResult {
    uint32_t size;
    int modelResult;                 // the last RunModel return (0 when the model did not run)
    uint32_t path;                   // OfpsPath
    uint32_t warpPath;               // OfpsWarpPath
} OfpsEvalResult;

typedef struct OfpsFencePoint {
    uint32_t size;
    ID3D12Fence *fence;
    uint64_t value;
} OfpsFencePoint;

typedef struct OfpsEventData {
    uint32_t size;
    const void *payload;
    void *handle;
    const char *text;
} OfpsEventData;

typedef struct OfpsHostCaps {
    uint32_t size;
    uint32_t flags;                  // OfpsHostCap bits
} OfpsHostCaps;

typedef struct OfpsStatus {
    uint32_t size;
    uint32_t active, featureCreated, adopted, directHost, deviceRemoved;
    uint32_t nativeW, nativeH, workW, workH;
    uint32_t temporalMode, warpPath;
    uint64_t evaluations, passthroughs, fallbackFrames, fullFrames, interpFrames;
    float modelMs; uint32_t modelSamples;
    uint32_t modelPassesRunning;
    int lastModelResult;
    const char *reason;              // why the last evaluate did not warp ("" when it did)
    const char *temporalReason;
    const char *modelPassReason;
    const char *fallbackReason;
} OfpsStatus;

typedef struct OfpsStatusRow {
    uint32_t group;                  // OfpsSettingGroup of the schema
    const char *label;
    const char *value;
    uint32_t severity;               // 0 info, 1 warn, 2 error
} OfpsStatusRow;

typedef struct OfpsLayoutPreview {
    uint32_t size;
    OfpsRectF frame, center, rawWork, zoneLimits;   // normalised 0..1 of the frame
    float offsetMaxX, offsetMaxY;
    float shiftMin[2], shiftMax[2];
    uint32_t nativeW, nativeH, rawWorkW, rawWorkH, modelW, modelH;
    float pixelPercent, maxSourceFootprintX, maxSourceFootprintY;
    uint32_t diagnosticFlags;        // layout flags (aggressive ...)
} OfpsLayoutPreview;

// Settings values by OfpsSettingId (ofps_settings_schema.h). `explicitMask` bit i = value i was set by
// the host (as opposed to "absent from the ini"): profile defaults apply only to unset values.
#define OFPS_SETTINGS_MAX 128u
typedef union OfpsSettingValue { int32_t i; float f; } OfpsSettingValue;
typedef struct OfpsSettingsValues {
    uint32_t size;
    uint32_t count;                  // valid entries in v[]
    OfpsSettingValue v[OFPS_SETTINGS_MAX];
    uint64_t explicitMask[2];
} OfpsSettingsValues;

#ifdef __cplusplus
} // extern "C"

// ---- interfaces --------------------------------------------------------------------------------
struct IOfpsHost {
    virtual void Log(OfpsLogLevel level, const char *text) = 0;
    virtual void OnEvent(OfpsEvent kind, const OfpsEventData *data) = 0;
};

struct IOfpsModelHost {
    virtual int  CreateModel(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, uint32_t withholdUi, void **handle) = 0;
    virtual int  ReleaseModel(void *handle) = 0;
    virtual int  RunModel(ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs *inputs) = 0;
    // Host codec (spec 4.5). OFPS_S_IDENTITY: the frame colour is the model input, frameBefore stays null.
    virtual int  PrepareModelInput(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame, OfpsResource *modelColor, OfpsResource *frameBefore) = 0;
    virtual int  ResolveAnswer(ID3D12GraphicsCommandList *cmd, const OfpsResource *answer, const OfpsFrameInputs *frame) = 0;
    virtual uint32_t DescribeInputs(char *out, uint32_t size) = 0;
    virtual uint32_t ModelReady(void *handle) = 0;   // for OFPS_S_MODEL_NEXT_FRAME models
    virtual void EndFrame(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame, const OfpsEvalResult *result) = 0;
};

struct IOfpsFeature {
    virtual int   Evaluate(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame, OfpsEvalResult *result) = 0;
    virtual void *CurrentModelHandle() = 0;
    virtual void  RequestModelRebuild() = 0;
    virtual void  Release() = 0;
};

struct IOfpsCore {
    virtual int  CreateFeature(ID3D12GraphicsCommandList *cmd, const OfpsFeatureDesc *desc, IOfpsModelHost *modelHost, IOfpsFeature **feature) = 0;
    virtual int  AdoptFeature(ID3D12GraphicsCommandList *cmd, const OfpsFeatureDesc *desc, void *existingHandle, IOfpsModelHost *modelHost, IOfpsFeature **feature) = 0;
    virtual void NotifyForeignReleased(void *handle) = 0;
    virtual int  SetSettings(const OfpsSettingsValues *values) = 0;
    virtual void GetSettings(OfpsSettingsValues *values) = 0;
    virtual void GetSettingRange(uint32_t settingId, float *lo, float *hi) = 0;
    virtual void Status(OfpsStatus *status) = 0;
    virtual uint32_t StatusLines(OfpsStatusRow *rows, uint32_t capacity) = 0;
    virtual void LayoutPreview(OfpsLayoutPreview *preview) = 0;
    virtual void RegisterQueue(ID3D12Device *resourceDevice, ID3D12CommandQueue *queue) = 0;
    virtual void UnregisterQueue(ID3D12CommandQueue *queue) = 0;
    virtual void OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list) = 0;
    virtual void RetireResource(IUnknown *object, const OfpsFencePoint *extra) = 0;
    virtual void SetDirectHost(IOfpsHost *host, uint32_t on) = 0;
    virtual void SetHostCaps(IOfpsHost *host, const OfpsHostCaps *caps) = 0;
    virtual void SetHostMotionGrid(uint32_t blockSize) = 0;
    virtual void Housekeeping() = 0;
    virtual void UnregisterHost(IOfpsHost *host) = 0;
    virtual void Release() = 0;
};

extern "C" {
uint32_t OfpsCoreVersion(OfpsVersion *out);
int OfpsCreateCore(uint32_t abiVersion, IOfpsHost *host, IOfpsCore **core);
}
#endif // __cplusplus
