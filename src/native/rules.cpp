// Selected Windows rule paths adapted from tw93/Mole (GPL-3.0).
// Upstream: 3423ee037774fec5b20ec63b912a6df65a79675d. See work/mole-windows-rules.md.
#include "storage.h"
#include <stdexcept>

namespace storage {
const std::array<Spec, groupCount> specs{{
    {L"user", L"临时文件", L"", Kind::Temp},
    {L"windows", L"系统临时文件", L"", Kind::Temp},
    {L"dump", L"崩溃转储", L"", Kind::Temp},
    {L"updater", L"更新残留", L"", Kind::Temp},
    {L"directx", L"DirectX 缓存", L"", Kind::Shader},
    {L"nvidia", L"NVIDIA 缓存", L"", Kind::Shader},
    {L"chrome", L"Chrome 缓存", L"chrome.exe", Kind::Browser},
    {L"edge", L"Edge 缓存", L"msedge.exe", Kind::Browser},
    {L"code", L"VS Code 缓存", L"Code.exe;cpptools.exe", Kind::Browser},
    {L"cursor", L"Cursor 缓存", L"Cursor.exe", Kind::Browser},
    {L"pip", L"pip 缓存（工具维护）", L"pip.exe;python.exe;pythonw.exe", Kind::Package},
    {L"npm", L"npm 缓存（工具维护）", L"node.exe", Kind::Package},
    {L"uv", L"uv 缓存（工具维护）", L"uv.exe;python.exe;pythonw.exe", Kind::Package},
    {L"firefox", L"Firefox 缓存", L"firefox.exe", Kind::Browser},
    {L"brave", L"Brave 缓存", L"brave.exe", Kind::Browser},
    {L"vivaldi", L"Vivaldi 缓存", L"vivaldi.exe", Kind::Browser},
    {L"opera", L"Opera 缓存", L"opera.exe", Kind::Browser},
    {L"discord", L"Discord 缓存", L"Discord.exe;DiscordCanary.exe;DiscordPTB.exe", Kind::Browser},
    {L"slack", L"Slack 缓存", L"slack.exe", Kind::Browser},
    {L"teams", L"Teams 缓存", L"Teams.exe;ms-teams.exe;msedgewebview2.exe", Kind::Browser},
    {L"windsurf", L"Windsurf 缓存", L"Windsurf.exe", Kind::Browser},
    {L"obsidian", L"Obsidian 缓存", L"Obsidian.exe", Kind::Browser},
    {L"notion", L"Notion 缓存", L"Notion.exe", Kind::Browser},
    {L"code-insiders", L"VS Code Insiders 缓存", L"Code - Insiders.exe;cpptools.exe", Kind::Browser},
    {L"amd", L"AMD 着色器缓存", L"", Kind::Shader},
    {L"intel", L"Intel 着色器缓存", L"", Kind::Shader},
    {L"yarn", L"Yarn 缓存", L"node.exe;yarn.exe", Kind::Temp},
    {L"bun", L"Bun 安装缓存", L"bun.exe;node.exe", Kind::Temp},
    {L"nuget", L"NuGet 下载缓存", L"dotnet.exe;nuget.exe;devenv.exe;MSBuild.exe", Kind::Temp},
    {L"node-build", L"Node 构建工具缓存", L"node.exe;electron.exe", Kind::Temp},
    {L"zed", L"Zed 缓存", L"zed.exe", Kind::Temp},
    {L"windows-maintenance", L"Windows 系统清理", L"", Kind::System}
}};

namespace {
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
bool absoluteLocal(const std::wstring& path) {
    return path.size() > 3 && iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\' &&
        path.find(L':', 2) == std::wstring::npos && path.find(L"..") == std::wstring::npos;
}
std::wstring base(const wchar_t* variable) {
    auto path = environment(variable);
    if (!absoluteLocal(path) || !plainPath(path)) throw std::runtime_error("Unsafe rule base path");
    return fullPath(path);
}
struct Discovery {
    std::vector<std::wstring> roots;
    bool partial = false;
    void add(const std::wstring& path) {
        if (!absoluteLocal(path) || !plainPath(path)) { partial = true; return; }
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) { if (!missing(GetLastError())) partial = true; return; }
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))) {
            partial = true; return;
        }
        auto full = fullPath(path);
        if (std::none_of(roots.begin(), roots.end(), [&](const std::wstring& r) { return !_wcsicmp(r.c_str(), full.c_str()); })) roots.push_back(full);
    }
    void leaves(const std::wstring& path, bool code = false) {
        for (auto leaf : {L"Cache", L"Code Cache", L"GPUCache"}) add(join(path, leaf));
        if (code) for (auto leaf : {L"CachedData", L"CachedExtensions", L"CachedExtensionVSIXs"}) add(join(path, leaf));
    }
    // Bound directory discovery, not only the number of accepted profiles.
    void children(const std::wstring& path, const std::function<void(const std::wstring&, const std::wstring&)>& visit) {
        if (!plainPath(path)) { partial = true; return; }
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(join(path, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) { if (!missing(GetLastError())) partial = true; return; }
        unsigned count = 0;
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            if (++count > 128) { partial = true; break; }
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) { partial = true; continue; }
            visit(join(path, data.cFileName), data.cFileName);
        } while (FindNextFileW(find, &data));
        if (count <= 128 && GetLastError() != ERROR_NO_MORE_FILES) partial = true;
        FindClose(find);
    }
    void chromium(const std::wstring& path) {
        children(path, [&](const std::wstring& p, const std::wstring& name) {
            if (name == L"Default" || (name.size() > 8 && name.substr(0, 8) == L"Profile " &&
                std::all_of(name.begin() + 8, name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))) leaves(p);
        });
    }
};
bool locked(const std::wstring& path) {
    if (!plainPath(path)) return true;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return !missing(GetLastError());
    if (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return true;
    Handle file(CreateFileW(path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return !file;
}
}

std::vector<std::wstring> ruleRoots(int id, bool* incomplete) {
    Discovery d;
    auto local = base(L"LOCALAPPDATA"), roaming = base(L"APPDATA"), home = base(L"USERPROFILE");
    auto l = [&](const wchar_t* path) { d.add(join(local, path)); };
    switch (id) {
    case 0: l(L"Temp"); break; // Keep existing narrow root; never trust a broad custom TEMP.
    case 1: d.add(join(base(L"SystemRoot"), L"Temp")); break;
    case 2: l(L"CrashDumps"); break;
    case 3: l(L"SquirrelTemp"); break;
    case 4: l(L"D3DSCache"); l(L"Microsoft\\DirectX Shader Cache"); l(L"VulkanCache"); break;
    case 5: l(L"NVIDIA\\DXCache"); l(L"NVIDIA\\GLCache"); l(L"NVIDIA Corporation\\NV_Cache"); break;
    case 6: d.chromium(join(local, L"Google\\Chrome\\User Data")); break;
    case 7: d.chromium(join(local, L"Microsoft\\Edge\\User Data")); break;
    case 8: d.leaves(join(roaming, L"Code"), true); break;
    case 9: d.leaves(join(roaming, L"Cursor")); break;
    case 10: l(L"pip\\cache"); break;
    case 11: l(L"npm-cache\\_cacache"); break;
    case 12: l(L"uv\\cache"); break;
    case 13:
        d.children(join(local, L"Mozilla\\Firefox\\Profiles"), [&](const std::wstring& p, const std::wstring&) {
            d.add(join(p, L"cache2")); d.add(join(p, L"startupCache"));
        }); break;
    case 14: d.chromium(join(local, L"BraveSoftware\\Brave-Browser\\User Data")); break;
    case 15: d.chromium(join(local, L"Vivaldi\\User Data")); break;
    case 16:
        for (auto& root : {local, roaming}) {
            auto opera = join(root, L"Opera Software\\Opera Stable");
            d.leaves(opera); d.chromium(opera);
        } break;
    case 17: d.leaves(join(roaming, L"discord")); break;
    case 18: d.leaves(join(roaming, L"Slack")); break;
    case 19:
        d.add(join(roaming, L"Microsoft\\Teams\\Cache")); d.add(join(roaming, L"Microsoft\\Teams\\GPUCache"));
        d.chromium(join(local, L"Packages\\MSTeams_8wekyb3d8bbwe\\LocalCache\\Microsoft\\MSTeams\\EBWebView")); break;
    case 20: d.leaves(join(roaming, L"Windsurf")); break;
    case 21: d.leaves(join(roaming, L"obsidian")); break;
    case 22: d.leaves(join(roaming, L"Notion")); break;
    case 23: d.leaves(join(roaming, L"Code - Insiders"), true); break;
    case 24: l(L"AMD\\DxCache"); l(L"AMD\\GLCache"); l(L"AMD\\VkCache"); break;
    case 25: l(L"Intel\\ShaderCache"); d.add(join(roaming, L"Intel\\ShaderCache")); break;
    case 26: l(L"Yarn\\Cache"); d.add(join(home, L".yarn\\cache")); break;
    case 27: d.add(join(home, L".bun\\install\\cache")); break;
    case 28: l(L"NuGet\\v3-cache"); l(L"NuGet\\plugins-cache"); break;
    case 29: l(L"node-gyp\\Cache"); l(L"electron\\Cache"); l(L"TypeScript"); break;
    case 30: l(L"Zed\\cache"); d.add(join(roaming, L"Zed\\cache")); break;
    default: d.partial = true;
    }
    if (incomplete) *incomplete = d.partial;
    return d.roots;
}

bool ruleLocked(int id) {
    if (specs[id].kind != Kind::Browser) return false;
    bool incomplete = false;
    auto roots = ruleRoots(id, &incomplete);
    if (incomplete) return true;
    for (auto& root : roots) {
        auto profile = parent(root);
        // Chromium/Electron locks may live in either the profile or its parent.
        for (auto& dir : {profile, parent(profile)})
            for (auto name : {L"lockfile", L"SingletonLock"}) if (locked(join(dir, name))) return true;
        if (id == 13) {
            auto profileName = profile.substr(profile.find_last_of(L'\\') + 1);
            auto owner = join(join(base(L"APPDATA"), L"Mozilla\\Firefox\\Profiles"), profileName);
            if (locked(join(owner, L"parent.lock"))) return true;
        }
    }
    return false;
}
} // namespace storage
