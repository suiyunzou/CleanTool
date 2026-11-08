#include "storage.h"
#include <emptyvc.h>

namespace storage {
bool elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    Handle token(raw); TOKEN_ELEVATION value{}; DWORD size = 0;
    return GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size) && value.TokenIsElevated;
}
namespace {
// Only these Windows-owned categories are allowed. Never enumerate all handlers:
// DownloadsFolder, Previous Installations, drivers and Recycle Bin are excluded.
struct Category { const wchar_t* key; const wchar_t* name; };
constexpr Category categories[]{
    {L"Update Cleanup", L"Windows 更新清理"},
    {L"Delivery Optimization Files", L"传递优化缓存"},
    {L"Temporary Setup Files", L"临时安装文件"}
};
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Handler {
    HKEY key = nullptr;
    IEmptyVolumeCache* cache = nullptr;
    ~Handler() {
        if (cache) { DWORD flags = 0; cache->Deactivate(&flags); cache->Release(); }
        if (key) RegCloseKey(key);
    }
    HRESULT open(const Category& category) {
        auto path = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VolumeCaches\\") + category.key;
        LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
        if (status) return HRESULT_FROM_WIN32(status);
        wchar_t text[80]{}; DWORD size = sizeof(text), type = 0;
        status = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(text), &size);
        if (status || type != REG_SZ || size >= sizeof(text)) return E_FAIL;
        CLSID clsid{};
        HRESULT hr = CLSIDFromString(text, &clsid);
        if (FAILED(hr)) return hr;
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
        if (FAILED(hr)) return hr;
        // Zero flags is essential: SETTINGSMODE may delete during Initialize.
        DWORD flags = 0; LPWSTR name = nullptr, description = nullptr, button = nullptr;
        IEmptyVolumeCache2* extended = nullptr;
        if (SUCCEEDED(cache->QueryInterface(IID_PPV_ARGS(&extended)))) {
            hr = extended->InitializeEx(key, L"C:\\", category.key, &name, &description, &button, &flags);
            extended->Release();
        } else hr = cache->Initialize(key, L"C:\\", &name, &description, &flags);
        CoTaskMemFree(name); CoTaskMemFree(description); CoTaskMemFree(button);
        return hr;
    }
};
class Progress final : public IEmptyVolumeCacheCallBack {
    LONG refs = 1;
public:
    HANDLE cancel;
    ULONGLONG deadline;
    explicit Progress(HANDLE event, DWORD budget) : cancel(event), deadline(GetTickCount64() + budget) {}
    HRESULT status() const {
        return (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) || GetTickCount64() >= deadline ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IEmptyVolumeCacheCallBack)) return E_NOINTERFACE;
        *value = static_cast<IEmptyVolumeCacheCallBack*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { auto n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE ScanProgress(DWORDLONG, DWORD, LPCWSTR) override { return status(); }
    HRESULT STDMETHODCALLTYPE PurgeProgress(DWORDLONG, DWORDLONG, DWORD, LPCWSTR) override { return status(); }
};
}
void systemCleanup(Group& group, HANDLE cancel, bool clean) {
    Group scanned = group;
    group = Group{}; group.kind = Kind::System;
    group.state = static_cast<LONG>(State::Scanning);
    if (!elevated()) {
        group.state = static_cast<LONG>(State::Unavailable);
        wcscpy_s(group.note, L"需要管理员权限；点“管理员扫描”将系统项目合并扫描");
        return;
    }
    ComScope com;
    if (FAILED(com.result)) {
        group.state = static_cast<LONG>(State::Unavailable); group.failed = 1;
        wcscpy_s(group.note, L"Windows 清理组件无法初始化"); return;
    }
    auto progress = new Progress(cancel, clean ? 15 * 60 * 1000 : 90 * 1000);
    for (const auto& category : categories) {
        if (FAILED(progress->status())) { group.state = static_cast<LONG>(State::Canceled); break; }
        // A cleanup can only execute a handler that was measured in the consumed preview.
        bool approved = false;
        for (DWORD i = 0; i < std::min<DWORD>(scanned.previewSize, previewCount); ++i)
            if (scanned.previews[i].bytes && !wcscmp(scanned.previews[i].path, category.name)) approved = true;
        if (clean && !approved) continue;
        Handler handler;
        HRESULT hr = handler.open(category);
        DWORDLONG bytes = 0;
        if (hr == S_OK) hr = handler.cache->GetSpaceUsed(&bytes, progress);
        if (hr == S_FALSE || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) continue;
        auto& preview = group.previews[group.previewSize++];
        wcsncpy_s(preview.path, category.name, _TRUNCATE);
        if (FAILED(hr)) {
            ++group.failed;
            swprintf_s(group.note, L"%s 未完成（0x%08lX）；可重试扫描", category.name, static_cast<DWORD>(hr));
            continue;
        }
        preview.bytes = bytes; group.bytes += bytes; group.eligible += bytes;
        if (bytes) ++group.candidates;
        if (clean && bytes && SUCCEEDED(progress->status())) {
            hr = handler.cache->Purge(bytes, progress);
            if (FAILED(hr)) {
                ++group.failed;
                swprintf_s(group.note, L"%s 清理未完成（0x%08lX）；请重新扫描", category.name, static_cast<DWORD>(hr));
            }
            else {
                // Windows owns the actual file list. Do not invent deleted-file counts.
                DWORDLONG after = 0;
                if (handler.cache->GetSpaceUsed(&after, progress) == S_OK && bytes >= after) group.removed += bytes - after;
                else wcscpy_s(group.note, L"Windows 已执行清理；释放量未能复核，可能需要重启");
            }
        }
    }
    bool stopped = FAILED(progress->status()); progress->Release();
    if (stopped) {
        ++group.failed;
        wcscpy_s(group.note, L"Windows 处理已停止或超时；部分操作可能已完成，请重新扫描");
    }
    group.state = static_cast<LONG>(stopped ? State::Canceled : group.failed ? State::Partial :
        clean ? State::Complete : group.candidates ? State::Ready : State::Empty);
    if (!*group.note) wcscpy_s(group.note, clean ? L"Windows 已处理所选项目；部分空间可能在重启后释放" :
        L"由 Windows 处理更新、传递优化与临时安装文件；不含下载、回收站和旧系统");
}
} // namespace storage
