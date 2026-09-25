#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "optiscaler_link.h"
#include "overlay.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace ofps::reshade {
namespace {

OptiScalerLink g_state;
std::wstring g_iniPath;
ULONGLONG g_lastRead = 0;
bool g_probed = false;
bool g_registered = false;
bool g_windowOpen = false;

bool IsOptiScalerModule(const wchar_t *path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) return false;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
    struct Translation { WORD language, codePage; } *translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void **>(&translations), &bytes) || bytes < sizeof(Translation))
        return false;
    wchar_t key[64]{};
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName",
               translations[0].language, translations[0].codePage);
    wchar_t *name = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&name), &length) || !name) return false;
    return wcsstr(name, L"OptiScaler") != nullptr;
}

void FindOptiScaler()
{
    HMODULE modules[1024]{};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
        if (!IsOptiScalerModule(path)) continue;
        if (wchar_t *slash = wcsrchr(path, L'\\')) slash[1] = 0;
        g_iniPath = std::wstring(path) + L"OptiScaler.ini";
        g_state.present = true;
        return;
    }
}

bool IniBool(const wchar_t *section, const wchar_t *key)
{
    wchar_t value[16]{};
    GetPrivateProfileStringW(section, key, L"", value, 16, g_iniPath.c_str());
    return _wcsicmp(value, L"true") == 0 || wcscmp(value, L"1") == 0;
}

unsigned IniNumber(const wchar_t *section, const wchar_t *key, unsigned fallback)
{
    wchar_t value[32]{};
    GetPrivateProfileStringW(section, key, L"", value, 32, g_iniPath.c_str());
    wchar_t *end = nullptr;
    const unsigned long number = wcstoul(value, &end, 0); // "auto" and "" parse as nothing
    return end != value && number != 0 ? static_cast<unsigned>(number) : fallback;
}

void RefreshIni()
{
    const ULONGLONG now = GetTickCount64();
    if (!g_state.present || now - g_lastRead < 2000) return;
    g_lastRead = now;
    g_state.ownCompression = IniBool(L"DlssNr", L"SpatialCompression");
    g_state.ownPasses = static_cast<int>(IniNumber(L"DlssNr", L"Passes", 1));
    g_state.shortcutKey = IniNumber(L"Menu", L"ShortcutKey", VK_INSERT);
}

void OnOverlayFrame(::reshade::api::effect_runtime *runtime)
{
    RefreshIni();
    const bool wasOpen = g_windowOpen;
    if (runtime->is_key_pressed(g_state.shortcutKey)) g_windowOpen = !g_windowOpen;
    if (g_windowOpen != wasOpen)
        ::reshade::log::message(::reshade::log::level::info, g_windowOpen
            ? "Optimizer FPS: settings window opened with OptiScaler's menu key"
            : "Optimizer FPS: settings window closed");
    // ReShade draws its own cursor exactly while its overlay is open; the tab is there then.
    if (!g_windowOpen || ImGui::GetIO().MouseDrawCursor) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - 40.0f, 60.0f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    if (ImGui::Begin("Optimizer FPS for DLSS5##optiscaler", &g_windowOpen, ImGuiWindowFlags_NoCollapse))
        DrawOverlay(runtime);
    ImGui::End();
}

} // namespace

const OptiScalerLink &OptiScalerState()
{
    RefreshIni();
    return g_state;
}

void OptiScalerLinkInit()
{
    if (g_probed) return;
    g_probed = true;
    FindOptiScaler();
    if (!g_state.present) return;
    RefreshIni();
    ::reshade::register_event<::reshade::addon_event::reshade_overlay>(OnOverlayFrame);
    g_registered = true;
    ::reshade::log::message(::reshade::log::level::info,
        "Optimizer FPS: OptiScaler found; its menu key also opens this add-on's settings window");
}

void OptiScalerLinkShutdown()
{
    if (!g_registered) return;
    ::reshade::unregister_event<::reshade::addon_event::reshade_overlay>(OnOverlayFrame);
    g_registered = false;
}

} // namespace ofps::reshade
