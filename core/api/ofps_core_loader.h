#pragma once
#include "core/api/ofps_core.h"
#include "core/temporal/diagnostic_keys.h"
#include "core/flow/motion_source.h"
#include <atomic>
#include <filesystem>
#include <string>
namespace ofps {
class CoreLoader final {
public:
    CoreLoader() = default;
    CoreLoader(const CoreLoader &) = delete;
    CoreLoader &operator=(const CoreLoader &) = delete;
    int Attach(const std::filesystem::path &hostFile, const char *hostName, IOfpsHost *host);
    bool Detach(bool serialize = true);
    IOfpsCore *Get() const { return core_.load(); }
    ofps::temporal::SetTemporalDiagnosticsV1 TemporalDiagnostics() const { return temporalDiagnostics_; }
    ofps::core::flow::SetTemporalMotionSourceV1 TemporalMotionSource() const { return temporalMotionSource_; }
    const std::string &Error() const { return error_; }
private:
    std::atomic<IOfpsCore *> core_{nullptr};
    IOfpsHost *host_ = nullptr;
    ofps::temporal::SetTemporalDiagnosticsV1 temporalDiagnostics_ = nullptr;
    ofps::core::flow::SetTemporalMotionSourceV1 temporalMotionSource_ = nullptr;
    std::string error_;
};
} // namespace ofps
