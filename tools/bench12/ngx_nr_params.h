#pragma once
#include "ngx_sdk.h"
#include <mutex>
#include <string>
#include <unordered_map>

// Host-owned NGX parameter block for feature 18. The NR runtime is hosted without the NGX core
// (see ngx_nr_runtime.h), so the bench supplies the block that the runtime and any hook read and
// write. Numeric getters convert between the stored and the requested type, like the core's block;
// pointer keys answer only the pointer getters. Missing keys return FAIL_UnsupportedParameter.
class NgxParameterMap final : public NVSDK_NGX_Parameter {
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
    void Reset() override;
    // While set, every write is printed as "[nr parameter] name = value".
    void AuditFinal(const char *path, const char *operation) const;
    void Audit(bool enabled) { audit=enabled; }
private:
    enum class Kind { Unsigned, Signed, Real, Pointer };
    struct Value { Kind kind=Kind::Unsigned; unsigned long long bits=0; double real=0; void *pointer=nullptr; const char *type="unknown"; };
    void Put(const char *name, const Value &value);
    template<class T> NVSDK_NGX_Result Number(const char *name, T *out) const;
    template<class T> NVSDK_NGX_Result Pointer(const char *name, T **out) const;
    mutable std::mutex mutex;
    std::unordered_map<std::string,Value> values;
    bool audit=false;
};
