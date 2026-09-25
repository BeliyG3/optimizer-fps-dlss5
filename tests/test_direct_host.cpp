#include "hosts/reshade/direct_host.h"
#include <cstdio>
int main() {
    unsigned failures = 0;
    auto check = [&](bool ok) { if (!ok) ++failures; };
    wchar_t name[96];
    swprintf_s(name, L"Local\\OptimizerFpsDirectHost_%lu", GetCurrentProcessId());
    {
        ofps::reshade::DirectHostProbe throttled(~ULONGLONG{0});
        check(!throttled.Active());
        ofps::reshade::DirectHostProbe probe(0); // Disable retry delay without sleeping.
        check(!probe.Active());
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name);
        if (!event) return 2;
        check(!probe.Active());
        check(SetEvent(event) != FALSE);
        check(!throttled.Active()); // Cached absence; no timed sleep needed.
        check(probe.Active());
        check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0); // Probe preserves the signal.
        check(ResetEvent(event) != FALSE);
        check(!probe.Active()); // Reset ends direct-host ownership.
        check(CloseHandle(event) != FALSE);
        check(!probe.Active());
    }
    ofps::reshade::DirectHostProbe fresh;
    check(!fresh.Active()); // All handles closed; a new probe starts unlatched.
    // Production Detours use a zero-delay probe: a late event must take effect
    // on the very next evaluate, even if create was observed before the event.
    check(!ofps::reshade::DirectHostActive());
    HANDLE late = CreateEventW(nullptr, TRUE, FALSE, name);
    if (!late) return 2;
    check(!ofps::reshade::DirectHostActive());
    check(SetEvent(late) != FALSE);
    check(ofps::reshade::DirectHostActive());
    check(WaitForSingleObject(late, 0) == WAIT_OBJECT_0);
    check(ResetEvent(late) != FALSE);
    check(!ofps::reshade::DirectHostActive());
    check(CloseHandle(late) != FALSE);
    HANDLE restarted = CreateEventW(nullptr, TRUE, TRUE, name);
    if (!restarted) return 2;
    check(ofps::reshade::DirectHostActive()); // A new event generation is visible.
    check(CloseHandle(restarted) != FALSE);
    std::puts(failures ? "direct host probe failed" : "direct host probe passed");
    return failures ? 1 : 0;
}
