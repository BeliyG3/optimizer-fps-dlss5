#pragma once
#include "device.h"
#include "hosts/optiscaler/tests/Sentinel.h"
#include <memory>

class CoreStateSentinel {
    std::unique_ptr<ofps::sentinel::Sentinel> fixture_;
    bool draw_ = false;
    bool rebind_ = false;
public:
    void Initialize(Device& d, const std::string& mode, bool rebind) {
        if (mode.empty()) return;
        if (mode != "draw" && mode != "compute") throw std::runtime_error("Invalid core state sentinel mode");
        draw_ = mode == "draw";
        rebind_ = rebind;
        fixture_ = std::make_unique<ofps::sentinel::Sentinel>(d.gpu.Get());
    }
    void Before(Device& d) {
        if (!fixture_) return;
        d.Wait(); fixture_->Verify();
        fixture_->Bind(d.list.Get(), draw_);
    }
    void After(Device& d) {
        if (!fixture_) return;
        if (rebind_) fixture_->Bind(d.list.Get(), draw_);
        fixture_->RecordCheck(d.list.Get());
    }
    void Finish(Device& d) {
        if (!fixture_) return;
        d.Wait(); fixture_->Verify(); fixture_.reset();
        std::puts("core state sentinel: PASS");
    }
};
