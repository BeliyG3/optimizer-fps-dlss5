#pragma once
// Diagnostics (DebugMenuDump=1): post-write-back dumps and menu_events.log (menu_dump.cpp; D3D11 swap chains:
// menu_dump_d3d11.cpp). Every call is made under the pipeline's lock.
#include <d3d11.h>
#include <d3d12.h>

namespace ofps::reshade {

void MenuEvent(const char *fmt, ...);
bool MenuDumpPrepare(ID3D12Device *device, const D3D12_RESOURCE_DESC &backBuffer); // (re)creates the pool
bool MenuDumpIdle(); // no dump copy in flight
// Copies the back buffer (PRESENT in and out) into a free readback buffer; probe = the one-off index check.
bool MenuDumpRecord(ID3D12GraphicsCommandList *list, ID3D12Resource *backBuffer, unsigned long long presentIndex, bool probe);
void MenuDumpSubmitted(UINT64 fenceValue); // the copies recorded since the last call retire at this f3 value
void MenuDumpDiscard();                    // the copies recorded since the last call were not submitted
double MenuDumpPoll(UINT64 completed);     // writes the BMPs whose copies completed; returns the ms spent
void MenuDumpRelease();
// menu_dump_<presentIndex>.bmp (or menu_dump_probe_...) next to the exe from 8-bit RGBA (swizzle) or BGRA rows.
bool MenuWriteBmp(const BYTE *rows, UINT rowPitch, UINT width, UINT height, bool swizzle, unsigned long long presentIndex, bool probe);

// D3D11 swap chains (the bridge path): staging copies on the immediate context after the write-back, mapped
// with DO_NOT_WAIT on a later present (never a CPU wait on the present path).
bool MenuDump11Prepare(ID3D11Device *device, UINT width, UINT height, DXGI_FORMAT format); // (re)creates the pool
bool MenuDump11Record(ID3D11DeviceContext *context, ID3D11Resource *backBuffer, unsigned long long presentIndex, bool probe);
bool MenuDump11Idle();
double MenuDump11Poll(ID3D11DeviceContext *context); // writes the BMPs whose copies completed; returns the ms spent
void MenuDump11Release();

} // namespace ofps::reshade
