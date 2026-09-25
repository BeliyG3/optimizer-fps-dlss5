// Deliberately has no exports: exercises rejection of an unrelated DLL.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
