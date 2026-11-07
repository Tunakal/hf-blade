// Smoke test: loads build\dinput8.dll into a non-game x86 process.
// Expected: no patches (wrong exe is rejected), DirectInput8Create forwards to the system DLL.
#include <windows.h>
#include <stdio.h>

static const GUID IID_IDirectInput8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
typedef HRESULT (WINAPI* Create)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);

int main(int argc, char** argv)
{
    HMODULE h = LoadLibraryA(argc > 1 ? argv[1] : "build\\dinput8.dll");
    if (!h) { printf("FAIL load %lu\n", GetLastError()); return 1; }
    Create fn = (Create)GetProcAddress(h, "DirectInput8Create");
    if (!fn) { printf("FAIL export\n"); return 1; }
    void* di = nullptr;
    HRESULT hr = fn(GetModuleHandleA(nullptr), 0x0800, IID_IDirectInput8A, &di, nullptr);
    printf("DirectInput8Create hr=0x%08lX iface=%p\n", (unsigned long)hr, di);
    fflush(stdout);
    if (SUCCEEDED(hr) && di) ((ULONG (WINAPI*)(void*))(*(void***)di)[2])(di); // IUnknown::Release
    return SUCCEEDED(hr) ? 0 : 1;
}
