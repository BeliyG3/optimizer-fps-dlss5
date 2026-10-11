#pragma once
#include <windows.h>
#include <cstdio>
#include <vector>

// --startup-profile: the wall time of the run's phases, from the process's creation (the loader, ReShade's dxgi.dll)
// to the end of the frames, printed as a table at the end. Mark(phase) closes the phase that ends now; Add(phase, s)
// books time measured elsewhere (inside a phase already marked, e.g. the dumps' share of the frames).
class StartupProfile {
public:
    static StartupProfile &Get() { static StartupProfile profile; return profile; }
    void Mark(const char *phase) { marks.push_back({phase,Now()}); }
    void Add(const char *phase, double seconds) { extra.push_back({phase,seconds}); }
    double Now() const
    {
        FILETIME now{}; GetSystemTimePreciseAsFileTime(&now);
        return double(Ticks(now)-created)*1e-7;
    }
    void Print() const
    {
        std::printf("[startup] %-58s %8s %8s\n","phase (from the process's creation)","s","end s");
        double last=0;
        for(const auto &m:marks) { std::printf("[startup] %-58s %8.3f %8.3f\n",m.phase,m.seconds-last,m.seconds); last=m.seconds; }
        for(const auto &e:extra) std::printf("[startup]   of which %-49s %8.3f\n",e.phase,e.seconds);
    }
private:
    struct Entry { const char *phase; double seconds; };
    static unsigned long long Ticks(FILETIME t) { return (unsigned long long)t.dwHighDateTime<<32 | t.dwLowDateTime; }
    StartupProfile()
    {
        FILETIME creation{},exit{},kernel{},user{};
        GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user);
        created=Ticks(creation);
    }
    unsigned long long created=0;
    std::vector<Entry> marks, extra;
};
inline void StartupMark(const char *phase) { StartupProfile::Get().Mark(phase); }
