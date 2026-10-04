#pragma once
// The NR runtime's parameter-block interface, declared by the add-on so it builds without NVIDIA's SDK headers
// (nothing from NVIDIA is distributed). The runtime calls a block through its vtable only, so what must match is the
// layout: eight Set overloads, eight Get overloads and Reset, pure virtual, in this declaration order, no destructor
// and no data. tests/test_ngx_param_shim.cpp checks the slots the add-on relies on. The names follow the runtime's
// ABI so code that talks to a host block reads the same as the runtime's documentation.
struct ID3D11Resource;
struct ID3D12Resource;

enum NVSDK_NGX_Result : unsigned int {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000,
    NVSDK_NGX_Result_FAIL_InvalidParameter = NVSDK_NGX_Result_Fail | 5,
    NVSDK_NGX_Result_FAIL_UnsupportedParameter = NVSDK_NGX_Result_Fail | 16,
};

struct NVSDK_NGX_Parameter {
    virtual void Set(const char *name, unsigned long long value) = 0;
    virtual void Set(const char *name, float value) = 0;
    virtual void Set(const char *name, double value) = 0;
    virtual void Set(const char *name, unsigned int value) = 0;
    virtual void Set(const char *name, int value) = 0;
    virtual void Set(const char *name, ID3D11Resource *value) = 0;
    virtual void Set(const char *name, ID3D12Resource *value) = 0;
    virtual void Set(const char *name, void *value) = 0;

    virtual NVSDK_NGX_Result Get(const char *name, unsigned long long *value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, float *value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, double *value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, unsigned int *value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, int *value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, ID3D11Resource **value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, ID3D12Resource **value) const = 0;
    virtual NVSDK_NGX_Result Get(const char *name, void **value) const = 0;

    virtual void Reset() = 0;
};
