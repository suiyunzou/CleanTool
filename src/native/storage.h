#pragma once
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace storage {
constexpr int groupCount = 32;
static_assert(groupCount <= 32, "Selection mask must fit uint32_t");
constexpr int previewCount = 12;
constexpr DWORD snapshotVersion = 5;
enum class Kind : DWORD { Temp, Shader, Browser, Package, System };
enum class State : LONG { Empty, Scanning, Ready, InUse, Partial, Unavailable, Canceled, Complete };
enum class Operation { Scan, Clean, Analyze, ComponentClean };

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(h.release()) {}
    Handle& operator=(Handle&& h) noexcept { reset(h.release()); return *this; }
    HANDLE release() { HANDLE h = value; value = nullptr; return h; }
    void reset(HANDLE h = nullptr) { if (value) CloseHandle(value); value = h; }
    explicit operator bool() const { return value != nullptr; }
    operator HANDLE() const { return value; }
};
struct Preview { wchar_t path[384]{}; uint64_t bytes = 0; };
struct Group {
    Kind kind{};
    volatile LONG state = 0;
    uint64_t bytes = 0, eligible = 0, files = 0, candidates = 0;
    uint64_t removed = 0, deleted = 0, skipped = 0, failed = 0;
    DWORD previewSize = 0;
    Preview previews[previewCount]{};
    wchar_t note[192]{};
};
// Fixed IPC storage. Candidate lists and deletion journals stay on disk.
struct Shared {
    DWORD version = snapshotVersion;
    volatile LONG active = -1, done = 0, canceled = 0, error = 0;
    uint64_t beforeFree = 0, afterFree = 0, elapsedMs = 0;
    Group groups[groupCount]{};
    wchar_t message[256]{};
};
struct Spec {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* process;
    Kind kind;
};
extern const std::array<Spec, groupCount> specs;
std::vector<std::wstring> ruleRoots(int id, bool* incomplete = nullptr);
bool ruleLocked(int id);
int scanRuleTest(const std::wstring& report);
bool elevated();
void systemCleanup(Group&, HANDLE cancel, bool clean);

std::wstring executable();
std::wstring projectRoot();
std::wstring join(const std::wstring&, const std::wstring&);
std::wstring parent(const std::wstring&);
std::wstring environment(const wchar_t*);
std::wstring fullPath(const std::wstring&);
std::wstring uniqueName();
std::wstring sizeText(uint64_t);
std::wstring errorText(DWORD);
std::wstring quote(const std::wstring&);
uint64_t fileTime(FILETIME);
bool plainPath(const std::wstring&);
void makeDirectory(const std::wstring&);
bool isSession(const std::wstring&);
uint64_t freeSpace(const wchar_t*);
void writeSnapshot(const std::wstring&, const Shared&);
bool readSnapshot(const std::wstring&, Shared&);
void removeSession(const std::wstring&, bool demo = false);

struct CommandResult { DWORD code = ERROR_PROCESS_ABORTED; bool canceled = false, timedOut = false; std::string output; };
CommandResult command(const std::wstring& program, const std::wstring& args, HANDLE cancel, DWORD timeout);

class Task {
    Handle process_, job_, cancel_, mapping_;
    Shared* shared_ = nullptr;
    ULONGLONG stopAt_ = 0;
public:
    std::wstring session;
    ~Task();
    Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    void start(Operation, const std::wstring&, bool demo, uint32_t selection = 0);
    void stop();
    void terminate();
    bool running() const;
    bool poll();
    Shared* shared() const { return shared_; }
    HANDLE process() const { return process_; }
    DWORD exitCode() const;
};

int worker(Operation, const std::wstring& session, Shared&, HANDLE cancel, bool demo, uint32_t selection);
void scanFixture(const std::wstring&, const std::wstring&, Shared&, HANDLE, DWORD budget = 30000);
void cleanFixture(const std::wstring&, const std::wstring&, Shared&, HANDLE);
int selfTest(const std::wstring& report);
int audit(const std::wstring& directory);
int autoTemp();
int runUi(bool demo, bool hiddenTest = false, const std::wstring& report = L"", bool navigationTest = false);
} // namespace storage
