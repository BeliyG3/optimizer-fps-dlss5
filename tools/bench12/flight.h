#pragma once
#include "settings.h"
#include "device.h"

class Flight {
public:
    ~Flight();
    bool Update(Device &device, Settings &settings, const CameraState &home);
    void ReleaseMouse();
private:
    bool looking=false;
    POINT restore{};
};
