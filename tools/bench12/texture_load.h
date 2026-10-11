#pragma once
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Builds `count` items (a scene's textures) on a pool of workers, in any order, and hands each to `upload` in index order
// on the calling thread (the device's). At most `window` items wait beyond the next upload: a 4k image is 85 MB with its
// mips. A failure in `build` stops the workers and is rethrown here, after they are joined.
template <class T>
void BuildInOrder(unsigned count, unsigned workers, unsigned window, const std::function<T(unsigned)> &build,
                  const std::function<void(unsigned, T &)> &upload)
{
    std::vector<T> chains(count);
    std::vector<char> ready(count, 0);
    std::exception_ptr failure;
    std::mutex mutex; std::condition_variable changed;
    unsigned next = 0, uploaded = 0; bool stop = false;
    auto work = [&] {
        for (;;) {
            unsigned i;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stop || next >= count || next < uploaded + window; });
                if (stop || next >= count) return;
                i = next++;
            }
            T chain{}; std::exception_ptr error;
            try { chain = build(i); } catch (...) { error = std::current_exception(); }
            std::lock_guard lock(mutex);
            if (error) { if (!failure) failure = error; stop = true; }
            else { chains[i] = std::move(chain); ready[i] = 1; }
            changed.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < std::max(1u, workers); ++k) pool.emplace_back(work);
    auto finish = [&] {
        { std::lock_guard lock(mutex); stop = true; }
        changed.notify_all();
        for (auto &thread : pool) thread.join();
    };
    try {
        for (unsigned i = 0; i < count; ++i) {
            T chain{};
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return ready[i] || failure; });
                if (!ready[i]) break;
                chain = std::move(chains[i]);
            }
            upload(i, chain);
            { std::lock_guard lock(mutex); uploaded = i + 1; }
            changed.notify_all();
        }
    } catch (...) { finish(); throw; }
    finish();
    if (failure) std::rethrow_exception(failure);
}
