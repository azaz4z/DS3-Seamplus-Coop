#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

int wmain() {
    wchar_t resultPath[32768]{};
    wchar_t appId[64]{};
    if (!GetEnvironmentVariableW(L"DS3SC_TEST_RESULT", resultPath, 32768)) return 2;
    GetEnvironmentVariableW(L"SteamAppId", appId, 64);
    const bool ok = GetModuleHandleW(L"fixture_mod.dll") && wcscmp(appId, L"374320") == 0;
    HANDLE file = CreateFileW(resultPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    DWORD written = 0;
    const char* result = ok ? "PASS" : "FAIL";
    WriteFile(file, result, 4, &written, nullptr);
    CloseHandle(file);
    return ok ? 0 : 1;
}
