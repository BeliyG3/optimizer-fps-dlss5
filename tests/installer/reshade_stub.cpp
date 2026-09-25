// A PE fixture for installer file checks. Never load it as ReShade.
#if !defined(_WIN32)
#error This fixture is Windows-only.
#endif

extern "C" __declspec(dllexport) void ReShadeRegisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon() {}
