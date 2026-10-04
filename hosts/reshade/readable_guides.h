#pragma once

#include <d3d12.h>

namespace ofps::reshade {

DXGI_FORMAT ReadableTwinFormat(DXGI_FORMAT format, bool depth);

// One evaluate owns the temporary parameter substitutions. The GPU copies and their twins
// remain owned by the shell until the command list's submission fence has completed.
class ReadableGuides {
public:
    ReadableGuides(ID3D12GraphicsCommandList *cmd, void *params);
    ~ReadableGuides();
    ReadableGuides(const ReadableGuides &) = delete;
    ReadableGuides &operator=(const ReadableGuides &) = delete;

private:
    struct Swap { const char *name; ID3D12Resource *original; };
    void *params_ = nullptr;
    Swap swaps_[2]{};
    unsigned count_ = 0;
};

// Menu mode's guide snapshots (menu_guides.cpp) ride the twins' submission tracking: a texture written on a host list
// is held (a reference and a fence, like a twin) from the recorded write until that list was submitted and the fence
// signalled after it passed. Busy: a recorded write is still in flight (record no other; a twin is never re-recorded in
// flight either). Write: false when the list cannot be tagged or no fence could be made (then record nothing). Drop:
// the hold is retired; its reference goes (through the core's graveyard, gated on its fence) once the write passed.
bool ReadableGuidesHoldBusy(ID3D12Resource *resource);
bool ReadableGuidesHoldWrite(ID3D12GraphicsCommandList *list, ID3D12Resource *resource);
void ReadableGuidesHoldDrop(ID3D12Resource *resource);

void ReadableGuidesExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list);
void ReadableGuidesPresented();
void ReadableGuidesQueueDestroyed(ID3D12CommandQueue *queue);
void ShutdownReadableGuides();

} // namespace ofps::reshade
