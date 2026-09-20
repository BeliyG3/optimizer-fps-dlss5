#include "menu.h"
#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_win32.h"
#include "external/imgui/backends/imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace {
struct GuiDescriptors { Device *device; std::vector<unsigned> available; };
GuiDescriptors descriptors;
LRESULT Message(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return ImGui_ImplWin32_WndProcHandler(window,message,w,l);
}
}
Menu::Menu(Device &d):device(d)
{
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename=nullptr; // All user-persisted state lives in settings.json.
    if(!ImGui_ImplWin32_Init(d.Window())) throw std::runtime_error("ImGui Win32 initialization failed");
    descriptors.device=&d; unsigned first=d.Allocate(64);
    for(unsigned i=0;i<64;++i) descriptors.available.push_back(first+i);
    ImGui_ImplDX12_InitInfo info;
    info.Device=d.gpu.Get(); info.CommandQueue=d.Queue(); info.NumFramesInFlight=2;
    info.RTVFormat=DXGI_FORMAT_R8G8B8A8_UNORM; info.SrvDescriptorHeap=d.heap.Get(); info.UserData=&descriptors;
    info.SrvDescriptorAllocFn=[](ImGui_ImplDX12_InitInfo *i,D3D12_CPU_DESCRIPTOR_HANDLE *cpu,D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
        auto &pool=*static_cast<GuiDescriptors *>(i->UserData);
        if(pool.available.empty()) throw std::runtime_error("ImGui descriptor pool exhausted");
        unsigned index=pool.available.back(); pool.available.pop_back(); *cpu=pool.device->Cpu(index); *gpu=pool.device->Gpu(index);
    };
    info.SrvDescriptorFreeFn=[](ImGui_ImplDX12_InitInfo *i,D3D12_CPU_DESCRIPTOR_HANDLE cpu,D3D12_GPU_DESCRIPTOR_HANDLE) {
        auto &pool=*static_cast<GuiDescriptors *>(i->UserData);
        auto step=pool.device->gpu->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        pool.available.push_back(unsigned((cpu.ptr-pool.device->Cpu(0).ptr)/step));
    };
    if(!ImGui_ImplDX12_Init(&info)) throw std::runtime_error("ImGui D3D12 initialization failed");
    d.messageHandler=Message;
}
Menu::~Menu()
{
    device.messageHandler=nullptr;
    // Every frame submission is synchronous. Backends release textures before Device.
    ImGui_ImplDX12_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); descriptors.available.clear();
}
void Menu::Begin()
{
    ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
}
void Menu::Render(Device &d)
{
    ImGui::Render(); ID3D12DescriptorHeap *heaps[]={d.heap.Get()}; d.list->SetDescriptorHeaps(1,heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),d.list.Get());
}
MenuActions Menu::Draw(Device &d, Scene &scene, Pathtrace &trace, Settings &s)
{
    MenuActions actions;
    unsigned triangle=NoLamp, group=NoLamp;
    if(trace.ReadPick(triangle,group) && group<scene.groups.size()) {
        selected=group; scrollToSelected=true; s.menu=true;
    }
    if(s.menu) {
        ImGui::SetNextWindowSize(ImVec2(420,680),ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(20,130),ImGuiCond_FirstUseEver);
        if(ImGui::Begin("Scene lab (F1)",&s.menu)) {
            if(ImGui::CollapsingHeader("Render",ImGuiTreeNodeFlags_DefaultOpen)) actions.changed=RenderControls(s.options);
            if(ImGui::CollapsingHeader("Animation",ImGuiTreeNodeFlags_DefaultOpen)) actions.changed|=AnimationControls(d,scene,s.options);
            if(scrollToSelected) ImGui::SetNextItemOpen(true);
            if(ImGui::CollapsingHeader("Lamps",ImGuiTreeNodeFlags_DefaultOpen)) actions.lamps=LampControls(scene);
            ImGui::Text("Accumulated %u / %s",trace.Accumulated(),
                s.options.accumulationInfinite ? "infinite" : std::to_string(s.options.accumulationFrames).c_str());
            actions.save=ImGui::Button("Save settings"); ImGui::SameLine();
            actions.reload=ImGui::Button("Reload settings"); ImGui::SameLine();
            actions.dump=ImGui::Button("Dump frame");
            ImGui::TextWrapped("RMB look; WASD / Q E move; wheel speed %.2f; Home reset; Ctrl+F5..F8 store, F5..F8 recall.",double(s.speed));
            if(!trace.Error().empty()) ImGui::TextWrapped("Upscaler error: %s",trace.Error().c_str());
            if(!status.empty()) ImGui::TextWrapped("%s",status.c_str());
        }
        ImGui::End();
    }
    if(s.overlay) {
        ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_Always); ImGui::SetNextWindowBgAlpha(0.65f);
        constexpr auto flags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("Timing overlay",nullptr,flags);
        ImGui::Text("%u x %u -> %u x %u | %s",trace.Width(),trace.Height(),d.width,d.height,s.options.upscaler.c_str());
        ImGui::Text("Path %.2f ms | Upscaler %.2f ms | GPU frame %.2f ms | Wall %.2f ms",d.lastTraceMs,d.lastUpscalerMs,d.lastFrameMs,d.wallFrameMs);
        ImGui::Text("Frame %.2f ms | %.1f fps | accumulated %u | F2 overlay",
            double(ImGui::GetIO().DeltaTime)*1000,double(ImGui::GetIO().Framerate),trace.Accumulated());
        ImGui::End();
        // The number one looks for, large and in the corner. With vsync on the presented rate stops at
        // the display's, so the rate the GPU could hold (from its frame time) is shown beside it.
        ImGui::SetNextWindowPos(ImVec2(float(d.width)-10,10),ImGuiCond_Always,ImVec2(1,0)); ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("Fps",nullptr,flags);
        ImGui::SetWindowFontScale(2.6f);
        ImGui::TextColored(ImVec4(0.45f,1.0f,0.45f,1),"%.0f fps",double(ImGui::GetIO().Framerate));
        ImGui::SetWindowFontScale(1.2f);
        if(d.lastFrameMs>0.01) ImGui::Text("GPU %.0f fps (%.1f ms)%s",1000.0/d.lastFrameMs,d.lastFrameMs,s.options.vsync ? ", vsync on" : "");
        ImGui::End();
    }
    if(!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto p=ImGui::GetIO().MousePos;
        if(p.x>=0 && p.y>=0 && p.x<float(d.width) && p.y<float(d.height)) trace.RequestPick(int(p.x),int(p.y));
    }
    return actions;
}
bool Menu::LampControls(Scene &scene)
{
    bool changed=false;
    ImGui::BeginChild("lamp groups",ImVec2(0,260),ImGuiChildFlags_Borders);
    for(unsigned i=0;i<scene.groups.size();++i) {
        auto &g=scene.groups[i]; ImGui::PushID(int(i));
        if(selected==i && scrollToSelected) { ImGui::SetScrollHereY(0.25f); scrollToSelected=false; }
        if(ImGui::Selectable(g.name.c_str(),selected==i)) selected=i;
        ImGui::Text("%u triangles | power %.3f",g.triangles,double(g.power));
        changed|=ImGui::SliderFloat("Intensity",&g.intensity,0,20,"%.3f",ImGuiSliderFlags_Logarithmic);
        changed|=ImGui::ColorEdit3("Tint",&g.tint.x);
        if(ImGui::Button("Solo")) { for(unsigned j=0;j<scene.groups.size();++j) scene.groups[j].intensity=j==i ? 1.0f : 0.0f; changed=true; }
        ImGui::SameLine();
        if(ImGui::Button("Reset")) { g.intensity=1; g.tint={1,1,1}; changed=true; }
        ImGui::Separator(); ImGui::PopID();
    }
    if(ImGui::Button("Reset all lamps")) { for(auto &g:scene.groups) { g.intensity=1; g.tint={1,1,1}; } changed=true; }
    ImGui::EndChild(); return changed;
}
