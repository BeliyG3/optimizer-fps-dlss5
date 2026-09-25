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

void ReadableGuidesExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list);
void ReadableGuidesPresented();
void ReadableGuidesQueueDestroyed(ID3D12CommandQueue *queue);
void ShutdownReadableGuides();

} // namespace ofps::reshade
