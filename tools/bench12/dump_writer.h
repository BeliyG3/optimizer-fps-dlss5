#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Encodes and writes the frame dumps (PNG/BMP, image.h) on worker threads, so the frames go on while a dump is encoded
// (WIC PNG of a 1080p frame takes about 70 ms, against a 23 ms frame). The files are the synchronous writer's byte for
// byte. Write blocks while `Capacity` dumps wait; Finish writes the rest and joins (before the process ends) and rethrows
// the first failure.
class DumpWriter {
public:
    static constexpr size_t Capacity=16;
    DumpWriter()=default;
    DumpWriter(const DumpWriter &)=delete;
    DumpWriter &operator=(const DumpWriter &)=delete;
    ~DumpWriter();
    void Write(std::filesystem::path path, unsigned width, unsigned height, std::vector<uint8_t> bgra, bool png);
    void Finish();
private:
    struct Job { std::filesystem::path path; unsigned width, height; std::vector<uint8_t> bgra; bool png; };
    void Work();
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::vector<std::thread> threads;
    std::string failure;
    bool stopping=false;
};
