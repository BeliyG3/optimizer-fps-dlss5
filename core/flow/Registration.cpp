// The textures' registration with the optical flow engine and its ordering against the host's lists.
//
// A registration is GPU work on the engine's own queue that touches the texture (the debug layer sees
// write|transition_barrier). The host's first capture writes the luminance images in a list on the host's queue, so
// that queue must wait for the registrations; otherwise the two race ("still referenced by write|transition_barrier
// GPU operations in-flight on another Command Queue"). The wait is on the GPU: Acquire can run on a present thread.
#include "core/flow/OpticalFlowInternal.h"
#include "core/gpu/queues.h"
#include "core/log.h"

#include <mutex>
#include <vector>

namespace ofps::core::flow
{
namespace
{
std::mutex g_quarantineMutex;
bool g_quarantineLogged = false;
} // namespace

std::vector<std::unique_ptr<Session::Impl>>& Session::Impl::Quarantine()
{
    static auto* quarantine = new std::vector<std::unique_ptr<Impl>>(); // never destroyed, as the sessions
    return *quarantine;
}

bool Session::Impl::Register(ID3D12Resource* resource, NvOFGPUBufferHandle* buffer)
{
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params = {};
    params.resource = resource;
    params.hOFGpuBuffer = buffer;
    // Input: a value the fence already holds (a Wait on 0 is flagged by the debug layer). Output: a fresh ticket.
    params.inputFencePoint = { fenceIn.Get(), kInputReady };
    params.outputFencePoint = { fenceOut.Get(), ++ticket };
    if (api.nvOFRegisterResourceD3D12(handle, &params) != NV_OF_SUCCESS)
        return false;
    registered = ticket;
    return true;
}

bool Session::Impl::Settled() const
{
    if (ticket == 0 || !fenceOut)
        return true; // nothing was handed to the engine's queue
    if (device && FAILED(device->GetDeviceRemovedReason()))
        return false;
    const UINT64 done = fenceOut->GetCompletedValue();
    return done != UINT64_MAX && done >= ticket;
}

void Session::Impl::Retire(std::unique_ptr<Impl> impl)
{
    if (!impl || impl->Settled())
        return; // destroyed here: no engine work on its objects is pending
    std::lock_guard<std::mutex> lock(g_quarantineMutex);
    if (!g_quarantineLogged)
    {
        g_quarantineLogged = true;
        Log(true, "Optimizer FPS optical flow: a failed session's registration work is still pending on the engine's "
                  "queue; its objects are kept until it completes (for the process if it never does) (logged once)");
    }
    Quarantine().push_back(std::move(impl));
}

void Session::Impl::Sweep()
{
    std::vector<std::unique_ptr<Impl>> settled;
    {
        std::lock_guard<std::mutex> lock(g_quarantineMutex);
        auto& quarantine = Quarantine();
        for (auto it = quarantine.begin(); it != quarantine.end();)
        {
            if ((*it)->Settled())
            {
                settled.push_back(std::move(*it));
                it = quarantine.erase(it);
            }
            else
                ++it;
        }
    } // destroyed outside the lock
}

bool Session::OrderAfterRegistration(ID3D12Device* device, ID3D12Device* proxyDevice)
{
    Impl& m = *_impl;
    if (FAILED(m.device->GetDeviceRemovedReason()))
        return false;
    const UINT64 done = m.fenceOut->GetCompletedValue();
    if (done == UINT64_MAX)
        return false;
    if (done >= m.registered)
        return true;
    return gpu::WaitEveryRegisteredQueue(device, proxyDevice, m.fenceOut.Get(), m.registered);
}
} // namespace ofps::core::flow
