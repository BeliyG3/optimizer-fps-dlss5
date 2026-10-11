#include "dump_writer.h"
#include "image.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

DumpWriter::~DumpWriter()
{
    try { Finish(); } catch(const std::exception &error) { std::fprintf(stderr,"[fail] %s\n",error.what()); }
}
void DumpWriter::Write(std::filesystem::path path, unsigned width, unsigned height, std::vector<uint8_t> bgra, bool png)
{
    std::unique_lock lock(mutex);
    if(!failure.empty()) throw std::runtime_error(failure);
    if(threads.empty()) {
        stopping=false;
        const unsigned count=std::clamp(std::thread::hardware_concurrency()/2,1u,8u);
        for(unsigned k=0;k<count;++k) threads.emplace_back([this] { Work(); });
    }
    changed.wait(lock,[&] { return jobs.size()<Capacity || !failure.empty(); });
    if(!failure.empty()) throw std::runtime_error(failure);
    jobs.push_back({std::move(path),width,height,std::move(bgra),png});
    changed.notify_all();
}
void DumpWriter::Finish()
{
    { std::lock_guard lock(mutex); stopping=true; }
    changed.notify_all();
    for(auto &thread:threads) thread.join();
    threads.clear();
    std::lock_guard lock(mutex);
    if(!failure.empty()) { const std::string error=failure; failure.clear(); throw std::runtime_error(error); }
}
void DumpWriter::Work()
{
    const bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)); // WIC's encoder, as on the main thread
    for(;;) {
        Job job;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock,[&] { return stopping || !jobs.empty(); });
            if(jobs.empty()) break;
            job=std::move(jobs.front()); jobs.pop_front();
            changed.notify_all();
        }
        try {
            if(job.png) WritePng(job.path,job.width,job.height,job.bgra.data()); else WriteBmp(job.path,job.width,job.height,job.bgra.data());
            std::printf("[info] wrote %s\n",job.path.string().c_str());
        } catch(const std::exception &error) {
            std::lock_guard lock(mutex);
            if(failure.empty()) failure=job.path.string()+": "+error.what();
            changed.notify_all();
        }
    }
    if(com) CoUninitialize();
}
