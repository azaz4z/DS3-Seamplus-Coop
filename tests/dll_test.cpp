#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    for (int i = 0; i < 20; ++i) {
        HMODULE module = LoadLibraryW(argv[1]);
        if (!module) return 3;
        auto status = reinterpret_cast<int (*)()>(GetProcAddress(module, "ds3sc_reconstructed_status"));
        auto modengine = reinterpret_cast<bool (*)(void*, void**)>(GetProcAddress(module, "modengine_ext_init"));
        if (!status || !modengine) return 4;
        void* extension = reinterpret_cast<void*>(1);
        if (modengine(nullptr, &extension) || extension != nullptr) return 5;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (status() < 2 && GetTickCount64() < deadline) Sleep(1);
        if (status() != 2) return 6;
        FreeLibrary(module);
    }
    // Immediate unloading exercises the worker's owned module reference.
    for (int i = 0; i < 50; ++i) {
        HMODULE module = LoadLibraryW(argv[1]);
        if (!module) return 7;
        FreeLibrary(module);
    }
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetModuleHandleW(argv[1]) && GetTickCount64() < deadline) Sleep(1);
    if (GetModuleHandleW(argv[1])) return 8;
    std::cout << "DLL lifecycle and ModEngine failure contract tests passed\n";
    return 0;
}
