#include "ngx_nr_release_thread.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

void NrReleaseThread::Start(Device &d, const NrRuntime &runtime, NVSDK_NGX_Handle *handle, int frame)
{
    if(Running()) throw std::runtime_error("--nr-release-thread: a release is already under way");
    presenting=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!presenting) throw std::runtime_error("--nr-release-thread: CreateEvent failed");
    done=false; startFrame=frame;
    d.presentingContext=presenting;
    d.onPresenting=[](void *event) { SetEvent(static_cast<HANDLE>(event)); };
    thread=std::thread([this,&runtime,handle] {
        WaitForSingleObject(presenting,INFINITE); // set by the next present, or by Join
        const auto t0=std::chrono::steady_clock::now();
        result=runtime.Release(handle);
        milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        done=true;
    });
}
NVSDK_NGX_Result NrReleaseThread::Finish(Device &d, int frame)
{
    Join();
    d.onPresenting=nullptr; d.presentingContext=nullptr;
    std::printf("[nr] frame %d: feature 18 released on a second thread during that frame's present (result 0x%08X, %.3f ms in the call); joined on frame %d, created again below\n",
        startFrame,unsigned(result),milliseconds,frame);
    return result;
}
void NrReleaseThread::Join()
{
    if(!thread.joinable()) return;
    SetEvent(presenting); // a present that never comes (an exception) must not keep the thread waiting
    thread.join();
    CloseHandle(presenting); presenting=nullptr;
}
