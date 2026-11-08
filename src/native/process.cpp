#include "storage.h"
#include <bcrypt.h>
#include <cstdio>
#include <stdexcept>
#include <memory>

namespace storage {
std::wstring executable() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!n) throw std::runtime_error("Executable path unavailable");
        if (n < buffer.size()) return std::wstring(buffer.data(), n);
        if (buffer.size() >= 32768) throw std::runtime_error("Executable path too long");
        buffer.resize(std::min<size_t>(32768, buffer.size() * 2));
    }
}
std::wstring parent(const std::wstring& p) {
    auto i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
std::wstring join(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }
std::wstring projectRoot() {
    auto dir = parent(executable());
    // Build output and the installed program both carry an explicit root marker.
    for (int i = 0; i != 5 && !dir.empty(); ++i) {
        if (GetFileAttributesW(join(dir, L"src\\StorageCore.ps1").c_str()) != INVALID_FILE_ATTRIBUTES) return dir;
        dir = parent(dir);
    }
    throw std::runtime_error("Project root unavailable");
}
std::wstring environment(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) throw std::runtime_error("Required environment path unavailable");
    std::wstring text(n, L'\0');
    DWORD capacity = n;
    n = GetEnvironmentVariableW(name, &text[0], capacity);
    if (!n || n >= capacity) throw std::runtime_error("Environment path changed");
    text.resize(n);
    return text;
}
std::wstring fullPath(const std::wstring& path) {
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!required || required > 32768) throw std::runtime_error("Invalid path");
    std::vector<wchar_t> text(required);
    DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(text.size()), text.data(), nullptr);
    if (!n || n >= text.size()) throw std::runtime_error("Invalid path");
    std::wstring result(text.data(), n);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
std::wstring uniqueName() {
    unsigned char id[16]{};
    HMODULE library = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) throw std::runtime_error("Cannot load random generator");
    auto random = reinterpret_cast<decltype(&BCryptGenRandom)>(GetProcAddress(library, "BCryptGenRandom"));
    bool ok = random && random(nullptr, id, sizeof(id), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
    FreeLibrary(library);
    if (!ok)
        throw std::runtime_error("Cannot create session id");
    wchar_t text[40]{};
    swprintf_s(text, L"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
        id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    return text;
}
std::wstring quote(const std::wstring& p) {
    if (p.find_first_of(L"\"\r\n") != std::wstring::npos) throw std::runtime_error("Invalid argument");
    return L"\"" + p + L"\"";
}
std::wstring sizeText(uint64_t n) {
    wchar_t text[64]{};
    double value = static_cast<double>(n);
    const wchar_t* unit = L"B";
    for (auto candidate : {L"KiB", L"MiB", L"GiB", L"TiB"}) {
        if (value < 1024) break;
        value /= 1024;
        unit = candidate;
    }
    swprintf_s(text, L"%.1f %s", value, unit);
    return text;
}
std::wstring errorText(DWORD code) {
    wchar_t text[256]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, 256, nullptr);
    return text;
}
uint64_t fileTime(FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
bool plainPath(const std::wstring& path) {
    auto current = fullPath(path);
    while (current.size() > 3) {
        DWORD attr = GetFileAttributesW(current.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        current = parent(current);
    }
    return true;
}
void makeDirectory(const std::wstring& path) {
    if (!plainPath(path)) throw std::runtime_error("Linked directory rejected");
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Directory occupied by a file");
        return;
    }
    auto up = parent(path);
    if (up.size() > 3) makeDirectory(up);
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("Cannot create directory");
}
bool isSession(const std::wstring& path) {
    auto base = fullPath(join(projectRoot(), L"scratch\\sessions")) + L"\\";
    auto value = fullPath(path);
    if (value.size() != base.size() + 36 || _wcsnicmp(value.c_str(), base.c_str(), base.size()) != 0) return false;
    for (size_t i = base.size(); i < value.size(); ++i)
        if (!(iswxdigit(value[i]) || value[i] == L'-')) return false;
    return plainPath(value);
}
uint64_t freeSpace(const wchar_t* drive) {
    ULARGE_INTEGER free{}, total{}, all{};
    return GetDiskFreeSpaceExW(drive, &free, &total, &all) ? free.QuadPart : 0;
}
void writeSnapshot(const std::wstring& path, const Shared& data) {
    auto pending = path + L".pending";
    if (!plainPath(path) || !plainPath(pending)) throw std::runtime_error("Linked snapshot rejected");
    Handle file(CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    if (!file || !WriteFile(file, &data, sizeof(data), &written, nullptr) || written != sizeof(data))
        throw std::runtime_error("Cannot write snapshot");
    file.reset();
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Cannot commit snapshot");
}
bool readSnapshot(const std::wstring& path, Shared& data) {
    if (!plainPath(path)) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    DWORD read = 0;
    // Old history is display-only. Never accept v3 candidate previews for cleanup.
    if (file && GetFileSizeEx(file, &size) && path.size() >= 8 && path.substr(path.size() - 8) == L".summary") {
        struct Version4Shared {
            DWORD version; LONG active, done, canceled, error;
            uint64_t beforeFree, afterFree, elapsedMs;
            Group groups[31]; wchar_t message[256];
        };
        if (size.QuadPart == sizeof(Version4Shared)) {
            auto old = std::make_unique<Version4Shared>();
            if (!ReadFile(file, old.get(), sizeof(Version4Shared), &read, nullptr) || read != sizeof(Version4Shared) || old->version != 4) return false;
            data = Shared{}; data.active = old->active; data.done = old->done; data.canceled = old->canceled; data.error = old->error;
            data.beforeFree = old->beforeFree; data.afterFree = old->afterFree; data.elapsedMs = old->elapsedMs;
            std::copy(std::begin(old->groups), std::end(old->groups), data.groups);
            wcsncpy_s(data.message, old->message, _TRUNCATE); return true;
        }
        struct LegacyShared {
            DWORD version; LONG active, done, canceled, error;
            uint64_t beforeFree, afterFree, elapsedMs;
            Group groups[13]; wchar_t message[256];
        };
        if (size.QuadPart == sizeof(LegacyShared)) {
            auto legacy = std::make_unique<LegacyShared>();
            if (!ReadFile(file, legacy.get(), sizeof(LegacyShared), &read, nullptr) || read != sizeof(LegacyShared) || legacy->version != 3) return false;
            data = Shared{}; data.active = legacy->active; data.done = legacy->done; data.canceled = legacy->canceled; data.error = legacy->error;
            data.beforeFree = legacy->beforeFree; data.afterFree = legacy->afterFree; data.elapsedMs = legacy->elapsedMs;
            std::copy(std::begin(legacy->groups), std::end(legacy->groups), data.groups);
            wcsncpy_s(data.message, legacy->message, _TRUNCATE); return true;
        }
    }
    return file && GetFileSizeEx(file, &size) && size.QuadPart == sizeof(data) &&
        ReadFile(file, &data, sizeof(data), &read, nullptr) && read == sizeof(data) && data.version == snapshotVersion;
}
void removeSession(const std::wstring& path, bool demo) {
    if (!isSession(path)) return;
    // Delete only owned files. Unknown entries and fixture directories are retained.
    for (int i = 0; i != groupCount; ++i) {
        auto file = join(path, std::to_wstring(i) + L".candidates");
        if (plainPath(file)) DeleteFileW(file.c_str());
    }
    for (auto name : {L"preview.bin", L"preview.bin.pending", L"fixture.marker"}) {
        auto file = join(path, name);
        if (plainPath(file)) DeleteFileW(file.c_str());
    }
    if (demo) {
        for (auto name : {L"old.tmp", L"recent.tmp", L"locked.tmp"}) {
            auto file = join(join(path, L"fixture"), name);
            if (plainPath(file)) DeleteFileW(file.c_str());
        }
        auto fixture = join(path, L"fixture");
        if (plainPath(fixture)) RemoveDirectoryW(fixture.c_str());
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(join(path, L"*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = data.cFileName;
                auto suffix = name.find(L".summary");
                if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
                    suffix != 52 || (name.substr(suffix) != L".summary" &&
                    name.substr(suffix) != L".summary.journal" && name.substr(suffix) != L".summary.pending")) continue;
                DeleteFileW(join(path, name).c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }
    RemoveDirectoryW(path.c_str());
}

static Handle createJob() {
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        throw std::runtime_error("Cannot contain background process");
    return job;
}
CommandResult command(const std::wstring& program, const std::wstring& args, HANDLE cancel, DWORD timeout) {
    CommandResult result;
    Handle job = createJob(), readPipe, writePipe;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) throw std::runtime_error("Cannot create output pipe");
    readPipe.reset(r); writePipe.reset(w);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input; startup.hStdOutput = writePipe; startup.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring line = quote(program) + L" " + args;
    if (!CreateProcessW(program.c_str(), &line[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &pi)) {
        result.code = GetLastError();
        return result;
    }
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job, process)) { TerminateProcess(process, ERROR_PROCESS_ABORTED); throw std::runtime_error("Cannot contain command"); }
    ResumeThread(thread);
    writePipe.reset();
    ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD available = 0, drained = 0;
        while (drained < 65536 && PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[2048]; DWORD n = 0;
            if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &n, nullptr)) break;
            drained += n;
            if (result.output.size() < 16384) result.output.append(buffer, std::min<size_t>(n, 16384 - result.output.size()));
        }
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) { GetExitCodeProcess(process, &result.code); break; }
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { result.canceled = true; break; }
        if (GetTickCount64() - start >= timeout) { result.timedOut = true; break; }
        WaitForSingleObject(process, 20);
    }
    if (result.canceled || result.timedOut) {
        TerminateJobObject(job, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(process, 500);
    }
    return result;
}
Task::~Task() {
    terminate();
    if (shared_) UnmapViewOfFile(shared_);
}
void Task::start(Operation op, const std::wstring& path, bool demo, uint32_t selection) {
    if (running()) throw std::runtime_error("Task already running");
    if (shared_) { UnmapViewOfFile(shared_); shared_ = nullptr; }
    process_.reset(); job_.reset(); mapping_.reset(); cancel_.reset(); stopAt_ = 0;
    if (!isSession(path)) throw std::runtime_error("Invalid task directory");
    session = path;
    makeDirectory(session);
    std::wstring name = L"Local\\CleanTool-" + uniqueName();
    mapping_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str()));
    if (!mapping_) throw std::runtime_error("Cannot create task state");
    shared_ = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared_) throw std::runtime_error("Cannot map task state");
    *shared_ = Shared{};
    cancel_.reset(CreateEventW(nullptr, TRUE, FALSE, (name + L"-cancel").c_str()));
    if (!cancel_) throw std::runtime_error("Cannot create cancellation event");
    job_ = createJob();
    auto exe = join(parent(executable()), L"StorageWorker.exe");
    std::wstring args = quote(exe) + L" --worker " + std::to_wstring(static_cast<int>(op)) +
        L" " + quote(session) + L" " + quote(name) + L" " + (demo ? L"1" : L"0") + L" " + std::to_wstring(selection);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &args[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, projectRoot().c_str(), &startup, &pi)) throw std::runtime_error("Cannot start worker");
    process_.reset(pi.hProcess); Handle thread(pi.hThread);
    if (!AssignProcessToJobObject(job_, process_)) {
        TerminateProcess(process_, ERROR_PROCESS_ABORTED);
        throw std::runtime_error("Cannot contain worker");
    }
    ResumeThread(thread);
}
void Task::stop() { if (running() && !stopAt_) { stopAt_ = GetTickCount64(); SetEvent(cancel_); } }
void Task::terminate() {
    if (job_) {
        TerminateJobObject(job_, ERROR_PROCESS_ABORTED);
        if (running()) WaitForSingleObject(process_, 200);
    }
}
bool Task::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
bool Task::poll() {
    if (stopAt_ && GetTickCount64() - stopAt_ >= 800 && running()) terminate();
    if (running()) return false;
    terminate();
    return true;
}
DWORD Task::exitCode() const {
    DWORD code = ERROR_PROCESS_ABORTED;
    if (process_) GetExitCodeProcess(process_, &code);
    return code;
}
} // namespace storage
