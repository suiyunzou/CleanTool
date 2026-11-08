#include "storage.h"
#include <cstdio>
#include <stdexcept>

namespace storage {
static int dispatch(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--auto-temp")) return autoTemp();
    if (argc >= 2 && (!wcscmp(argv[1], L"--audit") || !wcscmp(argv[1], L"--data-path"))) {
        bool readOnly = false;
        std::wstring data;
        for (int i = 1; i < argc; ++i) {
            if (!wcscmp(argv[i], L"--audit")) readOnly = true;
            else if (!wcscmp(argv[i], L"--data-path") && i + 1 < argc) data = argv[++i];
            else return 2;
        }
        if (readOnly) return audit(data);
        argc = 1;
    }
    if (argc == 7 && !wcscmp(argv[1], L"--worker")) {
        int op = _wtoi(argv[2]);
        if (op < 0 || op > 3 || !isSession(argv[3])) return 2;
        Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[4]));
        Handle cancel(OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(argv[4]) + L"-cancel").c_str()));
        if (!mapping || !cancel) return 2;
        auto shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!shared) return 2;
        int result = worker(static_cast<Operation>(op), argv[3], *shared, cancel, !wcscmp(argv[5], L"1"), wcstoul(argv[6], nullptr, 10));
        UnmapViewOfFile(shared);
        return result;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--scan-rule-test")) return scanRuleTest(argv[2]);
#ifndef STORAGE_WORKER
    if (argc == 3 && !wcscmp(argv[1], L"--navigation-ui-test")) return runUi(false, true, argv[2], true);
    if (argc == 3 && !wcscmp(argv[1], L"--self-test")) return selfTest(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--hidden-ui-test")) return runUi(true, true, argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--profile-readonly")) return runUi(false, true, argv[2]);
    if (argc == 2 && !wcscmp(argv[1], L"--stall-test")) { Sleep(INFINITE); return 0; }
    if (argc == 3 && !wcscmp(argv[1], L"--spawn-stall-test")) {
        auto line = quote(executable()) + L" --stall-test";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION pi{};
        auto exe = executable();
        if (!CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &pi)) return 1;
        Handle process(pi.hProcess), thread(pi.hThread);
        FILE* file = nullptr;
        if (_wfopen_s(&file, argv[2], L"wb")) return 1;
        fprintf(file, "%lu", pi.dwProcessId); fclose(file);
        Sleep(INFINITE); return 0;
    }
    bool demo = argc == 2 && (!_wcsicmp(argv[1], L"--demo") || !_wcsicmp(argv[1], L"-Demo"));
    bool scanOnOpen = argc == 2 && !wcscmp(argv[1], L"--scan");
    if (argc != 1 && !demo && !scanOnOpen) return 2;
    std::wstring mutexName = demo ? L"Local\\CleanTool.Native.Demo" : elevated() ? L"Local\\CleanTool.Native.Admin" : L"Local\\CleanTool.Native";
    Handle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(demo ? L"CleanTool.Native.Demo" : elevated() ? L"CleanTool.Native.Admin" : L"CleanTool.Native", nullptr);
        if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        return 0;
    }
    return runUi(demo);
#else
    return 2;
#endif
}
}
int wmain(int argc, wchar_t** argv) {
    int result = 1;
    try { result = storage::dispatch(argc, argv); }
    catch (const std::exception& error) {
        if (argc > 1) fprintf(stderr, "%s\n", error.what());
        try {
            auto logs = storage::join(storage::projectRoot(), L"logs");
            storage::makeDirectory(logs);
            FILE* file = nullptr;
            auto path = storage::join(logs, storage::uniqueName() + L"-native-error.txt");
            if (!_wfopen_s(&file, path.c_str(), L"wb")) { fprintf(file, "%s\n", error.what()); fclose(file); }
        } catch (...) {}
#ifndef STORAGE_WORKER
        if (argc == 1) MessageBoxA(nullptr, error.what(), "CleanTool", MB_OK | MB_ICONERROR);
#endif
    }
    return result;
}
