#include "storage.h"
#include <tlhelp32.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <winioctl.h>

namespace storage {
struct FindHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FindHandle() { if (h != INVALID_HANDLE_VALUE) FindClose(h); }
};
struct File {
    FILE* value = nullptr;
    File(const std::wstring& path, const wchar_t* mode) {
        if (!plainPath(path) || _wfopen_s(&value, path.c_str(), mode) != 0) throw std::runtime_error("Cannot open task file");
    }
    ~File() { if (value) fclose(value); }
    void flush() { if (fflush(value)) throw std::runtime_error("Cannot flush task file"); }
};
static void writeBytes(FILE* file, const void* data, size_t n) {
    if (fwrite(data, 1, n, file) != n) throw std::runtime_error("Task file write failed");
}
template<class T> static void writeValue(FILE* file, const T& value) { writeBytes(file, &value, sizeof(value)); }
template<class T> static bool readValue(FILE* file, T& value) { return fread(&value, 1, sizeof(value), file) == sizeof(value); }
static void writeString(FILE* file, const std::wstring& value) {
    DWORD length = static_cast<DWORD>(value.size());
    writeValue(file, length); writeBytes(file, value.data(), length * sizeof(wchar_t));
}
static bool readString(FILE* file, std::wstring& value) {
    DWORD length = 0;
    if (!readValue(file, length) || length > 32767) return false;
    value.resize(length);
    return !length || fread(&value[0], sizeof(wchar_t), length, file) == length;
}
struct Candidate {
    std::wstring path, root;
    uint64_t bytes = 0, modified = 0, created = 0, identity = 0;
    DWORD volume = 0;
};
static void saveCandidate(FILE* file, const Candidate& c) {
    writeString(file, c.path); writeString(file, c.root);
    writeValue(file, c.bytes); writeValue(file, c.modified); writeValue(file, c.created); writeValue(file, c.identity); writeValue(file, c.volume);
}
static bool loadCandidate(FILE* file, Candidate& c) {
    if (feof(file)) return false;
    int ch = fgetc(file);
    if (ch == EOF) { if (ferror(file)) throw std::runtime_error("Cannot read candidate list"); return false; }
    ungetc(ch, file);
    if (!readString(file, c.path) || !readString(file, c.root) || !readValue(file, c.bytes) ||
        !readValue(file, c.modified) || !readValue(file, c.created) || !readValue(file, c.identity) || !readValue(file, c.volume)) throw std::runtime_error("Truncated candidate list");
    return true;
}
static bool canceled(HANDLE event) { return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }
static std::wstring local(const wchar_t* path) { return join(environment(L"LOCALAPPDATA"), path); }
static bool equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
static bool within(const std::wstring& path, const std::wstring& root) {
    return path.size() > root.size() && path[root.size()] == L'\\' && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0;
}
static bool processRunning(const wchar_t* name) {
    if (!*name) return false;
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return true;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snap, &entry)) return true;
    do {
        std::wstring names = name;
        size_t begin = 0;
        while (begin < names.size()) {
            auto end = names.find(L';', begin);
            if (!_wcsicmp(names.substr(begin, end - begin).c_str(), entry.szExeFile)) return true;
            if (end == std::wstring::npos) break;
            begin = end + 1;
        }
    } while (Process32NextW(snap, &entry));
    return GetLastError() != ERROR_NO_MORE_FILES;
}
static void add(uint64_t& amount, uint64_t n) {
    if (UINT64_MAX - amount < n) throw std::runtime_error("Size overflow");
    amount += n;
}
static bool keepDirectory(const wchar_t* name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    return lower.find(L"diagoutput") != std::wstring::npos || lower.find(L"diagnostic") != std::wstring::npos ||
        lower.find(L"node-compile-cache") != std::wstring::npos;
}
static bool protectedExtension(const std::wstring& path) {
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    auto ext = path.substr(dot);
    for (auto value : {L".vhd", L".vhdx", L".vmdk", L".kdbx"}) if (equal(ext, value)) return true;
    return false;
}
struct ScanContext {
    Group& group;
    FILE* candidates;
    HANDLE cancel;
    ULONGLONG deadline;
    uint64_t cutoff;
    bool partial = false;
    int rule = -1;
    void walk(const std::wstring& directory, const std::wstring& root, unsigned depth = 0) {
        if (canceled(cancel)) return;
        if (!plainPath(directory)) { ++group.skipped; partial = true; return; }
        if (depth > 96 || GetTickCount64() >= deadline) { partial = true; return; }
        WIN32_FIND_DATAW data{};
        FindHandle find;
        find.h = FindFirstFileExW(join(directory, L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find.h == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) { ++group.skipped; partial = true; }
            return;
        }
        do {
            if (canceled(cancel)) return;
            if (GetTickCount64() >= deadline) { partial = true; return; }
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++group.skipped; continue; }
            auto path = join(directory, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (rule == 2) continue;
                if (group.kind == Kind::Temp && keepDirectory(data.cFileName)) { ++group.skipped; continue; }
                walk(path, root, depth + 1);
                continue;
            }
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) { ++group.skipped; continue; }
            uint64_t bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            add(group.bytes, bytes); ++group.files;
            // Seven-day creation AND modification age applies to every raw file rule.
            bool eligible = group.kind != Kind::Package && fileTime(data.ftLastWriteTime) < cutoff &&
                fileTime(data.ftCreationTime) < cutoff && !protectedExtension(path);
            if (rule == 2 && (path.size() < 4 || !equal(path.substr(path.size() - 4), L".dmp"))) eligible = false;
            if (!eligible || !candidates) continue;
            Handle file(CreateFileW(path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!file || !GetFileInformationByHandle(file, &info) || info.nNumberOfLinks != 1 ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) ||
                fileTime(info.ftCreationTime) != fileTime(data.ftCreationTime) ||
                fileTime(info.ftLastWriteTime) != fileTime(data.ftLastWriteTime) ||
                ((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != bytes) {
                ++group.skipped; continue;
            }
            saveCandidate(candidates, {path, root, bytes, fileTime(info.ftLastWriteTime), fileTime(info.ftCreationTime),
                (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow, info.dwVolumeSerialNumber});
            add(group.eligible, bytes); ++group.candidates;
            if (group.previewSize < previewCount) {
                auto& item = group.previews[group.previewSize++];
                wcsncpy_s(item.path, path.c_str(), _TRUNCATE); item.bytes = bytes;
            }
        } while (FindNextFileW(find.h, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES) { ++group.skipped; partial = true; }
    }
};
static void scanGroup(int id, const std::vector<std::wstring>& roots, const std::wstring& session,
    Group& group, HANDLE cancel, ULONGLONG deadline) {
    group = Group{};
    group.kind = specs[id].kind;
    group.state = static_cast<LONG>(State::Scanning);
    if (processRunning(specs[id].process) || ruleLocked(id)) {
        group.state = static_cast<LONG>(State::InUse); wcscpy_s(group.note, L"进程、配置锁或路径状态未确认，已保留"); return;
    }
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    File candidates(join(session, std::to_wstring(id) + L".candidates"), L"wb");
    ScanContext context{group, candidates.value, cancel, deadline, fileTime(now) - 7ULL * 86400 * 10000000};
    context.rule = id;
    for (const auto& root : roots) {
        if (canceled(cancel)) break;
        if (!plainPath(root)) { ++group.skipped; context.partial = true; continue; }
        context.walk(root, fullPath(root));
    }
    candidates.flush();
    group.state = static_cast<LONG>(canceled(cancel) ? State::Canceled : context.partial ? State::Partial :
        group.candidates || (group.kind == Kind::Package && group.bytes) ? State::Ready : State::Empty);
}

static void scanRule(int id, const std::wstring& session, Group& group, HANDLE cancel, ULONGLONG deadline) {
    bool incomplete = false;
    auto roots = ruleRoots(id, &incomplete);
    scanGroup(id, roots, session, group, cancel, deadline);
    if (incomplete && group.state != static_cast<LONG>(State::Canceled)) {
        group.state = static_cast<LONG>(State::Partial);
        wcscpy_s(group.note, L"部分路径无法确认或达到配置数量上限，未完整扫描");
    } else if (group.state != static_cast<LONG>(State::InUse)) {
        wcscpy_s(group.note, group.kind == Kind::Package ? L"仅统计默认缓存；清理前须由原工具确认配置" : L"仅列出创建和修改均超过 7 天的未占用文件");
    }
}

// Hold every ancestor without FILE_SHARE_DELETE until disposition is committed.
// This prevents a directory from being swapped for a junction during deletion.
class ParentGuard {
    std::wstring current;
    std::vector<Handle> handles;
public:
    bool lock(const std::wstring& directory) {
        if (equal(current, directory) && !handles.empty()) return true;
        handles.clear(); current.clear();
        std::wstring path = fullPath(directory);
        std::vector<std::wstring> ancestors;
        while (path.size() > 3) {
            ancestors.push_back(path);
            path = parent(path);
        }
        // Pin from the volume down, so each next path resolves through a pinned ancestor.
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
            Handle h(CreateFileW(it->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!h || !GetFileInformationByHandle(h, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                handles.clear(); return false;
            }
            handles.push_back(std::move(h));
        }
        current = directory;
        return true;
    }
};
static DWORD deleteCandidate(const Candidate& c, const std::vector<std::wstring>& roots, ParentGuard& parents, bool preflight = false) {
    auto path = fullPath(c.path), root = fullPath(c.root);
    bool allowed = std::any_of(roots.begin(), roots.end(), [&](const std::wstring& r) { return equal(fullPath(r), root); });
    if (!allowed || !within(path, root) || path.find(L':', 2) != std::wstring::npos) return ERROR_ACCESS_DENIED;
    if (!parents.lock(parent(path))) return ERROR_REPARSE_TAG_INVALID;
    Handle file(CreateFileW(path.c_str(), (preflight ? FILE_READ_DATA : DELETE) | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return GetLastError();
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
    uint64_t bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (info.dwVolumeSerialNumber != c.volume || ((static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != c.identity ||
        info.nNumberOfLinks != 1 || bytes != c.bytes || fileTime(info.ftLastWriteTime) != c.modified || fileTime(info.ftCreationTime) != c.created)
        return ERROR_FILE_INVALID;
    if (preflight) return ERROR_SUCCESS;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) return GetLastError();
    // A non-shared handle pins the validated file until close commits deletion.
    file.reset();
    DWORD attr = GetFileAttributesW(path.c_str()), error = GetLastError();
    return attr == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ERROR_SUCCESS : ERROR_RETRY;
}
static void cleanGroup(int id, const std::vector<std::wstring>& roots, const std::wstring& session,
    Group& group, HANDLE cancel, FILE* journal) {
    File candidates(join(session, std::to_wstring(id) + L".candidates"), L"rb");
    ParentGuard parents;
    Candidate c;
    ULONGLONG lastProcessCheck = 0;
    while (!canceled(cancel) && loadCandidate(candidates.value, c)) {
        if (GetTickCount64() - lastProcessCheck >= 100) {
            lastProcessCheck = GetTickCount64();
            if (processRunning(specs[id].process) || ruleLocked(id)) { group.state = static_cast<LONG>(State::InUse); break; }
        }
        // Write intent before mutation. An interrupted operation is never logged as success.
        BYTE intent = 0; writeValue(journal, intent); writeValue(journal, id); saveCandidate(journal, c);
        if (fflush(journal)) throw std::runtime_error("Journal unavailable; cleanup stopped");
        DWORD code = deleteCandidate(c, roots, parents);
        BYTE outcome = 1; writeValue(journal, outcome); writeValue(journal, code);
        if (fflush(journal)) throw std::runtime_error("Journal unavailable; cleanup stopped");
        if (!code) { add(group.removed, c.bytes); ++group.deleted; }
        else if (code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_FILE_INVALID ||
            code == ERROR_REPARSE_TAG_INVALID || code == ERROR_FILE_NOT_FOUND) ++group.skipped;
        else ++group.failed;
    }
    if (group.state != static_cast<LONG>(State::InUse))
        group.state = static_cast<LONG>(canceled(cancel) ? State::Canceled : State::Complete);
}
static std::wstring findProgram(const wchar_t* name) {
    wchar_t path[32768]{};
    if (!SearchPathW(nullptr, name, nullptr, 32768, path, nullptr)) return L"";
    return path;
}
static CommandResult packageCommand(int id, bool clean, HANDLE cancel, const std::wstring& cache = L"") {
    const wchar_t* name = id == 10 ? L"pip.exe" : id == 11 ? L"npm.cmd" : L"uv.exe";
    auto program = findProgram(name);
    if (program.empty()) return {ERROR_FILE_NOT_FOUND};
    std::wstring args = clean ? (id == 10 ? L"cache purge" : id == 11 ?
        L"cache clean --force --logs-max=0 --update-notifier=false" : L"cache prune --offline") :
        (id == 11 ? L"config get cache --logs-max=0 --update-notifier=false" : L"cache dir");
    if (clean) args += (id == 11 ? L" --cache " : L" --cache-dir ") + quote(cache);
    if (id == 11) {
        if ((program + cache).find_first_of(L"&|<>^%!") != std::wstring::npos) throw std::runtime_error("Unsupported command path");
        args = L"/d /s /c \"" + quote(program) + L" " + args + L"\"";
        program = join(environment(L"SystemRoot"), L"System32\\cmd.exe");
    }
    return command(program, args, cancel, clean ? 90000 : 5000);
}
static std::wstring outputPath(const std::string& text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], count);
    auto end = wide.find_last_not_of(L"\r\n \t");
    if (end == std::wstring::npos) return L"";
    wide.resize(end + 1);
    auto begin = wide.find_last_of(L"\r\n");
    if (begin != std::wstring::npos) wide.erase(0, begin + 1);
    return wide;
}
static void cleanPackage(int id, const std::vector<std::wstring>& roots, Group& group, HANDLE cancel) {
    if (roots.empty() || processRunning(specs[id].process)) { ++group.skipped; group.state = static_cast<LONG>(State::InUse); return; }
    auto migration = local((std::wstring(L"CDriveStorageTool\\migrations\\") + specs[id].id + L".json").c_str());
    if (GetFileAttributesW(migration.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ++group.skipped; wcscpy_s(group.note, L"存在迁移记录，请用原迁移工具复核"); return;
    }
    auto config = packageCommand(id, false, cancel);
    auto expected = id == 11 ? parent(roots[0]) : roots[0];
    auto actual = outputPath(config.output);
    if (config.code || config.canceled || config.timedOut || actual.empty() ||
        !equal(fullPath(actual), fullPath(expected))) {
        ++group.skipped; wcscpy_s(group.note, L"缓存配置未确认，已保留"); return;
    }
    ParentGuard cacheGuard;
    if (!cacheGuard.lock(actual)) { ++group.skipped; return; }
    Group before{}, after{}; before.kind = after.kind = Kind::Package;
    ScanContext measure{before, nullptr, cancel, GetTickCount64() + 4000, 0};
    measure.walk(roots[0], roots[0]);
    if (canceled(cancel)) return;
    auto result = packageCommand(id, true, cancel, actual);
    if (result.code || result.timedOut) ++group.failed;
    if (result.canceled || canceled(cancel)) { group.state = static_cast<LONG>(State::Canceled); return; }
    ScanContext finish{after, nullptr, cancel, GetTickCount64() + 4000, 0};
    finish.walk(roots[0], roots[0]);
    if (!measure.partial && !finish.partial && before.bytes >= after.bytes) group.removed = before.bytes - after.bytes;
    else wcscpy_s(group.note, L"大小未完整读取");
    group.state = static_cast<LONG>(State::Complete);
}
static void createFixture(const std::wstring& session) {
    auto root = join(session, L"fixture");
    makeDirectory(root);
    for (auto name : {L"old.tmp", L"recent.tmp", L"locked.tmp"}) {
        auto path = join(root, name);
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) continue;
        char content[4096]{}; DWORD written = 0;
        if (!WriteFile(file, content, sizeof(content), &written, nullptr)) throw std::runtime_error("Cannot write demo");
        if (wcscmp(name, L"recent.tmp")) {
            FILETIME now{}; GetSystemTimeAsFileTime(&now);
            uint64_t old = fileTime(now) - 40ULL * 86400 * 10000000;
            FILETIME stamp{static_cast<DWORD>(old), static_cast<DWORD>(old >> 32)};
            SetFileTime(file, &stamp, nullptr, &stamp);
        }
    }
}
void scanFixture(const std::wstring& root, const std::wstring& session, Shared& state, HANDLE cancel, DWORD budget) {
    scanGroup(0, {root}, session, state.groups[0], cancel, GetTickCount64() + budget);
    state.canceled = canceled(cancel);
    if (!state.canceled) writeSnapshot(join(session, L"preview.bin"), state);
}
void cleanFixture(const std::wstring& root, const std::wstring& session, Shared& state, HANDLE cancel) {
    File journal(join(session, L"fixture.journal"), L"wb");
    cleanGroup(0, {root}, session, state.groups[0], cancel, journal.value);
    state.canceled = canceled(cancel);
}
int worker(Operation op, const std::wstring& session, Shared& shared, HANDLE cancel, bool demo, uint32_t selection) {
    ULONGLONG start = GetTickCount64();
    std::wstring historyPath;
    try {
        if (!isSession(session)) throw std::runtime_error("Task directory rejected");
        if (op == Operation::Scan) {
            DeleteFileW(join(session, L"preview.bin").c_str());
            if (demo) { createFixture(session); scanFixture(join(session, L"fixture"), session, shared, cancel); }
            else {
                for (int id = 0; id != groupCount && !canceled(cancel); ++id) {
                    InterlockedExchange(&shared.active, id);
                    if (specs[id].kind == Kind::System) systemCleanup(shared.groups[id], cancel, false);
                    else scanRule(id, session, shared.groups[id], cancel, std::min(start + 30000, GetTickCount64() + 4000));
                }
                if (!canceled(cancel)) writeSnapshot(join(session, L"preview.bin"), shared);
            }
        } else if (op == Operation::Clean) {
            auto preview = join(session, L"preview.bin");
            WIN32_FILE_ATTRIBUTE_DATA info{};
            FILETIME now{}; GetSystemTimeAsFileTime(&now);
            if (!GetFileAttributesExW(preview.c_str(), GetFileExInfoStandard, &info) ||
                fileTime(now) > fileTime(info.ftLastWriteTime) + 30ULL * 60 * 10000000 ||
                !readSnapshot(preview, shared)) throw std::runtime_error("Preview expired; scan again");
            if (!selection) throw std::runtime_error("Invalid cleanup selection");
            if (!DeleteFileW(preview.c_str())) throw std::runtime_error("Cannot consume preview");
            auto logs = demo ? session : join(projectRoot(), L"logs");
            makeDirectory(logs);
            SYSTEMTIME time{}; GetLocalTime(&time);
            wchar_t timestamp[48]{};
            swprintf_s(timestamp, L"%04u%02u%02u-%02u%02u%02u-", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
            historyPath = join(logs, std::wstring(timestamp) + uniqueName() + L".summary");
            shared.beforeFree = freeSpace(L"C:\\");
            shared.done = 0;
            writeSnapshot(historyPath, shared);
            File journal(historyPath + L".journal", L"wb");
            Handle locked;
            if (demo) locked.reset(CreateFileW(join(session, L"fixture\\locked.tmp").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            for (int id = 0; id != groupCount && !canceled(cancel); ++id) {
                if (!(selection & (1U << id)) || (demo && id != 0)) continue;
                InterlockedExchange(&shared.active, id);
                auto& group = shared.groups[id];
                if (specs[id].kind == Kind::System) systemCleanup(group, cancel, true);
                else {
                    auto roots = demo ? std::vector<std::wstring>{join(session, L"fixture")} : ruleRoots(id);
                    if (group.kind == Kind::Package) cleanPackage(id, roots, group, cancel);
                    else cleanGroup(id, roots, session, group, cancel, journal.value);
                }
                shared.canceled = canceled(cancel);
                writeSnapshot(historyPath, shared);
            }
            shared.afterFree = freeSpace(L"C:\\");
        } else {
            if (demo) throw std::runtime_error("System tools disabled in demo");
            auto result = command(join(environment(L"SystemRoot"), L"System32\\dism.exe"),
                op == Operation::Analyze ? L"/Online /Cleanup-Image /AnalyzeComponentStore /NoRestart" :
                L"/Online /Cleanup-Image /StartComponentCleanup /NoRestart", cancel, 15 * 60 * 1000);
            shared.error = result.code != 0 && result.code != 3010;
            auto logs = join(projectRoot(), L"logs"); makeDirectory(logs);
            File report(join(logs, uniqueName() + L"-component.txt"), L"wb");
            writeBytes(report.value, result.output.data(), result.output.size());
            swprintf_s(shared.message, L"DISM: %lu", result.code);
        }
        shared.canceled = canceled(cancel); shared.elapsedMs = GetTickCount64() - start;
        InterlockedExchange(&shared.done, 1);
        if (!historyPath.empty()) writeSnapshot(historyPath, shared);
        return shared.error ? 1 : 0;
    } catch (const std::exception& error) {
        shared.error = 1; shared.canceled = canceled(cancel);
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, shared.message, 256);
        shared.elapsedMs = GetTickCount64() - start;
        InterlockedExchange(&shared.done, 1);
        if (!historyPath.empty()) { try { writeSnapshot(historyPath, shared); } catch (...) {} }
        return 1;
    }
}
int audit(const std::wstring& directory) {
    auto session = join(join(projectRoot(), L"scratch\\sessions"), uniqueName());
    makeDirectory(session);
    std::unique_ptr<Shared> state(new Shared);
    int result = worker(Operation::Scan, session, *state, nullptr, false, 0);
    auto logs = directory.empty() ? join(projectRoot(), L"logs") : fullPath(directory);
    makeDirectory(logs);
    auto path = join(logs, uniqueName() + L"-scan.txt");
    File report(path, L"wb");
    fprintf(report.value, "mode=SCAN_ONLY cleanup_executed=false\nmole_commit=3423ee037774fec5b20ec63b912a6df65a79675d\nelapsed_ms=%llu error=%ld\nstate: 0=empty 2=ready 3=in_use 4=partial 5=unavailable 6=canceled\nid\tstate\tfiles\tcandidates\tbytes\teligible\tskipped\n", state->elapsedMs, state->error);
    auto utf8 = [](const std::wstring& value) {
        int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &out[0], n, nullptr, nullptr);
        return out;
    };
    File detail(path + L".candidates.tsv", L"wb");
    fprintf(detail.value, "rule\tbytes\tpreflight_code\tpath\n");
    uint64_t verified = 0, retained = 0;
    for (int i = 0; i != groupCount; ++i) {
        const auto& g = state->groups[i];
        fprintf(report.value, "%ls\t%ld\t%llu\t%llu\t%llu\t%llu\t%llu\n", specs[i].id, g.state, g.files, g.candidates, g.bytes, g.eligible, g.skipped);
        fprintf(report.value, "note\t%s\n", utf8(g.note).c_str());
        if (g.kind == Kind::System) continue;
        auto roots = ruleRoots(i);
        for (auto& root : roots) fprintf(report.value, "root\t%s\n", utf8(root).c_str());
        if (!g.candidates) continue;
        File candidates(join(session, std::to_wstring(i) + L".candidates"), L"rb");
        Candidate c;
        ParentGuard guard;
        ULONGLONG deadline = GetTickCount64() + 4000;
        bool busy = processRunning(specs[i].process) || ruleLocked(i);
        while (loadCandidate(candidates.value, c)) {
            DWORD code = busy ? ERROR_BUSY : GetTickCount64() >= deadline ? ERROR_TIMEOUT : deleteCandidate(c, roots, guard, true);
            if (!code) ++verified; else ++retained;
            fprintf(detail.value, "%ls\t%llu\t%lu\t%s\n", specs[i].id, c.bytes, code, utf8(c.path).c_str());
        }
    }
    fprintf(report.value, "preflight_pass=%llu preflight_retained=%llu (metadata checks only; no DELETE access or disposition)\n", verified, retained);
    detail.flush();
    report.flush();
    removeSession(session);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    std::string encoded(bytes, '\0');
    if (bytes) WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), &encoded[0], bytes, nullptr, nullptr);
    fprintf(stdout, "%s\n", encoded.c_str());
    return result;
}

// Scan and metadata-only preflight. Deliberately never calls a cleanup operation.
int scanRuleTest(const std::wstring& report) {
    File output(report, L"wb");
    auto session = join(join(projectRoot(), L"scratch\\sessions"), uniqueName());
    makeDirectory(session);
    struct EnvironmentGuard {
        std::wstring local = environment(L"LOCALAPPDATA"), roaming = environment(L"APPDATA"), home = environment(L"USERPROFILE");
        ~EnvironmentGuard() { SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str()); SetEnvironmentVariableW(L"APPDATA", roaming.c_str()); SetEnvironmentVariableW(L"USERPROFILE", home.c_str()); }
    } restore;
    try {
        auto localRoot = join(session, L"local"), roaming = join(session, L"roaming"), home = join(session, L"home");
        for (auto& path : {localRoot, roaming, home}) makeDirectory(path);
        SetEnvironmentVariableW(L"LOCALAPPDATA", localRoot.c_str()); SetEnvironmentVariableW(L"APPDATA", roaming.c_str()); SetEnvironmentVariableW(L"USERPROFILE", home.c_str());
        auto check = [&](bool ok, const char* label) { fprintf(output.value, "%s %s\n", ok ? "PASS" : "FAIL", label); if (!ok) throw std::runtime_error(label); };
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        uint64_t old = fileTime(now) - 30ULL * 86400 * 10000000;
        FILETIME stamp{static_cast<DWORD>(old), static_cast<DWORD>(old >> 32)};
        auto fixture = [&](const std::wstring& path, bool aged = true) {
            makeDirectory(parent(path));
            Handle h(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_NEW, 0, nullptr));
            DWORD written = 0;
            if (!h || !WriteFile(h, "fixture", 7, &written, nullptr) || written != 7 ||
                (aged && !SetFileTime(h, &stamp, nullptr, &stamp))) throw std::runtime_error("Fixture creation failed");
        };
        auto root = join(localRoot, L"Temp");
        for (auto name : {L"old.tmp", L"locked.tmp", L"keep.vhdx", L"hardlink.tmp"}) fixture(join(root, name));
        fixture(join(root, L"recent.tmp"), false);
        check(CreateHardLinkW(join(root, L"hardlink-copy.tmp").c_str(), join(root, L"hardlink.tmp").c_str(), nullptr) != FALSE, "hardlink fixture");
        Handle busy(CreateFileW(join(root, L"locked.tmp").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        Group group{};
        scanGroup(0, {root}, session, group, nullptr, GetTickCount64() + 4000);
        fprintf(output.value, "fixture state=%ld candidates=%llu eligible=%llu skipped=%llu files=%llu\n", group.state, group.candidates, group.eligible, group.skipped, group.files);
        check(group.candidates == 1 && group.eligible == 7 && group.skipped == 3, "age / virtual disk / busy file / hardlink exclusions");
        Candidate candidate;
        { File list(join(session, L"0.candidates"), L"rb"); check(loadCandidate(list.value, candidate), "candidate identity captured"); }
        ParentGuard guard;
        check(deleteCandidate(candidate, {root}, guard, true) == 0 && GetFileAttributesW(candidate.path.c_str()) != INVALID_FILE_ATTRIBUTES, "preflight succeeds without deletion");
        auto pinRoot = join(session, L"pinned-parent"); makeDirectory(pinRoot);
        {
            ParentGuard pin;
            check(pin.lock(pinRoot), "parent directory guard acquires listing handle");
            check(!MoveFileW(pinRoot.c_str(), (pinRoot + L"-moved").c_str()), "parent directory cannot be renamed while guard is held");
        }
        auto changed = candidate; changed.identity ^= 1;
        check(deleteCandidate(changed, {root}, guard, true) == ERROR_FILE_INVALID, "same metadata with wrong file identity rejected");
        check(deleteCandidate(candidate, {home}, guard, true) == ERROR_ACCESS_DENIED, "foreign root rejected");
        check(MoveFileW(candidate.path.c_str(), (candidate.path + L".saved").c_str()) != FALSE, "replace fixture while preserving original");
        fixture(candidate.path);
        check(deleteCandidate(candidate, {root}, guard, true) == ERROR_FILE_INVALID, "replacement file with same size and timestamps rejected");
        fixture(join(localRoot, L"CrashDumps\\old.dmp")); fixture(join(localRoot, L"CrashDumps\\keep.txt")); fixture(join(localRoot, L"CrashDumps\\nested\\keep.dmp"));
        scanGroup(2, ruleRoots(2), session, group, nullptr, GetTickCount64() + 4000);
        check(group.candidates == 1, "CrashDumps only top-level .dmp");
        auto chrome = join(localRoot, L"BraveSoftware\\Brave-Browser\\User Data");
        fixture(join(chrome, L"Default\\Cache\\cache.bin")); fixture(join(chrome, L"Default\\Cookies"));
        fixture(join(chrome, L"Profile 2\\Code Cache\\cache.bin")); fixture(join(chrome, L"Profile invalid\\Cache\\keep.bin"));
        bool incomplete = false;
        auto roots = ruleRoots(14, &incomplete);
        check(!incomplete && roots.size() == 2 && std::none_of(roots.begin(), roots.end(), [](const std::wstring& p) { return p.find(L"invalid") != std::wstring::npos || p.find(L"Cookies") != std::wstring::npos; }), "Chromium profile names and cache leaf allowlist");
        for (int n = 0; n < 130; ++n) makeDirectory(join(chrome, L"Other" + std::to_wstring(n)));
        ruleRoots(14, &incomplete); check(incomplete, "bounded discovery reports incomplete");
        fixture(join(localRoot, L"Mozilla\\Firefox\\Profiles\\test.default\\cache2\\cache.bin"));
        fixture(join(roaming, L"Mozilla\\Firefox\\Profiles\\test.default\\places.sqlite"));
        auto lockPath = join(roaming, L"Mozilla\\Firefox\\Profiles\\test.default\\parent.lock"); fixture(lockPath);
        roots = ruleRoots(13, &incomplete);
        check(!incomplete && roots.size() == 1 && within(roots[0], localRoot), "Firefox local cache excludes roaming profile data");
        Handle lock(CreateFileW(lockPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        check(ruleLocked(13), "Firefox paired roaming lock blocks local cache");
        lock.reset(); check(!ruleLocked(13), "stale unlocked Firefox lock is allowed");
        auto self = executable(); self = self.substr(self.find_last_of(L'\\') + 1);
        check(processRunning((L"not-a-real-process.exe;" + self).c_str()), "multi-process owner guard");
        auto linkPath = join(localRoot, L"Zed\\cache"); makeDirectory(linkPath);
        {
            Handle dir(CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            std::wstring target = L"\\??\\" + root;
            struct Reparse { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t buffer[2048]; } data{};
            data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.subLength = static_cast<WORD>(target.size() * 2); data.printOffset = data.subLength + 2; data.printLength = static_cast<WORD>(root.size() * 2);
            wcscpy_s(data.buffer, target.c_str()); wcscpy_s(data.buffer + target.size() + 1, 2048 - target.size() - 1, root.c_str());
            data.length = static_cast<WORD>(8 + data.printOffset + data.printLength + 2); DWORD returned = 0;
            check(dir && DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, &data, data.length + 8, nullptr, 0, &returned, nullptr), "junction fixture");
        }
        roots = ruleRoots(30, &incomplete); check(incomplete && roots.empty(), "linked cache root rejected");
        const std::pair<int, const wchar_t*> positive[] = {
            {15, L"Vivaldi\\User Data\\Default\\Cache"}, {16, L"Opera Software\\Opera Stable\\Cache"},
            {19, L"Packages\\MSTeams_8wekyb3d8bbwe\\LocalCache\\Microsoft\\MSTeams\\EBWebView\\Default\\Cache"},
            {24, L"AMD\\DXCache"}, {25, L"Intel\\ShaderCache"}, {26, L"Yarn\\Cache"},
            {28, L"NuGet\\v3-cache"}, {29, L"node-gyp\\Cache"}
        };
        for (auto& item : positive) { makeDirectory(join(localRoot, item.second)); check(!ruleRoots(item.first).empty(), "new local cache rule resolves fixture"); }
        const std::pair<int, const wchar_t*> apps[] = {{17, L"discord"}, {18, L"Slack"}, {20, L"Windsurf"}, {21, L"obsidian"}, {22, L"Notion"}, {23, L"Code - Insiders"}};
        for (auto& item : apps) { makeDirectory(join(join(roaming, item.second), L"Cache")); check(!ruleRoots(item.first).empty(), "new roaming cache rule resolves fixture"); }
        makeDirectory(join(home, L".bun\\install\\cache")); check(!ruleRoots(27).empty(), "Bun installation cache resolves fixture");
        // v3 history remains readable, but its old candidate format is rejected.
        struct Legacy { DWORD version = 3; LONG active = -1, done = 1, canceled = 0, error = 0; uint64_t beforeFree = 1, afterFree = 2, elapsedMs = 3; Group groups[13]{}; wchar_t message[256]{}; };
        auto legacy = std::make_unique<Legacy>(); legacy->groups[0].removed = 7;
        auto history = join(session, L"old.summary"), preview = join(session, L"old-preview.bin");
        { File oldHistory(history, L"wb"); writeBytes(oldHistory.value, legacy.get(), sizeof(Legacy)); }
        { File oldPreview(preview, L"wb"); writeBytes(oldPreview.value, legacy.get(), sizeof(Legacy)); }
        auto snapshot = std::make_unique<Shared>();
        check(readSnapshot(history, *snapshot) && snapshot->groups[0].removed == 7 && snapshot->version == snapshotVersion, "legacy history display supported");
        check(!readSnapshot(preview, *snapshot), "legacy cleanup preview rejected");
        struct Version4 { DWORD version = 4; LONG active = -1, done = 1, canceled = 0, error = 0; uint64_t beforeFree = 1, afterFree = 2, elapsedMs = 3; Group groups[31]{}; wchar_t message[256]{}; };
        auto version4 = std::make_unique<Version4>(); version4->groups[30].removed = 123;
        { File oldHistory(history, L"wb"); writeBytes(oldHistory.value, version4.get(), sizeof(Version4)); }
        { File oldPreview(preview, L"wb"); writeBytes(oldPreview.value, version4.get(), sizeof(Version4)); }
        check(readSnapshot(history, *snapshot) && snapshot->groups[30].removed == 123 && !snapshot->groups[31].removed, "v4 history preserves all 31 groups");
        check(!readSnapshot(preview, *snapshot), "v4 cleanup preview rejected after system group added");
        scanGroup(0, {root}, session, group, nullptr, GetTickCount64());
        check(group.state == static_cast<LONG>(State::Partial), "expired scan budget reports partial");
        Handle cancel(CreateEventW(nullptr, TRUE, TRUE, nullptr));
        scanGroup(0, {root}, session, group, cancel, GetTickCount64() + 4000);
        check(group.state == static_cast<LONG>(State::Canceled), "canceled scan reports canceled");
        check(GetFileAttributesW(candidate.path.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(join(chrome, L"Default\\Cookies").c_str()) != INVALID_FILE_ATTRIBUTES, "fixtures preserved; cleanup never invoked");
        fprintf(output.value, "PASS scan-only suite\nFixture session: %ls\n", session.c_str()); output.flush(); return 0;
    } catch (const std::exception& e) {
        fprintf(output.value, "FAIL %s\nFixture session: %ls\n", e.what(), session.c_str()); output.flush(); return 1;
    }
}
int autoTemp() {
    auto session = join(join(projectRoot(), L"scratch\\sessions"), uniqueName());
    makeDirectory(session);
    try {
        std::unique_ptr<Shared> state(new Shared);
        for (int id = 0; id < 2; ++id)
            scanRule(id, session, state->groups[id], nullptr, GetTickCount64() + 4000);
        writeSnapshot(join(session, L"preview.bin"), *state);
        int result = worker(Operation::Clean, session, *state, nullptr, false, 3);
        removeSession(session);
        if (result || state->groups[0].failed || state->groups[1].failed) return 1;
        return state->groups[0].skipped || state->groups[1].skipped ? 2 : 0;
    } catch (...) { removeSession(session); throw; }
}
} // namespace storage
