#include <windows.h>

// Self-owned relocation fixture. This DLL has no game dependencies or hooks.
extern "C" {
__declspec(dllexport) int xess_aslr_anchor = 0x12345678;
__declspec(dllexport) int* xess_aslr_pointer = &xess_aslr_anchor;
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
}
