#pragma once
#include "pathtrace.h"
#include "settings.h"

struct MenuActions { bool changed=false, lamps=false, save=false, reload=false, dump=false; };
class Menu {
public:
    explicit Menu(Device &device);
    ~Menu();
    void Begin();
    MenuActions Draw(Device &device, Scene &scene, Pathtrace &trace, Settings &settings);
    static void Render(Device &device);
    std::string status;
private:
    Device &device;
    unsigned selected=NoLamp;
    bool scrollToSelected=false;
    bool RenderControls(Options &options);
    bool LampControls(Scene &scene);
    bool AnimationControls(Device &device, Scene &scene, Options &options);
};
