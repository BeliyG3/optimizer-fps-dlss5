#include "hosts/reshade/menu_bridge_d3d11.h"
#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <vector>

// Bench diagnostics for the bridge (MenuMode=1 on a D3D11 swap chain, with the bench's --debug-layer): the D3D11 and
// D3D12 debug layers' messages since the last call, in ReShade.log. Only with DebugMenuBridgeCanary=1 (final review I1):
// it changes the game's D3D12 info queue for the session (every message stored) and injects one API error.
namespace ofps::reshade {
using namespace menu_detail;
namespace {
struct Seen {
    UINT64 d11 = 0, d12 = 0;          // stored messages already logged
    UINT64 counts11[5] = {}, counts12[5] = {}; // per severity: corruption, error, warning, info, message
    bool logged = false;
};
Seen &S() { static Seen *seen = new Seen(); return *seen; }
constexpr unsigned kListed = 8; // message texts per call and layer

template <class Queue, class Message>
void Drain(Queue *q, UINT64 &seen, UINT64 (&counts)[5], const char *layer, const char *when) {
    const UINT64 stored = q->GetNumStoredMessages();
    if (stored < seen) seen = 0; // someone cleared the queue
    unsigned listed = 0;
    for (UINT64 m = seen; m < stored; ++m) {
        SIZE_T size = 0;
        if (FAILED(q->GetMessage(m, nullptr, &size)) || size == 0) continue;
        std::vector<char> buffer(size);
        auto *msg = reinterpret_cast<Message *>(buffer.data());
        if (FAILED(q->GetMessage(m, msg, &size))) continue;
        const unsigned severity = unsigned(msg->Severity) < 5 ? unsigned(msg->Severity) : 4;
        ++counts[severity];
        if (severity <= 2 && listed++ < kListed)
            Log("menu bridge debug (%s, %s): severity %u id %d: %.300s", layer, when, severity, int(msg->ID), msg->pDescription);
    }
    if (stored > seen)
        Log("menu bridge debug (%s, %s): %llu new; so far corruption %llu, error %llu, warning %llu, info %llu, message %llu", layer, when,
            stored - seen, counts[0], counts[1], counts[2], counts[3], counts[4]);
    seen = stored;
}
} // namespace

void MenuBridgeDebugMessages(ID3D11Device *d11, ID3D12Device *d12, const char *when) {
    if (!CurrentShellSettings().debugMenuBridgeCanary) return; // the product never touches the game's info queues
    auto &s = S();
    ComPtr<ID3D11InfoQueue> q11;
    ComPtr<ID3D12InfoQueue> q12;
    const bool has11 = d11 && SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&q11)));
    const bool has12 = d12 && SUCCEEDED(d12->QueryInterface(IID_PPV_ARGS(&q12)));
    if (!s.logged) {
        s.logged = true;
        if (has12) { // every severity stored (an empty filter on top of any other), no count limit (the default drops later ones)
            q12->PushEmptyStorageFilter(); q12->SetMessageCountLimit(UINT64(-1));
            Drain<ID3D12InfoQueue, D3D12_MESSAGE>(q12.Get(), s.d12, s.counts12, "D3D12", "before the bridge");
            // Positive control of the channel: a zero-width buffer is an API error the layer reports (creation fails, no
            // device removal). It is not counted: the counts below start after it.
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 0; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> canary;
            // Skipped when the layer breaks on errors (a debugger would stop in it): the channel is then unproven.
            const bool breaks = q12->GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR) || q12->GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION);
            const HRESULT hr = breaks ? E_ABORT
                : d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&canary));
            const UINT64 stored = q12->GetNumStoredMessages();
            Log("menu bridge debug: D3D12 canary (a zero-width buffer) returned 0x%08X and stored %llu message(s) (not counted)", unsigned(hr), stored - s.d12);
            s.d12 = stored;
            for (auto &count : s.counts12) count = 0;
        }
        Log("menu bridge debug: D3D11 info queue %s, D3D12 info queue %s", has11 ? "present" : "absent (no debug device)",
            has12 ? "present" : "absent (no debug layer)");
    }
    if (has11) Drain<ID3D11InfoQueue, D3D11_MESSAGE>(q11.Get(), s.d11, s.counts11, "D3D11", when);
    if (has12) {
        Drain<ID3D12InfoQueue, D3D12_MESSAGE>(q12.Get(), s.d12, s.counts12, "D3D12", when);
        Log("menu bridge debug (D3D12, %s): stored %llu, allowed by the storage filter %llu, denied %llu, discarded %llu, filter stack %u", when,
            q12->GetNumStoredMessages(), q12->GetNumMessagesAllowedByStorageFilter(), q12->GetNumMessagesDeniedByStorageFilter(),
            q12->GetNumMessagesDiscardedByMessageCountLimit(), q12->GetStorageFilterStackSize());
    }
}

} // namespace ofps::reshade
