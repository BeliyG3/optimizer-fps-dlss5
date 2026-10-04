#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <cstdio>
#include <memory>
#include <vector>

// GPU time of the private list, and the slow-pass budget: three menu passes in a row of kSlowPassMs or more suspend
// the run (MenuGpuTimeSlow), since such a menu frame costs the game more than NR gives it.
namespace ofps::reshade {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned kReportEvery = 60;
constexpr double kSlowPassMs = 100.0; // a menu frame this slow is not worth it (007 First Light starved of VRAM: 400-600 ms)

// Two timestamps per private ring slot, resolved at the end of the slot's list into a readback buffer.
struct Timing {
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    UINT64 frequency = 0;
    std::vector<UINT64> pending; // per slot: the f2 value its timestamps are valid at, 0 = nothing to read
    double sum = 0, max = 0, totalSum = 0, totalMax = 0;
    unsigned count = 0, totalCount = 0;
    unsigned slowInRow = 0; // passes of kSlowPassMs or more in a row, from the run's start
};
Timing *g_timing = nullptr; // heap-owned, touched under the pipeline's lock only

void Report(unsigned long long index) {
    Timing &t = *g_timing;
    MenuEvent("gpu %llu %.4f %.4f", index, t.sum / t.count, t.max);
    char text[160];
    std::snprintf(text, sizeof(text), "menu pipeline: gpu per menu present (private list) avg %.3f ms, max %.3f ms over the last %u",
                  t.sum / t.count, t.max, t.count);
    Host().Log(OFPS_LOG_INFO, text);
    t.sum = t.max = 0; t.count = 0;
}
} // namespace

bool MenuGpuTimeBuild(ID3D12Device *device, ID3D12CommandQueue *queue, unsigned slots) {
    auto t = std::make_unique<Timing>();
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2 * slots;
    if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&t->heap)))) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 2ull * slots * sizeof(UINT64);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&t->readback))) ||
        FAILED(queue->GetTimestampFrequency(&t->frequency)) || t->frequency == 0) return false;
    t->pending.assign(slots, 0);
    // Null unless a build failed after this call: a drained or never-built pipeline released it. Safe only because one
    // builder exists at a time: menu mode serves one swap chain (Pipeline::bound, final review Codex I3), so a D3D12
    // pipeline and a D3D11 bridge never both hold a query heap in flight.
    delete g_timing;
    g_timing = t.release();
    return true;
}

void MenuGpuTimeBegin(ID3D12GraphicsCommandList *list, unsigned slot) {
    if (g_timing) list->EndQuery(g_timing->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot);
}

void MenuGpuTimeEnd(ID3D12GraphicsCommandList *list, unsigned slot) {
    if (!g_timing) return;
    list->EndQuery(g_timing->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot + 1);
    list->ResolveQueryData(g_timing->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot, 2, g_timing->readback.Get(),
                           UINT64(2 * slot) * sizeof(UINT64));
}

void MenuGpuTimeSubmitted(unsigned slot, UINT64 value2) {
    if (g_timing && slot < g_timing->pending.size()) g_timing->pending[slot] = value2;
}

void MenuGpuTimePoll(UINT64 completed2, unsigned long long presentIndex) {
    if (!g_timing) return;
    Timing &t = *g_timing;
    for (unsigned slot = 0; slot < t.pending.size(); ++slot) {
        if (t.pending[slot] == 0 || completed2 < t.pending[slot]) continue;
        t.pending[slot] = 0;
        D3D12_RANGE range{SIZE_T(2 * slot) * sizeof(UINT64), SIZE_T(2 * slot + 2) * sizeof(UINT64)};
        void *mapped = nullptr;
        if (FAILED(t.readback->Map(0, &range, &mapped))) continue;
        const UINT64 *stamps = static_cast<const UINT64 *>(mapped) + 2 * slot;
        const double ms = stamps[1] > stamps[0] ? double(stamps[1] - stamps[0]) * 1000.0 / double(t.frequency) : 0.0;
        D3D12_RANGE none{0, 0}; t.readback->Unmap(0, &none);
        t.sum += ms; t.totalSum += ms; ++t.count; ++t.totalCount;
        if (ms > t.max) t.max = ms;
        if (ms > t.totalMax) t.totalMax = ms;
        t.slowInRow = ms >= kSlowPassMs ? t.slowInRow + 1 : 0;
        if (t.count == kReportEvery) {
            if (CurrentShellSettings().debugMenuDump != 0) Report(presentIndex); // diagnostics only: no log line per second in a menu
            else { t.sum = t.max = 0; t.count = 0; }
        }
    }
}

bool MenuGpuTimeSlow() { return g_timing && g_timing->slowInRow >= 3; }
void MenuGpuTimeRunStart() { if (g_timing) g_timing->slowInRow = 0; }

void MenuGpuTimeTotals(double *avgMs, double *maxMs, unsigned *samples) {
    *avgMs = g_timing && g_timing->totalCount ? g_timing->totalSum / g_timing->totalCount : 0.0;
    *maxMs = g_timing ? g_timing->totalMax : 0.0;
    *samples = g_timing ? g_timing->totalCount : 0;
}

void MenuGpuTimeRelease() {
    delete g_timing;
    g_timing = nullptr;
}

} // namespace ofps::reshade
