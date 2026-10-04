#include "hosts/reshade/ngx_param_shim.h"
#include <cstring>
#include <type_traits>

namespace ofps::reshade {

const char *NgxTypeName(NgxType type) {
    static const char *const names[] = {"ULL", "float", "double", "uint", "int", "D3D11", "D3D12", "void*"};
    const unsigned index = static_cast<unsigned>(type);
    return index < 8 ? names[index] : "?";
}

// --- TraceParams -------------------------------------------------------------------------------------

void TraceParams::Record(const char *name, NgxType type, bool set, int result) const {
    if (!name) return;
    for (const TracedKey &k : keys_)
        if (k.type == type && k.set == set && k.name == name) return;
    keys_.push_back({name, type, set, result});
}

template <class T> NVSDK_NGX_Result TraceParams::Forward(const char *name, T *value, NgxType type) const {
    const NVSDK_NGX_Result result = host_ ? host_->Get(name, value) : NVSDK_NGX_Result_Fail;
    Record(name, type, false, result);
    return result;
}

template <class T> void TraceParams::ForwardSet(const char *name, T value, NgxType type) {
    if (host_) host_->Set(name, value);
    Record(name, type, true, NVSDK_NGX_Result_Success);
}

void TraceParams::Set(const char *n, unsigned long long v) { ForwardSet(n, v, NgxType::ULL); }
void TraceParams::Set(const char *n, float v) { ForwardSet(n, v, NgxType::Float); }
void TraceParams::Set(const char *n, double v) { ForwardSet(n, v, NgxType::Double); }
void TraceParams::Set(const char *n, unsigned int v) { ForwardSet(n, v, NgxType::UInt); }
void TraceParams::Set(const char *n, int v) { ForwardSet(n, v, NgxType::Int); }
void TraceParams::Set(const char *n, ID3D11Resource *v) { ForwardSet(n, v, NgxType::D3D11); }
void TraceParams::Set(const char *n, ID3D12Resource *v) { ForwardSet(n, v, NgxType::D3D12); }
void TraceParams::Set(const char *n, void *v) { ForwardSet(n, v, NgxType::Pointer); }
NVSDK_NGX_Result TraceParams::Get(const char *n, unsigned long long *v) const { return Forward(n, v, NgxType::ULL); }
NVSDK_NGX_Result TraceParams::Get(const char *n, float *v) const { return Forward(n, v, NgxType::Float); }
NVSDK_NGX_Result TraceParams::Get(const char *n, double *v) const { return Forward(n, v, NgxType::Double); }
NVSDK_NGX_Result TraceParams::Get(const char *n, unsigned int *v) const { return Forward(n, v, NgxType::UInt); }
NVSDK_NGX_Result TraceParams::Get(const char *n, int *v) const { return Forward(n, v, NgxType::Int); }
NVSDK_NGX_Result TraceParams::Get(const char *n, ID3D11Resource **v) const { return Forward(n, v, NgxType::D3D11); }
NVSDK_NGX_Result TraceParams::Get(const char *n, ID3D12Resource **v) const { return Forward(n, v, NgxType::D3D12); }
NVSDK_NGX_Result TraceParams::Get(const char *n, void **v) const { return Forward(n, v, NgxType::Pointer); }
void TraceParams::Reset() {
    resetCalled_ = true;
    if (host_) host_->Reset();
}

// --- OwnParams ---------------------------------------------------------------------------------------

namespace {
bool IsPointer(const NgxValue &v) { return v.index() >= 5; }
void *AsPointer(const NgxValue &v) {
    return std::visit([](auto x) -> void * {
        if constexpr (std::is_pointer_v<decltype(x)>) return static_cast<void *>(x);
        else return nullptr;
    }, v);
}
} // namespace

void OwnParams::Set(const char *n, unsigned long long v) { Store(n, v); }
void OwnParams::Set(const char *n, float v) { Store(n, v); }
void OwnParams::Set(const char *n, double v) { Store(n, v); }
void OwnParams::Set(const char *n, unsigned int v) { Store(n, v); }
void OwnParams::Set(const char *n, int v) { Store(n, v); }
void OwnParams::Set(const char *n, ID3D11Resource *v) { Store(n, v); }
void OwnParams::Set(const char *n, ID3D12Resource *v) { Store(n, v); }
void OwnParams::Set(const char *n, void *v) { Store(n, v); }

NVSDK_NGX_Result OwnParams::Missing(const char *name) const {
    const auto absent = absent_.find(name);
    return absent != absent_.end() ? absent->second : NVSDK_NGX_Result_FAIL_UnsupportedParameter;
}

// As the bench's block: a real converts from its value, a signed integer sign-extends, an unsigned one
// converts from its bits; a pointer-typed key does not answer a numeric getter.
template <class T> NVSDK_NGX_Result OwnParams::Number(const char *name, T *out) const {
    if (!name || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto found = values_.find(name);
    if (found == values_.end()) return Missing(name);
    if (IsPointer(found->second)) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    std::visit([out](auto x) {
        using V = decltype(x);
        if constexpr (std::is_same_v<V, float> || std::is_same_v<V, double>) *out = static_cast<T>(x);
        else if constexpr (std::is_same_v<V, int>) *out = static_cast<T>(static_cast<long long>(x));
        else if constexpr (std::is_arithmetic_v<V>) *out = static_cast<T>(x);
    }, found->second);
    return NVSDK_NGX_Result_Success;
}

template <class T> NVSDK_NGX_Result OwnParams::Pointer(const char *name, T **out) const {
    if (!name || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto found = values_.find(name);
    if (found == values_.end()) return Missing(name);
    if (!IsPointer(found->second)) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    *out = static_cast<T *>(AsPointer(found->second));
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result OwnParams::Get(const char *n, unsigned long long *v) const { return Number(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, float *v) const { return Number(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, double *v) const { return Number(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, unsigned int *v) const { return Number(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, int *v) const { return Number(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, ID3D11Resource **v) const { return Pointer(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, ID3D12Resource **v) const { return Pointer(n, v); }
NVSDK_NGX_Result OwnParams::Get(const char *n, void **v) const { return Pointer(n, v); }

// --- ResetParams -------------------------------------------------------------------------------------

namespace {
bool IsResetKey(const char *name) { return name && std::strcmp(name, "DLSSNR.Reset") == 0; }
} // namespace

ResetParams::ResetParams(NVSDK_NGX_Parameter *inner) : inner_(inner) { reset_.Put("DLSSNR.Reset", 1u); }

template <class T> NVSDK_NGX_Result ResetParams::Forward(const char *name, T *value) const {
    if (IsResetKey(name)) return reset_.Get(name, value);
    return inner_ ? inner_->Get(name, value) : NVSDK_NGX_Result_Fail;
}

template <class T> void ResetParams::ForwardSet(const char *name, T value) {
    if (IsResetKey(name)) reset_.Set(name, value);
    else if (inner_) inner_->Set(name, value);
}

void ResetParams::Set(const char *n, unsigned long long v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, float v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, double v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, unsigned int v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, int v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, ID3D11Resource *v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, ID3D12Resource *v) { ForwardSet(n, v); }
void ResetParams::Set(const char *n, void *v) { ForwardSet(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, unsigned long long *v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, float *v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, double *v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, unsigned int *v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, int *v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, ID3D11Resource **v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, ID3D12Resource **v) const { return Forward(n, v); }
NVSDK_NGX_Result ResetParams::Get(const char *n, void **v) const { return Forward(n, v); }
void ResetParams::Reset() {
    if (inner_) inner_->Reset();
}

// --- CopyTracedKeys ----------------------------------------------------------------------------------

namespace {
template <class T> NVSDK_NGX_Result Read(NVSDK_NGX_Parameter *from, const char *name, NgxValue *out) {
    T value{};
    const NVSDK_NGX_Result result = from->Get(name, &value);
    if (result == NVSDK_NGX_Result_Success) *out = value;
    return result;
}
} // namespace

bool IsPointerValue(const NgxValue &value) { return IsPointer(value); }

NVSDK_NGX_Result ReadAs(NVSDK_NGX_Parameter *from, const char *name, NgxType type, NgxValue *out) {
    if (!from || !name || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    switch (type) {
    case NgxType::ULL: return Read<unsigned long long>(from, name, out);
    case NgxType::Float: return Read<float>(from, name, out);
    case NgxType::Double: return Read<double>(from, name, out);
    case NgxType::UInt: return Read<unsigned int>(from, name, out);
    case NgxType::Int: return Read<int>(from, name, out);
    case NgxType::D3D11: return Read<ID3D11Resource *>(from, name, out);
    case NgxType::D3D12: return Read<ID3D12Resource *>(from, name, out);
    case NgxType::Pointer: return Read<void *>(from, name, out);
    }
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
}

bool GetAs(NVSDK_NGX_Parameter *from, const char *name, NgxType type, NgxValue *out) {
    return ReadAs(from, name, type, out) == NVSDK_NGX_Result_Success;
}

void CopyTracedKeys(const TraceParams &trace, NVSDK_NGX_Parameter *from, OwnParams &to) {
    CopyKeys(trace.Keys(), from, to);
}

void CopyKeys(const std::vector<TracedKey> &keys, NVSDK_NGX_Parameter *from, OwnParams &to) {
    to.Reset();
    if (!from) return;
    for (const TracedKey &k : keys) {
        if (k.set || to.Knows(k.name.c_str())) continue; // the first overload the runtime read with
        NgxValue value;
        const NVSDK_NGX_Result result = ReadAs(from, k.name.c_str(), k.type, &value);
        if (result == NVSDK_NGX_Result_Success) to.Put(k.name, value);
        else to.PutAbsent(k.name, result);
    }
}

} // namespace ofps::reshade
