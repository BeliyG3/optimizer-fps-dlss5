#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hosts/reshade/ngx_param_shim.h"
#include "hosts/reshade/ngx_params.h"
#include <cstdio>
#include <cstring>

// The shim as the NR runtime calls it: through raw vtable slots, not through the C++ overloads.
using namespace ofps::reshade;
namespace {
int failures = 0;
void Check(bool condition, const char *what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// MSVC order (reverse declaration order within the Set and the Get group): Set void* 0, D3D12 1,
// D3D11 2, int 3, uint 4, double 5, float 6, ULL 7; Get the same + 8; Reset 16.
constexpr int kSetSlot[8] = {7, 6, 5, 4, 3, 2, 1, 0}; // by NgxType (declaration order)
constexpr int kGetOffset = 8, kResetSlot = 16;
void **Slots(NVSDK_NGX_Parameter *p) { return *reinterpret_cast<void ***>(p); }
template <class T> void RawSet(NVSDK_NGX_Parameter *p, NgxType type, const char *name, T value) {
    reinterpret_cast<void (*)(void *, const char *, T)>(Slots(p)[kSetSlot[int(type)]])(p, name, value);
}
template <class T> int RawGet(NVSDK_NGX_Parameter *p, NgxType type, const char *name, T *value) {
    return reinterpret_cast<int (*)(void *, const char *, T *)>(Slots(p)[kSetSlot[int(type)] + kGetOffset])(p, name, value);
}
void RawReset(NVSDK_NGX_Parameter *p) { reinterpret_cast<void (*)(void *)>(Slots(p)[kResetSlot])(p); }
bool Ok(int result) { return result == NVSDK_NGX_Result_Success; }
ID3D12Resource *FakeD3D12() { return reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(0x1234560)); }
ID3D11Resource *FakeD3D11() { return reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(0x7654320)); }

void VTableLayout() {
    // A raw slot answered by the C++ overload of the type the slot is expected to hold.
    OwnParams own;
    NVSDK_NGX_Parameter &cpp = own, *raw = &own;
    RawSet(raw, NgxType::Float, "f", 0.375f);
    float f = 0; Check(Ok(cpp.Get("f", &f)) && f == 0.375f, "raw slot 6 is Set(float)");
    cpp.Set("f2", 0.625f);
    float f2 = 0; Check(Ok(RawGet(raw, NgxType::Float, "f2", &f2)) && f2 == 0.625f, "raw slot 14 is Get(float*)");
    RawSet(raw, NgxType::Int, "i", -7);
    int i = 0; Check(Ok(cpp.Get("i", &i)) && i == -7, "raw slot 3 is Set(int)");
    RawSet(raw, NgxType::Pointer, "p", static_cast<void *>(FakeD3D12()));
    void *p = nullptr; Check(Ok(cpp.Get("p", &p)) && p == FakeD3D12(), "raw slot 0 is Set(void*)");
    RawSet(raw, NgxType::Double, "d", 2.0625);
    double d = 0; Check(Ok(cpp.Get("d", &d)) && d == 2.0625, "raw slot 5 is Set(double)");
    // The add-on's own host-block helpers (ngx_params.cpp) on the shim: the same slots on both.
    SetUInt(raw, "DLSSNR.Width", 1920u);
    unsigned w = 0; Check(GetUInt(raw, "DLSSNR.Width", &w) && w == 1920u, "ngx_params SetUInt/GetUInt on OwnParams");
    SetFloat(raw, "DLSSNR.MVecScaleX", -0.5f);
    float sx = 0; Check(GetFloat(raw, "DLSSNR.MVecScaleX", &sx) && sx == -0.5f, "ngx_params SetFloat/GetFloat on OwnParams");
    Check(FloatSetterSlot() == 6 && FloatGetterSlot() == 14, "ngx_params finds the float pair at 6/14 on OwnParams");
    SetResource(raw, "DLSSNR.Color", FakeD3D12());
    Check(GetResource(raw, "DLSSNR.Color") == FakeD3D12(), "ngx_params SetResource/GetResource on OwnParams");
}

void RoundTrips() {
    OwnParams own; NVSDK_NGX_Parameter *raw = &own;
    RawSet(raw, NgxType::ULL, "ull", 0x100000005ull);
    RawSet(raw, NgxType::Float, "float", 2.5f);
    RawSet(raw, NgxType::Double, "double", 1.25);
    RawSet(raw, NgxType::UInt, "uint", 7u);
    RawSet(raw, NgxType::Int, "int", -3);
    RawSet(raw, NgxType::D3D11, "d3d11", FakeD3D11());
    RawSet(raw, NgxType::D3D12, "d3d12", FakeD3D12());
    RawSet(raw, NgxType::Pointer, "void", static_cast<void *>(FakeD3D11()));
    unsigned long long ull = 0; Check(Ok(RawGet(raw, NgxType::ULL, "ull", &ull)) && ull == 0x100000005ull, "ULL round trip");
    float f = 0; Check(Ok(RawGet(raw, NgxType::Float, "float", &f)) && f == 2.5f, "float round trip");
    double d = 0; Check(Ok(RawGet(raw, NgxType::Double, "double", &d)) && d == 1.25, "double round trip");
    unsigned u = 0; Check(Ok(RawGet(raw, NgxType::UInt, "uint", &u)) && u == 7u, "uint round trip");
    int i = 0; Check(Ok(RawGet(raw, NgxType::Int, "int", &i)) && i == -3, "int round trip");
    ID3D11Resource *r11 = nullptr; Check(Ok(RawGet(raw, NgxType::D3D11, "d3d11", &r11)) && r11 == FakeD3D11(), "D3D11 round trip");
    ID3D12Resource *r12 = nullptr; Check(Ok(RawGet(raw, NgxType::D3D12, "d3d12", &r12)) && r12 == FakeD3D12(), "D3D12 round trip");
    void *v = nullptr; Check(Ok(RawGet(raw, NgxType::Pointer, "void", &v)) && v == FakeD3D11(), "void* round trip");

    // Absent keys and Reset.
    unsigned missing = 42;
    Check(RawGet(raw, NgxType::UInt, "absent", &missing) == NVSDK_NGX_Result_FAIL_UnsupportedParameter && missing == 42,
          "absent key -> 0xBAD00010 (what every stand host answers), value untouched");
    void *missingPointer = nullptr; Check(!Ok(RawGet(raw, NgxType::Pointer, "absent", &missingPointer)), "absent pointer key -> not success");
    RawReset(raw);
    Check(!Ok(RawGet(raw, NgxType::UInt, "uint", &u)) && own.Values().empty(), "Reset clears every key");
}

// The bench block's conversions (tools/bench12/ngx_nr_params.cpp Number/Pointer).
void Conversions() {
    OwnParams own; NVSDK_NGX_Parameter *raw = &own;
    RawSet(raw, NgxType::UInt, "u", 7u);
    int i = 0; Check(Ok(RawGet(raw, NgxType::Int, "u", &i)) && i == 7, "uint read as int");
    float f = 0; Check(Ok(RawGet(raw, NgxType::Float, "u", &f)) && f == 7.0f, "uint read as float");
    double d = 0; Check(Ok(RawGet(raw, NgxType::Double, "u", &d)) && d == 7.0, "uint read as double");
    unsigned long long ull = 0; Check(Ok(RawGet(raw, NgxType::ULL, "u", &ull)) && ull == 7ull, "uint read as ULL");
    RawSet(raw, NgxType::Int, "neg", -3);
    Check(Ok(RawGet(raw, NgxType::ULL, "neg", &ull)) && ull == static_cast<unsigned long long>(-3ll), "int read as ULL sign-extends");
    unsigned u = 0; Check(Ok(RawGet(raw, NgxType::UInt, "neg", &u)) && u == static_cast<unsigned>(-3), "int read as uint");
    Check(Ok(RawGet(raw, NgxType::Float, "neg", &f)) && f == -3.0f, "int read as float");
    RawSet(raw, NgxType::Float, "real", 2.5f);
    Check(Ok(RawGet(raw, NgxType::Double, "real", &d)) && d == 2.5, "float read as double");
    Check(Ok(RawGet(raw, NgxType::UInt, "real", &u)) && u == 2u, "float read as uint truncates");
    Check(Ok(RawGet(raw, NgxType::Int, "real", &i)) && i == 2, "float read as int truncates");
    Check(Ok(RawGet(raw, NgxType::ULL, "real", &ull)) && ull == 2ull, "float read as ULL truncates");
    RawSet(raw, NgxType::Double, "wide", 1.25);
    Check(Ok(RawGet(raw, NgxType::Float, "wide", &f)) && f == 1.25f, "double read as float");
    RawSet(raw, NgxType::ULL, "big", 0x100000005ull);
    Check(Ok(RawGet(raw, NgxType::UInt, "big", &u)) && u == 5u, "ULL read as uint keeps the low bits");
    // Pointers: any pointer overload answers any pointer-typed key; numbers and pointers do not mix.
    RawSet(raw, NgxType::D3D12, "res", FakeD3D12());
    void *v = nullptr; Check(Ok(RawGet(raw, NgxType::Pointer, "res", &v)) && v == FakeD3D12(), "D3D12 key read as void*");
    ID3D11Resource *r11 = nullptr; Check(Ok(RawGet(raw, NgxType::D3D11, "res", &r11)) && static_cast<void *>(r11) == FakeD3D12(), "D3D12 key read as D3D11");
    RawSet(raw, NgxType::Pointer, "vp", static_cast<void *>(FakeD3D11()));
    ID3D12Resource *r12 = nullptr; Check(Ok(RawGet(raw, NgxType::D3D12, "vp", &r12)) && static_cast<void *>(r12) == FakeD3D11(), "void* key read as D3D12");
    Check(!Ok(RawGet(raw, NgxType::ULL, "res", &ull)), "pointer key read as ULL -> not success");
    Check(!Ok(RawGet(raw, NgxType::UInt, "res", &u)), "pointer key read as uint -> not success");
    Check(!Ok(RawGet(raw, NgxType::Pointer, "u", &v)), "numeric key read as void* -> not success");
    RawSet(raw, NgxType::UInt, "res", 9u); // a later Set of another type replaces the key
    Check(Ok(RawGet(raw, NgxType::UInt, "res", &u)) && u == 9u && !Ok(RawGet(raw, NgxType::Pointer, "res", &v)), "a Set replaces the type");
}

void TraceAndCopy() {
    OwnParams host;
    host.Set("DLSSNR.Width", 1920u);
    host.Set("DLSSNR.Intensity", 2.0f);
    host.Set("DLSSNR.Color", FakeD3D12());
    host.Set("DLSSNR.Unread", 5u);
    TraceParams trace(&host);
    NVSDK_NGX_Parameter *raw = &trace;
    unsigned w = 0; Check(Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Width", &w)) && w == 1920u, "trace forwards a read");
    Check(Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Width", &w)), "trace: second read");
    float intensity = 0; Check(Ok(RawGet(raw, NgxType::Float, "DLSSNR.Intensity", &intensity)) && intensity == 2.0f, "trace forwards a float read");
    ID3D12Resource *colour = nullptr; Check(Ok(RawGet(raw, NgxType::D3D12, "DLSSNR.Color", &colour)) && colour == FakeD3D12(), "trace forwards a pointer read");
    unsigned reset = 0; Check(!Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Reset", &reset)), "trace forwards an absent key's failure");
    RawSet(raw, NgxType::UInt, "DLSSNR.Out", 3u);
    Check(host.Has("DLSSNR.Out"), "trace forwards a write");
    const auto &keys = trace.Keys();
    Check(keys.size() == 5, "trace records once per key, overload and direction");
    Check(keys.size() == 5 && keys[0].name == "DLSSNR.Width" && keys[0].type == NgxType::UInt && !keys[0].set && keys[0].hostResult == NVSDK_NGX_Result_Success, "trace entry: uint read");
    Check(keys.size() == 5 && keys[3].name == "DLSSNR.Reset" && keys[3].hostResult != NVSDK_NGX_Result_Success, "trace entry: absent key with the host's failure");
    Check(keys.size() == 5 && keys[4].set && keys[4].type == NgxType::UInt, "trace entry: write");

    OwnParams copy;
    copy.Set("Stale", 1u);
    CopyTracedKeys(trace, &host, copy);
    Check(!copy.Has("Stale"), "copy resets the target first");
    Check(copy.Values().size() == 3, "copy holds the read keys the host answered");
    unsigned cw = 0; Check(GetUInt(&copy, "DLSSNR.Width", &cw) && cw == 1920u, "copied uint");
    float ci = 0; Check(Ok(copy.Get("DLSSNR.Intensity", &ci)) && ci == 2.0f, "copied float");
    Check(copy.Values().count("DLSSNR.Intensity") && std::holds_alternative<float>(copy.Values().at("DLSSNR.Intensity")), "copied with the runtime's overload");
    Check(GetResource(&copy, "DLSSNR.Color") == FakeD3D12(), "copied pointer");
    Check(!copy.Has("DLSSNR.Reset"), "a key absent in the host stays absent");
    Check(!copy.Has("DLSSNR.Unread") && !copy.Has("DLSSNR.Out"), "unread keys and runtime writes are not copied");
    RawReset(raw);
    Check(trace.ResetCalled() && host.Values().empty(), "trace forwards Reset");
}

// Exact result codes (review ruling of 0b-2a): a key the host did not answer keeps the host's code in the own block.
void AbsentCodes() {
    OwnParams own; NVSDK_NGX_Parameter *raw = &own;
    own.PutAbsent("DLSSNR.Reset", NVSDK_NGX_Result_FAIL_InvalidParameter);
    unsigned u = 5;
    Check(RawGet(raw, NgxType::UInt, "DLSSNR.Reset", &u) == NVSDK_NGX_Result_FAIL_InvalidParameter && u == 5, "a recorded absent key answers the host's code");
    float f = 0;
    Check(RawGet(raw, NgxType::Float, "DLSSNR.Reset", &f) == NVSDK_NGX_Result_FAIL_InvalidParameter, "whatever the overload");
    Check(own.Knows("DLSSNR.Reset") && !own.Has("DLSSNR.Reset"), "known as absent, not as a value");
    RawSet(raw, NgxType::UInt, "DLSSNR.Reset", 1u);
    Check(Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Reset", &u)) && u == 1u && own.Absent().empty(), "a Set replaces the absent entry");
    RawReset(raw);
    Check(!own.Knows("DLSSNR.Reset"), "Reset clears absent entries too");

    OwnParams host;
    host.Set("DLSSNR.Width", 1920u);
    host.PutAbsent("DLSSNR.Reset", NVSDK_NGX_Result_FAIL_InvalidParameter);
    TraceParams trace(&host);
    unsigned w = 0, reset = 0;
    RawGet(&trace, NgxType::UInt, "DLSSNR.Width", &w);
    RawGet(&trace, NgxType::UInt, "DLSSNR.Reset", &reset);
    RawGet(&trace, NgxType::UInt, "DLSSNR.Other", &reset);
    OwnParams copy;
    CopyTracedKeys(trace, &host, copy);
    Check(copy.Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_FAIL_InvalidParameter, "the copy keeps the host's code of an absent key");
    Check(copy.Get("DLSSNR.Other", &reset) == NVSDK_NGX_Result_FAIL_UnsupportedParameter, "and the host's default code for a key it never had");
    NgxValue value;
    Check(ReadAs(&host, "DLSSNR.Reset", NgxType::UInt, &value) == NVSDK_NGX_Result_FAIL_InvalidParameter, "ReadAs returns the host's code");
    void *pointer = nullptr;
    ID3D12Resource *resource = nullptr;
    Check(RawGet(&copy, NgxType::Pointer, "DLSSNR.Reset", &pointer) == NVSDK_NGX_Result_FAIL_InvalidParameter &&
              RawGet(&copy, NgxType::D3D12, "DLSSNR.Reset", &resource) == NVSDK_NGX_Result_FAIL_InvalidParameter,
          "an absent key answers the host's code through the pointer overloads too");
}

// Menu mode's exit reset (C8): the evaluate reads DLSSNR.Reset=1 through the wrapper; the host's block keeps its key
// presence, value and result code; every other key goes to the host's block as it is.
void ResetWrapper() {
    OwnParams absent; // the host never set Reset
    absent.Set("DLSSNR.Width", 1920u);
    ResetParams wrapper(&absent);
    NVSDK_NGX_Parameter *raw = &wrapper;
    unsigned u = 0; Check(Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Reset", &u)) && u == 1u, "the evaluate sees Reset=1 (uint slot)");
    int i = 0; Check(Ok(RawGet(raw, NgxType::Int, "DLSSNR.Reset", &i)) && i == 1, "the evaluate sees Reset=1 (int slot)");
    float f = 0; Check(Ok(RawGet(raw, NgxType::Float, "DLSSNR.Reset", &f)) && f == 1.0f, "the evaluate sees Reset=1 (float slot)");
    unsigned helper = 0; Check(GetUInt(raw, "DLSSNR.Reset", &helper) && helper == 1u, "ngx_params GetUInt sees Reset=1 through the wrapper");
    unsigned w = 0; Check(Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Width", &w)) && w == 1920u, "other keys are read from the host's block");
    RawSet(raw, NgxType::UInt, "DLSSNR.Out", 3u);
    Check(absent.Has("DLSSNR.Out"), "other writes reach the host's block");
    RawSet(raw, NgxType::UInt, "DLSSNR.Reset", 0u);
    Check(Ok(RawGet(raw, NgxType::UInt, "DLSSNR.Reset", &u)) && u == 0u, "a write of Reset is read back through the wrapper");
    Check(!absent.Knows("DLSSNR.Reset"), "an absent Reset key stays absent in the host's block");
    Check(absent.Get("DLSSNR.Reset", &u) == NVSDK_NGX_Result_FAIL_UnsupportedParameter, "and keeps the host's result code");

    OwnParams coded; // the host answers Reset with its own failure code
    coded.PutAbsent("DLSSNR.Reset", NVSDK_NGX_Result_FAIL_InvalidParameter);
    {
        ResetParams around(&coded);
        Check(GetUInt(&around, "DLSSNR.Reset", &u) && u == 1u, "Reset=1 over a host that answers a failure code");
    }
    Check(coded.Get("DLSSNR.Reset", &u) == NVSDK_NGX_Result_FAIL_InvalidParameter && !coded.Has("DLSSNR.Reset"),
          "the host's failure code for Reset is unchanged");

    OwnParams present; // the host set Reset=0
    present.Set("DLSSNR.Reset", 0u);
    {
        ResetParams around(&present);
        TraceParams trace(&around); // as the traced evaluate composes them: trace -> wrapper -> host
        unsigned seen = 7; Check(Ok(RawGet(&trace, NgxType::UInt, "DLSSNR.Reset", &seen)) && seen == 1u, "a trace over the wrapper sees Reset=1");
        RawSet(&trace, NgxType::UInt, "DLSSNR.Reset", 1u);
    }
    unsigned kept = 9; Check(Ok(present.Get("DLSSNR.Reset", &kept)) && kept == 0u, "a present Reset key keeps the host's old value");
    ResetParams unbound;
    Check(!Ok(unbound.Get("DLSSNR.Width", &w)), "an unbound wrapper answers a failure for other keys");
}
} // namespace

int main() {
    VTableLayout();
    RoundTrips();
    Conversions();
    TraceAndCopy();
    AbsentCodes();
    ResetWrapper();
    if (failures == 0) std::printf("ngx param shim: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
