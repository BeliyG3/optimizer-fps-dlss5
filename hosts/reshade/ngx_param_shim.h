#pragma once
// Menu mode: NGX parameter objects of the add-on's own.
//   TraceParams: forwards every Set/Get/Reset to a host block and records which keys, with which
//                overload, the NR runtime touched (once per key, overload and direction).
//   OwnParams:   a block the add-on owns. Numeric getters convert between the stored and the requested
//                type and pointer getters answer any pointer-typed key, like the bench's block
//                (tools/bench12/ngx_nr_params.cpp); a key the host did not answer keeps the host's code
//                (PutAbsent), any other unknown key answers NVSDK_NGX_Result_FAIL_UnsupportedParameter
//                (0xBAD00010), what every stand host answers (spike 0b-2a).
//   ResetParams: forwards to a host block but answers DLSSNR.Reset itself (menu mode's exit reset, C8).
// Both are called by the NR runtime through the raw vtable. MSVC lays overloads of one name out in
// reverse declaration order (the slots ngx_params.cpp uses on a host block: void* 0/8, int 3/11,
// float 6/14); tests/test_ngx_param_shim.cpp checks these classes have that layout.
#include "hosts/reshade/ngx_param_iface.h"
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ofps::reshade {

// The eight overloads, in the declaration order of ngx_param_iface.h.
enum class NgxType : unsigned char { ULL, Float, Double, UInt, Int, D3D11, D3D12, Pointer };
const char *NgxTypeName(NgxType type);

struct TracedKey {
    std::string name;
    NgxType type;
    bool set;       // the runtime wrote the key (else read it)
    int hostResult; // the host block's answer to the first read (NVSDK_NGX_Result_Success for a write)
};

class TraceParams final : public NVSDK_NGX_Parameter {
public:
    explicit TraceParams(NVSDK_NGX_Parameter *host = nullptr) : host_(host) {}
    void Bind(NVSDK_NGX_Parameter *host) { host_ = host; }
    NVSDK_NGX_Parameter *Host() const { return host_; }

    void Set(const char *name, unsigned long long value) override;
    void Set(const char *name, float value) override;
    void Set(const char *name, double value) override;
    void Set(const char *name, unsigned int value) override;
    void Set(const char *name, int value) override;
    void Set(const char *name, ID3D11Resource *value) override;
    void Set(const char *name, ID3D12Resource *value) override;
    void Set(const char *name, void *value) override;
    NVSDK_NGX_Result Get(const char *name, unsigned long long *value) const override;
    NVSDK_NGX_Result Get(const char *name, float *value) const override;
    NVSDK_NGX_Result Get(const char *name, double *value) const override;
    NVSDK_NGX_Result Get(const char *name, unsigned int *value) const override;
    NVSDK_NGX_Result Get(const char *name, int *value) const override;
    NVSDK_NGX_Result Get(const char *name, ID3D11Resource **value) const override;
    NVSDK_NGX_Result Get(const char *name, ID3D12Resource **value) const override;
    NVSDK_NGX_Result Get(const char *name, void **value) const override;
    void Reset() override;

    const std::vector<TracedKey> &Keys() const { return keys_; } // first-seen order
    bool ResetCalled() const { return resetCalled_; }
    void Clear() { keys_.clear(); resetCalled_ = false; }

private:
    void Record(const char *name, NgxType type, bool set, int result) const;
    template <class T> NVSDK_NGX_Result Forward(const char *name, T *value, NgxType type) const;
    template <class T> void ForwardSet(const char *name, T value, NgxType type);
    NVSDK_NGX_Parameter *host_;
    mutable std::vector<TracedKey> keys_;
    bool resetCalled_ = false;
};

using NgxValue = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource *, ID3D12Resource *, void *>;

class OwnParams final : public NVSDK_NGX_Parameter {
public:
    void Set(const char *name, unsigned long long value) override;
    void Set(const char *name, float value) override;
    void Set(const char *name, double value) override;
    void Set(const char *name, unsigned int value) override;
    void Set(const char *name, int value) override;
    void Set(const char *name, ID3D11Resource *value) override;
    void Set(const char *name, ID3D12Resource *value) override;
    void Set(const char *name, void *value) override;
    NVSDK_NGX_Result Get(const char *name, unsigned long long *value) const override;
    NVSDK_NGX_Result Get(const char *name, float *value) const override;
    NVSDK_NGX_Result Get(const char *name, double *value) const override;
    NVSDK_NGX_Result Get(const char *name, unsigned int *value) const override;
    NVSDK_NGX_Result Get(const char *name, int *value) const override;
    NVSDK_NGX_Result Get(const char *name, ID3D11Resource **value) const override;
    NVSDK_NGX_Result Get(const char *name, ID3D12Resource **value) const override;
    NVSDK_NGX_Result Get(const char *name, void **value) const override;
    void Reset() override { values_.clear(); absent_.clear(); }

    const std::unordered_map<std::string, NgxValue> &Values() const { return values_; }
    const std::unordered_map<std::string, NVSDK_NGX_Result> &Absent() const { return absent_; }
    bool Has(const char *name) const { return values_.count(name) != 0; }
    bool Knows(const char *name) const { return values_.count(name) != 0 || absent_.count(name) != 0; }
    void Put(const std::string &name, const NgxValue &value) { values_[name] = value; absent_.erase(name); }
    // A key the host did not answer: every overload answers `result` (the host's code), the value untouched.
    void PutAbsent(const std::string &name, NVSDK_NGX_Result result) { values_.erase(name); absent_[name] = result; }

private:
    template <class T> NVSDK_NGX_Result Number(const char *name, T *out) const;
    template <class T> NVSDK_NGX_Result Pointer(const char *name, T **out) const;
    NVSDK_NGX_Result Missing(const char *name) const;
    void Store(const char *name, const NgxValue &value) { if (name) Put(name, value); }
    std::unordered_map<std::string, NgxValue> values_;
    std::unordered_map<std::string, NVSDK_NGX_Result> absent_;
};

// Menu mode's exit reset (C8): forwards every call to a block except DLSSNR.Reset, which it answers itself (1 until
// the runtime writes another value, which it keeps). The block's own Reset key - present, absent, its result code - is
// never touched, so nothing is added to the host's block for the one evaluate it wraps.
class ResetParams final : public NVSDK_NGX_Parameter {
public:
    explicit ResetParams(NVSDK_NGX_Parameter *inner = nullptr);
    void Bind(NVSDK_NGX_Parameter *inner) { inner_ = inner; }

    void Set(const char *name, unsigned long long value) override;
    void Set(const char *name, float value) override;
    void Set(const char *name, double value) override;
    void Set(const char *name, unsigned int value) override;
    void Set(const char *name, int value) override;
    void Set(const char *name, ID3D11Resource *value) override;
    void Set(const char *name, ID3D12Resource *value) override;
    void Set(const char *name, void *value) override;
    NVSDK_NGX_Result Get(const char *name, unsigned long long *value) const override;
    NVSDK_NGX_Result Get(const char *name, float *value) const override;
    NVSDK_NGX_Result Get(const char *name, double *value) const override;
    NVSDK_NGX_Result Get(const char *name, unsigned int *value) const override;
    NVSDK_NGX_Result Get(const char *name, int *value) const override;
    NVSDK_NGX_Result Get(const char *name, ID3D11Resource **value) const override;
    NVSDK_NGX_Result Get(const char *name, ID3D12Resource **value) const override;
    NVSDK_NGX_Result Get(const char *name, void **value) const override;
    void Reset() override;

private:
    template <class T> NVSDK_NGX_Result Forward(const char *name, T *value) const;
    template <class T> void ForwardSet(const char *name, T value);
    NVSDK_NGX_Parameter *inner_;
    OwnParams reset_; // DLSSNR.Reset only
};

// Reads `name` from `from` with the overload `type`: the host's result code, *out set only on success.
NVSDK_NGX_Result ReadAs(NVSDK_NGX_Parameter *from, const char *name, NgxType type, NgxValue *out);
// Reads `name` from `from` with the overload `type`; false when `from` does not answer it.
bool GetAs(NVSDK_NGX_Parameter *from, const char *name, NgxType type, NgxValue *out);
bool IsPointerValue(const NgxValue &value);

// `to` is reset, then gets every key the trace saw the runtime read, read from `from` with the overload
// the runtime used (the first one when it used several). A key `from` does not answer is recorded absent with its code.
void CopyTracedKeys(const TraceParams &trace, NVSDK_NGX_Parameter *from, OwnParams &to);
// The same for a key list taken from a trace earlier (the menu book freezes one per generation).
void CopyKeys(const std::vector<TracedKey> &keys, NVSDK_NGX_Parameter *from, OwnParams &to);

} // namespace ofps::reshade
