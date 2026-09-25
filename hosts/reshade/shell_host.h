#pragma once
#include "hosts/reshade/ngx_params.h"
#include "core/api/ofps_core.h"
namespace ofps::reshade {
struct ShellStatus {
    ShellFrameInfo frame;
    int lastNgxResult = kNgxSuccess;
};
ShellStatus &ShellState();
bool HasShellFeatures();
class ShellHost final : public IOfpsHost {
  public:
    void Log(OfpsLogLevel level, const char *text) override;
    void OnEvent(OfpsEvent kind, const OfpsEventData *data) override;
};
ShellHost &Host();
IOfpsCore *Core();
int CoreAttach();
const char *CoreLoadError();
bool CoreDetach(bool serialize = true);
bool ShellDeviceRemoved();
} // namespace ofps::reshade
