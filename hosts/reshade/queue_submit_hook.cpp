#include "hosts/reshade/queue_submit_hook.h"
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cwchar>

#pragma intrinsic(_ReturnAddress)

namespace ofps::reshade {
namespace {
using PFN_Execute = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
// ID3D12CommandQueue's vtable: IUnknown (3), ID3D12Object (4), ID3D12DeviceChild (1), ID3D12Pageable (0), then
// UpdateTileMappings, CopyTileMappings, ExecuteCommandLists.
constexpr unsigned kExecuteSlot = 10;
constexpr unsigned kMaxSkipped = 4;

std::atomic<PFN_Execute> g_real{nullptr}; // the slot's pointer before the swap (the runtime's, or an earlier hook's)
std::atomic<void **> g_vtable{nullptr};   // the swapped vtable while installed
std::atomic<SubmitSink> g_before{nullptr}, g_after{nullptr};
std::atomic<std::uintptr_t> g_skipLo[kMaxSkipped], g_skipHi[kMaxSkipped];

std::uintptr_t ImageEnd(HMODULE module) {
    const auto base = reinterpret_cast<const unsigned char *>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return reinterpret_cast<std::uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
}

bool Skipped(const void *address) {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    for (unsigned i = 0; i < kMaxSkipped; ++i)
        if (at >= g_skipLo[i].load(std::memory_order_relaxed) && at < g_skipHi[i].load(std::memory_order_relaxed)) return true;
    return false;
}

// Fix round 1 (C1): `before` only for what must precede the list on its queue; `after` once the native call returned,
// so completion tracking never runs ahead of the real submission. The sinks are read again after the call: a silence
// in between takes effect at once.
void STDMETHODCALLTYPE HookedExecute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    const bool reported = queue && lists && count && !Skipped(_ReturnAddress());
    if (reported)
        if (const SubmitSink before = g_before.load(std::memory_order_acquire)) before(queue, count, lists);
    g_real.load(std::memory_order_acquire)(queue, count, lists);
    if (reported)
        if (const SubmitSink after = g_after.load(std::memory_order_acquire)) after(queue, count, lists);
}

// The D3D12 runtime's class (d3d12.dll, D3D12Core.dll, D3D12SDKLayers.dll), not a proxy of ReShade or of another wrapper.
bool InD3D12Runtime(const void *address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) || !module) return false;
    if (Skipped(address)) return false; // ReShade installed as d3d12.dll is a skipped module
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsnicmp(name, L"d3d12", 5) == 0;
}

// One aligned pointer-sized compare-exchange on the slot, with the page made writable around it. False: the page could
// not be made writable, or the slot no longer held `expected`.
bool SwapSlot(void **slot, void *expected, void *desired) {
    if (reinterpret_cast<std::uintptr_t>(slot) % sizeof(void *) != 0) return false; // the write must be one aligned store
    // A vtable normally sits in read-only data; should it share a page with code, the page stays executable meanwhile.
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(slot, &info, sizeof(info))) return false;
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), (info.Protect & kExecute) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return false;
    const bool swapped = InterlockedCompareExchangePointer(slot, desired, expected) == expected;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old, &ignored);
    return swapped;
}
} // namespace

void SubmitHookSkip(const HMODULE *skipped, unsigned skippedCount) {
    for (unsigned i = 0; i < kMaxSkipped; ++i) {
        const HMODULE module = skipped && i < skippedCount ? skipped[i] : nullptr;
        const std::uintptr_t end = module ? ImageEnd(module) : 0;
        g_skipLo[i].store(end ? reinterpret_cast<std::uintptr_t>(module) : 0, std::memory_order_relaxed);
        g_skipHi[i].store(end, std::memory_order_relaxed);
    }
}

bool SubmitHookInstall(ID3D12Device *device, const SubmitSinks &sinks, const HMODULE *skipped, unsigned skippedCount,
                       const char **why) {
    const char *ignored = nullptr;
    if (!why) why = &ignored;
    if (g_vtable.load()) return true;
    if (!device || (!sinks.before && !sinks.after)) { *why = "no device"; return false; }
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *probe = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probe))) || !probe) { *why = "no temporary queue on the device"; return false; }
    void **vtable = *reinterpret_cast<void ***>(probe);
    probe->Release(); // the vtable stays: the device keeps the runtime loaded
    SubmitHookSkip(skipped, skippedCount);
    if (!InD3D12Runtime(vtable)) { *why = "the queue's vtable is not the D3D12 runtime's (a proxy)"; return false; }
    void **slot = &vtable[kExecuteSlot];
    void *original = *static_cast<void *volatile *>(slot);
    if (!original || original == reinterpret_cast<void *>(&HookedExecute)) { *why = "unexpected vtable slot"; return false; }
    g_real.store(reinterpret_cast<PFN_Execute>(original), std::memory_order_release);
    g_before.store(sinks.before, std::memory_order_release); // before the slot can lead here
    g_after.store(sinks.after, std::memory_order_release);
    if (!SwapSlot(slot, original, reinterpret_cast<void *>(&HookedExecute))) {
        SubmitHookSilence();
        *why = "the vtable slot could not be written";
        return false;
    }
    // Fix round 1 (C2): the slot now leads into this module, which must never be unmapped while it can be called.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&HookedExecute), &self)) {
        SwapSlot(slot, reinterpret_cast<void *>(&HookedExecute), original);
        SubmitHookSilence();
        *why = "the module could not be pinned";
        return false;
    }
    g_vtable.store(vtable);
    return true;
}

bool SubmitHookInstalled() { return g_vtable.load() != nullptr; }

bool SubmitHookCovers(ID3D12CommandQueue *queue) {
    void **vtable = g_vtable.load();
    return queue && vtable && *reinterpret_cast<void ***>(queue) == vtable;
}

void SubmitHookSilence() {
    g_before.store(nullptr, std::memory_order_release);
    g_after.store(nullptr, std::memory_order_release);
}

bool SubmitHookRestore() {
    SubmitHookSilence();
    void **vtable = g_vtable.exchange(nullptr);
    if (!vtable) return true;
    // The original stays in g_real: a call that read the slot before this write still forwards through the hook.
    return SwapSlot(&vtable[kExecuteSlot], reinterpret_cast<void *>(&HookedExecute), reinterpret_cast<void *>(g_real.load()));
}

} // namespace ofps::reshade
