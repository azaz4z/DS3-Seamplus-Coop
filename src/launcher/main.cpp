#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr char kGameExecutable[] = "DarkSoulsIII.exe";
constexpr char kModRelativePath[] = "TheAshenLink\\ds3sc.dll";
constexpr char kModSeamplusRelativePath[] = "SeamplusCoop\\ds3sc.dll";
constexpr char kModLegacyRelativePath[] = "SeamlessCoop\\ds3sc.dll";
constexpr char kSteamAppId[] = "374320";
constexpr DWORD kInjectionTimeoutMs = 10'000;

class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        HANDLE result = value_;
        value_ = nullptr;
        return result;
    }
    void reset(HANDLE next = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = next;
    }

private:
    HANDLE value_ = nullptr;
};

class RemoteAllocation final {
public:
    RemoteAllocation(HANDLE process, SIZE_T size)
        : process_(process), address_(VirtualAllocEx(process, nullptr, size,
                                                     MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE)) {}
    ~RemoteAllocation() {
        if (address_) VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
    }

    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;
    [[nodiscard]] void* get() const noexcept { return address_; }
    [[nodiscard]] explicit operator bool() const noexcept { return address_ != nullptr; }
    void abandonUntilProcessExit() noexcept { address_ = nullptr; }

private:
    HANDLE process_;
    void* address_;
};

class SuspendedProcessGuard final {
public:
    explicit SuspendedProcessGuard(HANDLE process) : process_(process) {}
    ~SuspendedProcessGuard() {
        if (!resumed_) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, kInjectionTimeoutMs);
        }
    }
    void resumed() noexcept { resumed_ = true; }
    SuspendedProcessGuard(const SuspendedProcessGuard&) = delete;
    SuspendedProcessGuard& operator=(const SuspendedProcessGuard&) = delete;
private:
    HANDLE process_;
    bool resumed_ = false;
};

bool g_GuiEnabled = true;

void PrintError(const char* operation, DWORD error = GetLastError()) {
    char* message = nullptr;
    if (error != 0) {
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<char*>(&message), 0, nullptr);
    }
    char buffer[1024];
    if (error != 0) {
        std::snprintf(buffer, sizeof(buffer), "%s\n\nError = %lu%s%s", operation,
                     static_cast<unsigned long>(error), message ? ": " : "",
                     message ? message : "");
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s", operation);
    }
    std::fprintf(stderr, "%s\n", buffer);
    if (g_GuiEnabled) {
        MessageBoxA(nullptr, buffer, "The Ashen Link: DS3 Coop", MB_OK | MB_ICONERROR);
    }
    if (message) LocalFree(message);
}

void ShowMessage(const std::wstring& message, UINT icon = MB_ICONERROR) {
    std::fwprintf(stderr, L"%ls\n", message.c_str());
    if (g_GuiEnabled) {
        MessageBoxW(nullptr, message.c_str(), L"The Ashen Link: DS3 Coop", MB_OK | icon);
    }
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code ec1;
    std::error_code ec2;
    const auto a = std::filesystem::weakly_canonical(left, ec1).wstring();
    const auto b = std::filesystem::weakly_canonical(right, ec2).wstring();
    return !ec1 && !ec2 && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool GameAlreadyRunning() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) throw std::runtime_error("Could not check if DS3 is already running.");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"DarkSoulsIII.exe") == 0) return true;
        } while (Process32NextW(snapshot.get(), &entry));
    }
    return false;
}

std::filesystem::path DetectGameDirectory() {
    HKEY hKey = nullptr;
    const wchar_t* subKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 374320";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t installLocation[MAX_PATH] = {};
        DWORD size = sizeof(installLocation);
        if (RegQueryValueExW(hKey, L"InstallLocation", nullptr, nullptr, reinterpret_cast<LPBYTE>(installLocation), &size) == ERROR_SUCCESS) {
            std::filesystem::path p(installLocation);
            p /= "Game";
            std::error_code ec;
            if (std::filesystem::is_regular_file(p / kGameExecutable, ec) && !ec) {
                RegCloseKey(hKey);
                return p;
            }
        }
        RegCloseKey(hKey);
    }
    const std::filesystem::path defaultPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\DARK SOULS III\\Game";
    std::error_code ec;
    if (std::filesystem::is_regular_file(defaultPath / kGameExecutable, ec) && !ec) {
        return defaultPath;
    }
    return {};
}

bool ProcessContainsModule(HANDLE process, const std::filesystem::path& expected) {
    std::vector<HMODULE> modules(256);
    DWORD bytesNeeded = 0;

    for (;;) {
        if (!K32EnumProcessModules(process, modules.data(),
                                   static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                   &bytesNeeded)) {
            PrintError("Could not enumerate game modules");
            return false;
        }
        if (bytesNeeded <= modules.size() * sizeof(HMODULE)) break;
        modules.resize((bytesNeeded / sizeof(HMODULE)) + 16);
    }

    const size_t count = bytesNeeded / sizeof(HMODULE);
    std::array<wchar_t, 32768> path{};
    for (size_t i = 0; i < count; ++i) {
        const DWORD length = K32GetModuleFileNameExW(process, modules[i], path.data(),
                                                     static_cast<DWORD>(path.size()));
        if (length && length < path.size() && SamePath(std::filesystem::path(path.data(), path.data() + length),
                               expected)) {
            return true;
        }
    }
    return false;
}

bool InjectLibrary(HANDLE process, const std::filesystem::path& absoluteDllPath) {
    const std::wstring dllPath = absoluteDllPath.wstring();
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    RemoteAllocation remotePath(process, bytes);
    if (!remotePath) {
        PrintError("Could not allocate memory in DarkSoulsIII.exe");
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath.get(), dllPath.c_str(), bytes, &written) ||
        written != bytes) {
        PrintError("Could not write DLL path into target process");
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
    if (!loadLibrary) {
        PrintError("Could not resolve LoadLibraryW");
        return false;
    }

    Handle remoteThread(CreateRemoteThread(process, nullptr, 0, loadLibrary,
                                           remotePath.get(), 0, nullptr));
    if (!remoteThread) {
        PrintError("Could not create injection thread");
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread.get(), kInjectionTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        remotePath.abandonUntilProcessExit();
        PrintError("Injection did not complete within the timeout",
                   waitResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
        return false;
    }

    if (!ProcessContainsModule(process, absoluteDllPath)) {
        std::fprintf(stderr, "The DLL does not appear in the game module list.\n");
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) try {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) {
        PrintError("Could not determine launcher directory");
        return 1;
    }
    const auto launcherDir = std::filesystem::path(executable.data()).parent_path();
    auto directory = launcherDir;
    std::filesystem::path modPath = kModRelativePath;
    bool checkOnly = false;
    bool enableCompanion = true;
    bool explicitDirectory = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--game-dir" && i + 1 < argc) { directory = argv[++i]; explicitDirectory = true; }
        else if (argument == L"--dll" && i + 1 < argc) modPath = argv[++i];
        else if (argument == L"--check") { checkOnly = true; g_GuiEnabled = false; }
        else if (argument == L"--no-companion") enableCompanion = false;
        else if (argument == L"--no-gui") g_GuiEnabled = false;
        else if (argument == L"--help") {
            std::puts("Usage: TheAshenLink [--game-dir DIR] [--dll PATH] [--check] [--no-gui] [--with-companion]");
            return 0;
        } else {
            ShowMessage(L"Unknown or incomplete argument. See --help.");
            return 1;
        }
    }
    std::error_code ec;
    directory = std::filesystem::absolute(directory);
    auto gamePath = directory / kGameExecutable;
    if (!std::filesystem::is_regular_file(gamePath, ec) || ec) {
        const auto detected = explicitDirectory ? std::filesystem::path{} : DetectGameDirectory();
        if (!detected.empty()) {
            directory = detected;
            gamePath = directory / kGameExecutable;
        }
    }

    if (!std::filesystem::is_regular_file(gamePath, ec) || ec) {
        ShowMessage(L"\"DarkSoulsIII.exe\" not found.\n\n"
                    L"Please make sure Dark Souls III is installed on Steam "
                    L"or copy the launcher to the game's 'Game' folder:\n"
                    L"(for example: C:\\Program Files (x86)\\Steam\\steamapps\\common\\DARK SOULS III\\Game)");
        return 1;
    }

    auto dllPath = modPath.is_absolute() ? modPath : directory / modPath;
    ec.clear();
    if (!std::filesystem::is_regular_file(dllPath, ec)) {
        auto seamplusPath = directory / kModSeamplusRelativePath;
        if (std::filesystem::is_regular_file(seamplusPath, ec) && !ec) {
            dllPath = seamplusPath;
        } else {
            auto legacyPath = directory / kModLegacyRelativePath;
            if (std::filesystem::is_regular_file(legacyPath, ec) && !ec) {
                dllPath = legacyPath;
            }
        }
    }

    ec.clear();
    if (!checkOnly && (!std::filesystem::is_regular_file(dllPath, ec) || ec)) {
        auto localCoop = launcherDir / "TheAshenLink";
        std::string targetFolderName = "TheAshenLink";
        if (!std::filesystem::is_regular_file(localCoop / "ds3sc.dll", ec)) {
            localCoop = launcherDir / "SeamplusCoop";
            targetFolderName = "SeamplusCoop";
        }
        if (!std::filesystem::is_regular_file(localCoop / "ds3sc.dll", ec)) {
            localCoop = launcherDir / "SeamlessCoop";
            targetFolderName = "SeamlessCoop";
        }
        const auto localDll = localCoop / "ds3sc.dll";
        if (std::filesystem::is_regular_file(localDll, ec) && !ec) {
            std::error_code copyEc;
            std::filesystem::copy(localCoop, directory / targetFolderName,
                                  std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                                  copyEc);
            if (copyEc) {
                ShowMessage(L"Could not install " + std::wstring(targetFolderName.begin(), targetFolderName.end()) + L" into the Game folder.");
                return 1;
            }
        }
        dllPath = directory / (targetFolderName + "\\ds3sc.dll");
    }

    ec.clear();
    if (!std::filesystem::is_regular_file(dllPath, ec) || ec) {
        std::wstring msg = L"Mod DLL not found:\n" + dllPath.wstring() +
                           L"\n\nMake sure the 'TheAshenLink' folder containing ds3sc.dll "
                           L"is copied inside the Dark Souls III 'Game' folder.";
        ShowMessage(msg);
        return 1;
    }
    if (checkOnly) {
        std::fwprintf(stdout, L"Game: %ls\nDLL: %ls\nFiles present; game was not started.\n",
                      gamePath.c_str(), dllPath.c_str());
        return 0;
    }

    Handle launchLock(CreateMutexW(nullptr, TRUE, L"Local\\DS3SC-Launcher-Startup"));
    const DWORD lockError = GetLastError();
    if (!launchLock || lockError == ERROR_ALREADY_EXISTS || GameAlreadyRunning()) {
        ShowMessage(L"Dark Souls III is already running or starting. Close the game before launching the mod again.");
        return 1;
    }

    if (!SetEnvironmentVariableA("SteamAppId", kSteamAppId)) {
        PrintError("Could not set SteamAppId");
        return 1;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION rawProcess{};
    if (!CreateProcessW(gamePath.c_str(), nullptr, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | DETACHED_PROCESS, nullptr, directory.c_str(),
                        &startup, &rawProcess)) {
        PrintError("Could not start DarkSoulsIII.exe");
        return 1;
    }

    Handle process(rawProcess.hProcess);
    Handle mainThread(rawProcess.hThread);
    SuspendedProcessGuard startupGuard(process.get());

    if (enableCompanion) {
        const auto companionDll = dllPath.parent_path() / "ds3sc_companion.dll";
        if (std::filesystem::is_regular_file(companionDll)) {
            if (!InjectLibrary(process.get(), companionDll)) {
                ShowMessage(L"Could not inject companion DLL (ds3sc_companion.dll) into DarkSoulsIII.exe.\n\n"
                            L"Check that your antivirus is not blocking the injection.");
                return 2;
            }
        }
    }

    if (!InjectLibrary(process.get(), dllPath)) {
        ShowMessage(L"Could not inject mod DLL (ds3sc.dll) into DarkSoulsIII.exe.\n\n"
                    L"Check that your antivirus is not blocking the injection and that "
                    L"Steam is running.");
        return 1;
    }

    // Give extension worker a brief moment to install hooks while main thread is suspended
    Sleep(50);

    if (ResumeThread(mainThread.get()) == static_cast<DWORD>(-1)) {
        PrintError("Could not resume game main thread");
        return 1;
    }
    startupGuard.resumed();
    if (WaitForSingleObject(process.get(), 1500) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.get(), &exitCode);
        if (exitCode == 0) return 0;
        PrintError("DS3 closed during startup", exitCode);
        return 1;
    }
    std::printf("DS3 started (PID %lu).\n", rawProcess.dwProcessId);
    return 0;
} catch (const std::exception& error) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "Launcher error: %s", error.what());
    std::fprintf(stderr, "%s\n", buffer);
    if (g_GuiEnabled) {
        MessageBoxA(nullptr, buffer, "The Ashen Link: DS3 Coop", MB_OK | MB_ICONERROR);
    }
    return 1;
}
