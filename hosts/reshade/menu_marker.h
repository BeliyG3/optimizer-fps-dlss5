#pragma once
// Diagnostics (DebugMenuPass=1): the marker pass of the menu pipeline (menu_marker.cpp).
#include "hosts/reshade/menu_pipeline.h"

namespace ofps::reshade {

// capture -> output, then a 64x64 opaque red square at (16,16). MenuMarkerCreate records the one-time
// upload on uploadList (the texture ends in COPY_SOURCE).
bool MenuMarkerCreate(ID3D12Device *device, DXGI_FORMAT format, ID3D12GraphicsCommandList *uploadList,
                      ID3D12Resource **texture, ID3D12Resource **upload);
void MenuMarkerBind(ID3D12Resource *texture);
bool MenuMarkerPass(MenuFrame &frame);

} // namespace ofps::reshade
