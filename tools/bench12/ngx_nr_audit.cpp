#include "ngx_nr_params.h"
#include <d3d12.h>
#include <cstdio>
#include <map>
#include <exception>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

void NgxParameterMap::AuditFinal(const char *path, const char *operation) const
{
    std::lock_guard<std::mutex> lock(mutex);
    // Sort keys so separate processes can be compared without pointer or hash-table ordering.
    const std::map<std::string, Value> sorted(values.begin(), values.end());
    std::ostringstream block;
    block << std::setprecision(17) << "[ngx audit] begin " << path << ' ' << operation << '\n';
    for (const auto &[key, value] : sorted) {
        // Shell-only float-slot discovery probe, not a model input. Exclude it
        // from comparison without changing the parameter block sent to NGX.
        if (key == "PeripheralWarp.FloatProbe") continue;
        // Off/pass-through audit exception: DLSSNR.Width/Height may use uint32
        // in the native bench and int32 in the direct host. Only equal numeric
        // values are equivalent (including their retained Evaluate entries).
        // Keep the raw types AND values here: compare_plan2.py scopes this
        // exception to off_t0/core_off and rejects changed sizes. Resource
        // setters are compared by role/description, not pointer vtable slot.
        block << "[ngx audit] " << key << ' ' << value.type << ' ';
        if (value.kind == Kind::Real) block << value.real;
        else if (value.kind == Kind::Signed) block << static_cast<long long>(value.bits);
        else if (value.kind == Kind::Unsigned) block << value.bits;
        else if (!value.pointer) block << "null";
        else if (key == "DLSSNR.Color" || key == "DLSSNR.Depth" ||
                 key == "DLSSNR.MVec" || key == "DLSSNR.Output") {
            const auto d = static_cast<ID3D12Resource *>(value.pointer)->GetDesc();
            block << "resource dim=" << unsigned(d.Dimension) << " width=" << d.Width
                << " height=" << d.Height << " array=" << d.DepthOrArraySize << " mips=" << d.MipLevels
                << " format=" << unsigned(d.Format) << " samples=" << d.SampleDesc.Count
                << " quality=" << d.SampleDesc.Quality << " layout=" << unsigned(d.Layout)
                << " flags=" << unsigned(d.Flags) << " alignment=" << d.Alignment;
        } else block << "pointer";
        block << '\n';
    }
    block << "[ngx audit] end\n";
    // Keep an exact file independent of driver output and PowerShell's redirected stdout chunks.
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> outputLock(outputMutex);
    static std::ofstream output("ngx_final_audit.log", std::ios::binary | std::ios::trunc);
    output << block.str(); output.flush();
    if (!output) throw std::runtime_error("cannot write final NGX audit");

}

// Runs in the executable that owns the parameter map, including its RTTI and CRT.
extern "C" __declspec(dllexport) void OfpsBenchAuditNgx(
    void *parameters, const char *path, const char *operation) noexcept
{
    try {
        auto *map = dynamic_cast<NgxParameterMap *>(static_cast<NVSDK_NGX_Parameter *>(parameters));
        if (map) map->AuditFinal(path, operation);
    } catch (...) {
        std::fputs("[ngx audit] failed\n", stderr);
        std::terminate(); // Never carry an exception across the module boundary.
    }
}
