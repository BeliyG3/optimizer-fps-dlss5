#include "flight.h"
#include "camera_angles.h"
#include "external/imgui/imgui.h"

Flight::~Flight() { ReleaseMouse(); }
void Flight::ReleaseMouse()
{
    if(!looking) return;
    ClipCursor(nullptr); ReleaseCapture(); SetCursorPos(restore.x,restore.y); SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)));
    if(ImGui::GetCurrentContext()) ImGui::GetIO().ConfigFlags&=~ImGuiConfigFlags_NoMouseCursorChange;
    looking=false;
}
bool Flight::Update(Device &d, Settings &s, const CameraState &home)
{
    auto &io=ImGui::GetIO(); CameraState old=s.pose;
    const bool focused=GetForegroundWindow()==d.Window();
    if(!focused) { ReleaseMouse(); return false; }
    if(!io.WantCaptureKeyboard) {
        if(ImGui::IsKeyPressed(ImGuiKey_F1,false)) s.menu=!s.menu;
        if(ImGui::IsKeyPressed(ImGuiKey_F2,false)) s.overlay=!s.overlay;
        if(ImGui::IsKeyPressed(ImGuiKey_Home,false)) s.pose=home;
        for(int i=0;i<4;++i) if(ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_F5+i),false)) {
            if(io.KeyCtrl) s.bookmarks[size_t(i)]={true,s.pose};
            else if(s.bookmarks[size_t(i)].valid) s.pose=s.bookmarks[size_t(i)].pose;
        }
    }
    if(!io.WantCaptureMouse) s.speed=std::clamp(s.speed*std::pow(1.2f,io.MouseWheel),0.01f,1000.0f);
    bool right=ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if(!right || io.WantCaptureMouse) ReleaseMouse();
    else {
        RECT rect{}; GetClientRect(d.Window(),&rect); POINT corner{rect.left,rect.top}, opposite{rect.right,rect.bottom};
        ClientToScreen(d.Window(),&corner); ClientToScreen(d.Window(),&opposite);
        POINT centre{(corner.x+opposite.x)/2,(corner.y+opposite.y)/2};
        if(!looking) { GetCursorPos(&restore); looking=true; SetCapture(d.Window()); SetCursorPos(centre.x,centre.y); }
        POINT mouse{}; GetCursorPos(&mouse);
        float yaw=0,pitch=0; DirectionAngles(s.pose.target-s.pose.eye,yaw,pitch);
        yaw+=float(mouse.x-centre.x)*0.12f; pitch=std::clamp(pitch-float(mouse.y-centre.y)*0.12f,-89.0f,89.0f);
        s.pose.target=s.pose.eye+DirectionFromAngles(yaw,pitch);
        RECT clip{corner.x,corner.y,opposite.x,opposite.y}; ClipCursor(&clip);
        io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange; SetCursor(nullptr); SetCursorPos(centre.x,centre.y);
    }
    if(!io.WantCaptureKeyboard) {
        auto axis=[](ImGuiKey positive,ImGuiKey negative) { return float(ImGui::IsKeyDown(positive))-float(ImGui::IsKeyDown(negative)); };
        Vec3 forward=Normalize(s.pose.target-s.pose.eye), rightAxis=Normalize(Cross({0,1,0},forward));
        Vec3 movement=forward*axis(ImGuiKey_W,ImGuiKey_S)+rightAxis*axis(ImGuiKey_D,ImGuiKey_A)+Vec3{0,axis(ImGuiKey_E,ImGuiKey_Q),0};
        float step=s.speed*std::min(io.DeltaTime,0.1f)*(io.KeyShift ? 4 : 1)*(io.KeyCtrl ? 0.25f : 1);
        s.pose.eye=s.pose.eye+movement*step; s.pose.target=s.pose.target+movement*step;
    }
    s.hasPose=true; return CameraMoved(old,s.pose);
}
