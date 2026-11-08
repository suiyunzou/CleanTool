#include "storage.h"
#include <winioctl.h>
#include <tlhelp32.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <chrono>

namespace storage {
namespace {
int checks = 0;
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); ++checks; }
void fixture(const std::wstring& path, bool old, DWORD bytes = 1024) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(!!file, "Cannot create fixture");
    std::vector<char> buffer(bytes, 'x'); DWORD written = 0;
    check(WriteFile(file, buffer.data(), bytes, &written, nullptr) && written == bytes, "Cannot write fixture");
    if (old) {
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        uint64_t value = fileTime(now) - 30ULL * 86400 * 10000000;
        FILETIME date{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
        check(SetFileTime(file, &date, nullptr, &date) != FALSE, "Cannot age fixture");
    }
}
bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void junction(const std::wstring& path, const std::wstring& destination) {
    makeDirectory(path);
    Handle dir(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    std::wstring target = L"\\??\\" + destination;
    struct Reparse {
        DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength;
        wchar_t buffer[2048];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.subLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.printOffset = data.subLength + sizeof(wchar_t);
    data.printLength = static_cast<WORD>(destination.size() * sizeof(wchar_t));
    wcscpy_s(data.buffer, target.c_str());
    wcscpy_s(data.buffer + target.size() + 1, 2048 - target.size() - 1, destination.c_str());
    data.length = static_cast<WORD>(8 + data.printOffset + data.printLength + sizeof(wchar_t));
    DWORD result = 0;
    check(dir && DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, &data, data.length + 8, nullptr, 0, &result, nullptr), "Cannot create test junction");
}
void cleanupOwned(const std::wstring& root, const std::wstring& path) {
    auto resolved = fullPath(path), boundary = fullPath(root);
    check(resolved == boundary || (resolved.size() > boundary.size() && resolved[boundary.size()] == L'\\' &&
        _wcsnicmp(resolved.c_str(), boundary.c_str(), boundary.size()) == 0), "Fixture cleanup escaped root");
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(path.c_str()); return; }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { DeleteFileW(path.c_str()); return; }
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(join(path, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            cleanupOwned(root, join(path, data.cFileName));
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}
struct CancelAfter {
    Shared* state; HANDLE cancel;
    static DWORD WINAPI run(void* arg) {
        auto self = static_cast<CancelAfter*>(arg);
        auto deadline = GetTickCount64() + 5000;
        while (self->state->groups[0].deleted < 1 && GetTickCount64() < deadline) Sleep(1);
        SetEvent(self->cancel); return 0;
    }
};
}
int selfTest(const std::wstring& report) {
    FILE* output = nullptr;
    if (_wfopen_s(&output, report.c_str(), L"wb")) return 1;
    std::wstring session = join(join(projectRoot(), L"scratch\\sessions"), uniqueName());
    bool passed = false;
    try {
        makeDirectory(session);
        std::wstring root = join(session, L"fixture"), outside = join(session, L"outside");
        makeDirectory(root); makeDirectory(outside);
        fixture(join(root, L"old.tmp"), true);
        fixture(join(root, L"recent.tmp"), false);
        fixture(join(root, L"changed.tmp"), true);
        fixture(join(root, L"locked.tmp"), true);
        fixture(join(root, L"keep.vhdx"), true);
        fixture(join(outside, L"outside.tmp"), true);
        junction(join(root, L"linked"), outside);
        Handle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        std::unique_ptr<Shared> state(new Shared);
        scanFixture(root, session, *state, cancel);
        check(state->groups[0].candidates == 3, "Scan age/extension/reparse selection failed");
        check(state->groups[0].files == 5, "Junction was traversed");
        fixture(join(root, L"changed.tmp"), true, 2048);
        Handle lock(CreateFileW(join(root, L"locked.tmp").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        cleanFixture(root, session, *state, cancel);
        check(!exists(join(root, L"old.tmp")), "Old file was not removed");
        check(exists(join(root, L"recent.tmp")) && exists(join(root, L"changed.tmp")) &&
            exists(join(root, L"locked.tmp")) && exists(join(root, L"keep.vhdx")) && exists(join(outside, L"outside.tmp")),
            "Protected fixture was deleted");
        check(state->groups[0].deleted == 1 && state->groups[0].skipped >= 2, "Cleanup result counts incorrect");
        lock.reset();
        // Replace a scanned directory with a junction before cleanup.
        auto swap = join(root, L"swap"); makeDirectory(swap);
        fixture(join(swap, L"outside.tmp"), true);
        scanFixture(root, session, *state, cancel);
        DeleteFileW(join(swap, L"outside.tmp").c_str()); RemoveDirectoryW(swap.c_str()); junction(swap, outside);
        cleanFixture(root, session, *state, cancel);
        check(exists(join(outside, L"outside.tmp")), "Directory replacement bypassed deletion boundary");
        scanFixture(outside, session, *state, cancel);
        cleanFixture(root, session, *state, cancel);
        check(exists(join(outside, L"outside.tmp")), "Candidate from a different root was deleted");
        fprintf(output, "PASS deletion: age, changed file, locked file, disk image, junction, post-scan directory replacement\n");
        auto sparsePath = join(root, L"large.tmp");
        {
            Handle file(CreateFileW(sparsePath.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            DWORD returned = 0;
            LARGE_INTEGER size{}; size.QuadPart = 5LL * 1024 * 1024 * 1024;
            check(file && DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
                SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file), "Cannot create sparse size fixture");
        }
        scanFixture(root, session, *state, cancel);
        check(state->groups[0].bytes >= 5ULL * 1024 * 1024 * 1024, "64-bit scan size overflow");
        check(sizeText(UINT64_MAX).find(L"TiB") != std::wstring::npos, "Large size formatting failed");
        DeleteFileW(sparsePath.c_str());
        fprintf(output, "PASS 5-GiB sparse file and UINT64 size formatting\n");
        SetEvent(cancel);
        scanFixture(root, session, *state, cancel);
        check(state->canceled != 0, "Scan cancellation ignored");
        ResetEvent(cancel);
        scanFixture(root, session, *state, cancel, 0);
        check(state->groups[0].state == static_cast<LONG>(State::Partial), "Scan budget was ignored");
        fprintf(output, "PASS cancellation and scan budget\n");

        auto bulk = join(session, L"bulk"); makeDirectory(bulk);
        for (int i = 0; i < 4000; ++i) fixture(join(bulk, std::to_wstring(i) + L".tmp"), true, 128);
        auto scanStart = std::chrono::steady_clock::now();
        scanFixture(bulk, session, *state, cancel);
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scanStart).count();
        check(state->groups[0].candidates == 4000 && state->groups[0].previewSize == previewCount, "Streaming scan count or preview bound failed");
        fprintf(output, "4000-file scan: %.3f ms; preview retained: %u\n", elapsed, state->groups[0].previewSize);
        CancelAfter control{state.get(), cancel};
        Handle thread(CreateThread(nullptr, 0, CancelAfter::run, &control, 0, nullptr));
        ULONGLONG start = GetTickCount64();
        cleanFixture(bulk, session, *state, cancel);
        check(WaitForSingleObject(thread, 6000) == WAIT_OBJECT_0, "Cancellation probe did not finish");
        check(state->canceled && state->groups[0].deleted > 0 && state->groups[0].deleted < 4000, "Cleanup cancellation did not preserve partial result");
        fprintf(output, "Cleanup interrupted after %llu files: %llu ms\n", state->groups[0].deleted, GetTickCount64() - start);
        ResetEvent(cancel);
        scanFixture(bulk, session, *state, cancel);
        cleanFixture(bulk, session, *state, cancel);
        check(state->groups[0].failed == 0, "Cleanup after cancellation failed");
        fprintf(output, "PASS rescan and cleanup after interruption\n");

        auto pidPath = join(session, L"child.pid");
        start = GetTickCount64();
        auto timed = command(executable(), L"--spawn-stall-test " + quote(pidPath), cancel, 250);
        check(timed.timedOut && GetTickCount64() - start < 1500, "Stalled command was not bounded");
        FILE* pidFile = nullptr; DWORD pid = 0;
        check(!_wfopen_s(&pidFile, pidPath.c_str(), L"rb"), "Child PID unavailable");
        fscanf_s(pidFile, "%lu", &pid); fclose(pidFile);
        Handle child(OpenProcess(SYNCHRONIZE, FALSE, pid));
        check(!child || WaitForSingleObject(child, 1000) == WAIT_OBJECT_0, "Command descendant survived job termination");
        fprintf(output, "PASS stalled command and descendant termination: %llu ms\n", GetTickCount64() - start);
        {
            Task task;
            task.start(Operation::Scan, session, true);
            task.stop(); task.stop();
            start = GetTickCount64();
            while (!task.poll() && GetTickCount64() - start < 2000) Sleep(10);
            check(!task.running(), "Repeated task stop did not finish");
            fprintf(output, "Task cancel and process exit: %llu ms\n", GetTickCount64() - start);
        }
        {
            Task task;
            task.start(Operation::Scan, session, false);
            Handle threads(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            THREADENTRY32 entry{sizeof(entry)};
            bool suspended = false;
            if (threads && Thread32First(threads, &entry)) do {
                if (entry.th32OwnerProcessID != GetProcessId(task.process())) continue;
                Handle ownedThread(OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID));
                if (ownedThread && SuspendThread(ownedThread) != static_cast<DWORD>(-1)) suspended = true;
            } while (Thread32Next(threads, &entry));
            check(suspended, "Could not stall owned worker");
            task.stop();
            start = GetTickCount64();
            while (!task.poll() && GetTickCount64() - start < 1500) Sleep(5);
            check(!task.running() && GetTickCount64() - start < 1200, "Hard stop deadline was not enforced");
            fprintf(output, "PASS unresponsive worker hard stop: %llu ms\n", GetTickCount64() - start);
        }
        PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
        fprintf(output, "Test process working set: %llu; private bytes: %llu\n",
            static_cast<unsigned long long>(mem.WorkingSetSize), static_cast<unsigned long long>(mem.PrivateUsage));
        fprintf(output, "PASS behavioral assertions: %d\n", checks);
        passed = true;
    } catch (const std::exception& error) { fprintf(output, "FAIL: %s\n", error.what()); }
    fclose(output);
    try { if (isSession(session)) cleanupOwned(session, session); } catch (...) {}
    return passed ? 0 : 1;
}
} // namespace storage
