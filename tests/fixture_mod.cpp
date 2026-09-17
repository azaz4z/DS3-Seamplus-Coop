#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t mode[32]{};
        GetEnvironmentVariableW(L"DS3SC_TEST_MODE", mode, 32);
        if (wcscmp(mode, L"reject") == 0) return FALSE;
        if (wcscmp(mode, L"timeout") == 0) Sleep(15000);
    }
    return TRUE;
}
