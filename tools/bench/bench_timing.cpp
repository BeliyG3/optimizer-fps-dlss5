#include "bench_timing.h"

#include "bench_d3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

void PaceFrame(double fpsCap, double fpsJitterMs)
{
    static LARGE_INTEGER capStart{}; static LARGE_INTEGER capFreq{};
    if (capFreq.QuadPart == 0) QueryPerformanceFrequency(&capFreq);
    long long budget = (long long) (capFreq.QuadPart / fpsCap);
    if (fpsJitterMs > 0.0) budget += (long long) (capFreq.QuadPart * (fpsJitterMs * (rand() / (double) RAND_MAX)) / 1000.0);
    if (capStart.QuadPart != 0) { LARGE_INTEGER now; do { QueryPerformanceCounter(&now); } while (now.QuadPart - capStart.QuadPart < budget); capStart.QuadPart += budget; if (now.QuadPart - capStart.QuadPart > budget) capStart = now; }
    else QueryPerformanceCounter(&capStart);
}

void RecordFrameTime(int frame, int frames, bool frametime)
{
    static LARGE_INTEGER lastPresent{}; static std::vector<double> intervals; static LARGE_INTEGER qpf{};
    if (qpf.QuadPart == 0) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (lastPresent.QuadPart != 0 && frame >= 60) intervals.push_back((double) (now.QuadPart - lastPresent.QuadPart) * 1000.0 / (double) qpf.QuadPart);
    lastPresent = now;
    if (frametime && frame == frames - 1 && !intervals.empty()) {
        double sum = 0, mn = 1e9, mx = 0; for (double v : intervals) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
        const double avg = sum / intervals.size(); double var = 0; int slow = 0;
        for (double v : intervals) { var += (v - avg) * (v - avg); if (v > 1.5 * avg) ++slow; }
        std::vector<double> sorted = intervals; std::sort(sorted.begin(), sorted.end());
        std::printf("[frametime] frames %zu: avg %.2f ms (%.1f fps), min %.2f, max %.2f, p99 %.2f, stddev %.2f ms (%.0f%% of avg), frames > 1.5x avg: %.1f%%\n",
                    intervals.size(), avg, 1000.0 / avg, mn, mx, sorted[(size_t) (sorted.size() * 0.99)], std::sqrt(var / intervals.size()),
                    100.0 * std::sqrt(var / intervals.size()) / avg, 100.0 * slow / intervals.size());
    }
}
