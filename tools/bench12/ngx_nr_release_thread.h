#pragma once
// --nr-release-thread (menu mode's correction C4): the release of --nr-recreate runs on a second thread that starts
// while that frame's present is under way, where the Optimizer FPS add-on runs its menu pass (and, with Mode
// Uniform/Peripheral, its call into the core's feature). The release so races the menu call. Until the thread is
// joined the bench neither evaluates nor creates feature 18.
#include "ngx_nr_runtime.h"
#include <atomic>
#include <thread>

class NrReleaseThread {
public:
    NrReleaseThread()=default;
    ~NrReleaseThread() { Join(); }
    NrReleaseThread(const NrReleaseThread &)=delete;
    NrReleaseThread &operator=(const NrReleaseThread &)=delete;
    // The thread waits for the device's next present (Device::onPresenting), then releases `handle`.
    void Start(Device &device, const NrRuntime &runtime, NVSDK_NGX_Handle *handle, int frame);
    bool Running() const { return thread.joinable(); }
    bool Done() const { return done.load(); }
    // Joins, detaches the present hook and prints the release line (as --nr-recreate does, with the start frame).
    NVSDK_NGX_Result Finish(Device &device, int frame);
private:
    void Join();
    std::thread thread;
    std::atomic<bool> done{false};
    HANDLE presenting=nullptr;
    NVSDK_NGX_Result result=NVSDK_NGX_Result_Success;
    double milliseconds=0;
    int startFrame=-1;
};
