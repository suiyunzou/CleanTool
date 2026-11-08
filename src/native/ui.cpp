#include "storage.h"
#include <windowsx.h>
#include <uxtheme.h>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace storage {
namespace {
constexpr COLORREF canvas = RGB(248, 249, 251), surface = RGB(255, 255, 255);
constexpr COLORREF ink = RGB(37, 48, 68), muted = RGB(116, 128, 148), accent = RGB(49, 95, 189);
constexpr COLORREF navigationSurface = RGB(245, 245, 247), navigationInk = RGB(60, 60, 67);
constexpr UINT timerId = 1;
constexpr int layoutId = 115;
constexpr int navOverview = 101, navHistory = 102, navTools = 103, primaryId = 110, resetId = 111, folderId = 112;
constexpr int previousId = 113, nextId = 114;
constexpr int searchId = 116, filterId = 117, selectId = 118, filesTab = 119, ruleTab = 120;
constexpr int copyId = 121, toggleId = 122, expandId = 123, noteId = 125, detailId = 126;
constexpr int groupBase = 200, toolBase = 300, historyBase = 400;
void shellOpen(HWND owner, const wchar_t* target, const wchar_t* args = nullptr) {
    HMODULE module = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::runtime_error("Cannot load Windows shell");
    auto open = reinterpret_cast<decltype(&ShellExecuteW)>(GetProcAddress(module, "ShellExecuteW"));
    auto result = open ? open(owner, L"open", target, args, nullptr, SW_SHOWNORMAL) : nullptr;
    FreeLibrary(module);
    if (reinterpret_cast<INT_PTR>(result) <= 32) throw std::runtime_error("Cannot open location or tool");
}
struct History { std::wstring path, label, note, details; uint64_t bytes = 0; bool interrupted = false; };
struct NoteEditor {
    std::wstring value;
    static INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_INITDIALOG) {
            auto self = reinterpret_cast<NoteEditor*>(l); SetWindowLongPtrW(dialog, DWLP_USER, l);
            SetWindowTextW(dialog, L"编辑记录备注");
            RECT r{}; GetClientRect(dialog, &r);
            auto edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", self->value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                12, 12, r.right - 24, r.bottom - 64, dialog, reinterpret_cast<HMENU>(100), nullptr, nullptr);
            SendMessageW(edit, EM_LIMITTEXT, 1000, 0);
            CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, r.right-180,r.bottom-40,76,28,dialog,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
            CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, r.right-92,r.bottom-40,76,28,dialog,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
            for (int id : {100, IDOK, IDCANCEL}) SendMessageW(GetDlgItem(dialog,id),WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            SetFocus(edit); return FALSE;
        }
        if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
            if (LOWORD(w) == IDOK) {
                auto self = reinterpret_cast<NoteEditor*>(GetWindowLongPtrW(dialog,DWLP_USER));
                wchar_t text[1001]{}; GetDlgItemTextW(dialog,100,text,1001); self->value=text;
            }
            EndDialog(dialog,LOWORD(w));return TRUE;
        }
        if (message == WM_CLOSE) { EndDialog(dialog,IDCANCEL);return TRUE; }
        return FALSE;
    }
    bool show(HWND owner) {
        struct Template { DLGTEMPLATE dialog; WORD menu, cls, title; } data{};
        data.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;
        data.dialog.cx=250; data.dialog.cy=130;
        return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&data.dialog,owner,procedure,reinterpret_cast<LPARAM>(this))==IDOK;
    }
};

class Ui {
    HWND window = nullptr, tip = nullptr;
    HWND search = nullptr, detail = nullptr;
    std::wstring query, detailValue, toast;
    ULONGLONG toastUntil = 0;
    int filter = 0, detailTab = 0, historyIndex = 0, historyFirst = 0;
    bool expanded = false, toastError = false;
    std::vector<int> filtered;
    std::vector<HWND> buttons;
    std::array<HWND, groupCount> groupButtons{};
    std::array<HWND, 6> toolButtons{};
    std::vector<HWND> historyButtons;
    std::vector<History> history;
    std::unique_ptr<Task> task;
    Shared result{};
    int firstGroup = 0, visibleGroups = 13;
    int page = 0, focusedGroup = 0, dpi = 96, width = 900, height = 640;
    uint32_t selection = 0;
    bool demo = false, ready = false, finished = false, stopping = false, hidden = false, motion = true;
    bool sideNavigation = true;
    Operation operation = Operation::Scan;
    std::wstring session, message, reportPath;
    std::array<RECT, 31> tipAreas{};
    UINT_PTR tipTarget = 0;
    POINT tipPoint{};
    ULONGLONG tipSince = 0;
    HFONT normal = nullptr, large = nullptr, menuFont = nullptr, selectedMenuFont = nullptr;
    ULONGLONG frame = 0, lastDisk = 0, taskStarted = 0;
    uint64_t free[2]{}, total[2]{};
    HBITMAP buffer = nullptr;
    HDC memory = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    int bufferWidth = 0, bufferHeight = 0;
    int px(int value) const { return MulDiv(value, dpi, 96); }
    int contentLeft() const { return sideNavigation ? px(164) : 0; }
    int contentWidth() const { return width - contentLeft(); }
    RECT menuPlate(RECT box) const {
        int middle = (box.top + box.bottom) / 2;
        return {box.left + px(4), middle - px(15), box.right - px(4), middle + px(15)};
    }
    void toggleNavigation() {
        sideNavigation = !sideNavigation;
        setName(layoutId, sideNavigation ? L"菜单切换到顶部 · Ctrl+B" : L"菜单切换到左侧 · Ctrl+B");
        layout();
    }
    bool working() const { return task && taskStarted != 0; }
    bool servicing() const {
        return working() && (operation == Operation::ComponentClean ||
            (operation == Operation::Clean && task->shared() && task->shared()->active == 31));
    }
    bool usable(int i) const {
        auto& g = result.groups[i];
        return (g.state == static_cast<LONG>(State::Ready) || g.state == static_cast<LONG>(State::Partial)) &&
            (g.candidates || (g.kind == Kind::Package && g.bytes));
    }
    void fonts() {
        if (normal) DeleteObject(normal);
        if (large) DeleteObject(large);
        if (menuFont) DeleteObject(menuFont);
        if (selectedMenuFont) DeleteObject(selectedMenuFont);
        normal = CreateFontW(-px(13), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        large = CreateFontW(-px(26), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        menuFont = CreateFontW(-px(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        selectedMenuFont = CreateFontW(-px(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (HWND control : {search, detail}) if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(normal), FALSE);
    }
    void regionTip(UINT_PTR id, RECT rect) {
        tipAreas.at(id - 600) = rect;
    }
    std::wstring tipText(UINT_PTR id) const {
        if (id >= 600 && id <= 601) {
            int i = static_cast<int>(id - 600);
            return std::wstring(i ? L"D" : L"C") + L" · 可用 " + sizeText(free[i]) + L" / 总计 " + sizeText(total[i]);
        }
        if (id >= 610 && id < 610 + previewCount) {
            int i = static_cast<int>(id - 610);
            return i < static_cast<int>(result.groups[focusedGroup].previewSize) ? result.groups[focusedGroup].previews[i].path : L"";
        }
        if (id == 630) {
            uint64_t deleted = 0, skipped = 0, failed = 0;
            for (auto& g : result.groups) { deleted += g.deleted; skipped += g.skipped; failed += g.failed; }
            return (result.canceled ? std::wstring(L"已停止 · ") : L"") +
                L"删除 " + std::to_wstring(deleted) + L" · 保留 " + std::to_wstring(skipped) + L" · 失败 " + std::to_wstring(failed);
        }
        wchar_t name[512]{};
        GetWindowTextW(GetDlgItem(window, static_cast<int>(id)), name, 512);
        return name;
    }
    void hover() {
        if (hidden) return;
        POINT point{}; GetCursorPos(&point);
        UINT_PTR target = 0;
        if (GetForegroundWindow() == window && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            HWND control = WindowFromPoint(point);
            if (IsChild(window, control)) target = GetDlgCtrlID(control);
            else if (control == window) {
                POINT client = point; ScreenToClient(window, &client);
                for (size_t i = 0; i < tipAreas.size(); ++i) if (PtInRect(&tipAreas[i], client)) { target = 600 + i; break; }
            }
        }
        auto now = GetTickCount64();
        if (target != tipTarget || abs(point.x - tipPoint.x) > px(3) || abs(point.y - tipPoint.y) > px(3)) {
            tipTarget = target; tipPoint = point; tipSince = now; ShowWindow(tip, SW_HIDE); return;
        }
        if (!target || now - tipSince > 16000) { ShowWindow(tip, SW_HIDE); return; }
        if (now - tipSince < 500 || IsWindowVisible(tip)) return;
        auto value = tipText(target);
        if (value.empty()) return;
        RECT box{0, 0, px(460), 0};
        HDC dc = GetDC(tip); auto old = SelectObject(dc, normal);
        DrawTextW(dc, value.c_str(), -1, &box, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old); ReleaseDC(tip, dc);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
        int x = std::clamp<int>(point.x, monitor.rcWork.left, std::max<int>(monitor.rcWork.left, monitor.rcWork.right - box.right - px(8)));
        int y = point.y + px(22);
        if (y + box.bottom + px(8) > monitor.rcWork.bottom) y = point.y - box.bottom - px(8);
        SendMessageW(tip, WM_SETFONT, reinterpret_cast<WPARAM>(normal), FALSE);
        SetWindowTextW(tip, value.c_str());
        SetWindowPos(tip, HWND_TOPMOST, x, y, box.right + px(8), box.bottom + px(8), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    static LRESULT CALLBACK groupProcedure(HWND control, UINT message, WPARAM w, LPARAM l) {
        auto previous = reinterpret_cast<WNDPROC>(GetPropW(control, L"CleanTool.ButtonProc"));
        RECT r{}; GetClientRect(control, &r);
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        bool toggle = message == WM_LBUTTONUP && GetCapture() == control && PtInRect(&r, point) && point.x < r.right / 8;
        LRESULT value = CallWindowProcW(previous, control, message, w, l);
        if (toggle) SendMessageW(GetParent(control), WM_COMMAND, toggleId, 0);
        if (message == WM_NCDESTROY) RemovePropW(control, L"CleanTool.ButtonProc");
        return value;
    }
    HWND button(int id, const wchar_t* name) {
        HWND b = CreateWindowExW(0, L"BUTTON", name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!b) throw std::runtime_error("Cannot create control");
        if (id >= groupBase && id < groupBase + groupCount) {
            auto previous = SetWindowLongPtrW(b, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(groupProcedure));
            SetPropW(b, L"CleanTool.ButtonProc", reinterpret_cast<HANDLE>(previous));
        }
        buttons.push_back(b); return b;
    }
    void setName(int id, const std::wstring& name) {
        HWND control = GetDlgItem(window, id);
        wchar_t current[512]{};
        GetWindowTextW(control, current, 512);
        if (name != current) SetWindowTextW(control, name.c_str());
    }
    void discardBuffer() {
        if (memory && oldBitmap) SelectObject(memory, oldBitmap);
        if (buffer) DeleteObject(buffer);
        if (memory) DeleteDC(memory);
        memory = nullptr; buffer = nullptr; oldBitmap = nullptr; bufferWidth = bufferHeight = 0;
    }
    static void fill(HDC dc, RECT r, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &r, brush); DeleteObject(brush);
    }
    void rounded(HDC dc, RECT r, COLORREF color, int radius = 12) {
        HBRUSH brush = CreateSolidBrush(color);
        auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, r.left, r.top, r.right, r.bottom, px(radius), px(radius));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush);
    }
    void text(HDC dc, const std::wstring& value, RECT rect, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
        auto old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, flags | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    void symbol(HDC dc, int type, int x, int y, int size, COLORREF color) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, px(2)), color);
        auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        auto point = [&](int a, int b) { return POINT{x + a * size / 24, y + b * size / 24}; };
        auto line = [&](int a, int b, int c, int d) { auto p = point(a, b), q = point(c, d); MoveToEx(dc, p.x, p.y, nullptr); LineTo(dc, q.x, q.y); };
        auto rect = [&](int a, int b, int c, int d) { auto p = point(a, b), q = point(c, d); Rectangle(dc, p.x, p.y, q.x, q.y); };
        switch (type) {
        case 0: { auto a = point(4, 4), b = point(20, 20); Ellipse(dc, a.x, a.y, b.x, b.y); line(16, 4, 21, 4); line(21, 4, 21, 9); break; }
        case 1: rect(6, 6, 18, 18); break;
        case 2: line(17, 2, 10, 12); line(4, 12, 15, 19); line(4, 12, 2, 18); line(2, 18, 12, 23); line(12, 23, 15, 19); break;
        case 3: { auto a = point(3, 3), b = point(21, 21); Ellipse(dc, a.x, a.y, b.x, b.y); line(12, 6, 12, 12); line(12, 12, 16, 14); break; }
        case 4: rect(5, 5, 19, 19); line(9, 1, 9, 5); line(15, 1, 15, 5); line(9, 19, 9, 23); line(15, 19, 15, 23); line(1, 9, 5, 9); line(19, 9, 23, 9); break;
        case 5: line(3, 7, 10, 7); line(10, 7, 12, 10); line(12, 10, 21, 10); line(21, 10, 21, 20); line(21, 20, 3, 20); line(3, 20, 3, 7); break;
        case 6: line(4, 12, 10, 18); line(10, 18, 21, 5); break;
        case 7: line(12, 3, 22, 21); line(22, 21, 2, 21); line(2, 21, 12, 3); line(12, 9, 12, 14); line(12, 17, 12, 18); break;
        case 8: rect(3, 5, 21, 20); line(3, 15, 21, 15); line(6, 18, 8, 18); break;
        case 9: rect(3, 3, 10, 10); rect(14, 3, 21, 10); rect(3, 14, 10, 21); rect(14, 14, 21, 21); break;
        case 10: rect(6, 7, 18, 21); line(3, 6, 21, 6); line(8, 3, 16, 3); line(10, 10, 10, 18); line(14, 10, 14, 18); break;
        case 12: rect(2, 3, 22, 21); line(8, 3, 8, 21); break;
        case 13: rect(2, 3, 22, 21); line(2, 9, 22, 9); break;
        default: rect(5, 3, 19, 21); line(9, 8, 15, 8); line(9, 12, 15, 12); break;
        }
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    }
    void refreshDisks() {
        for (int i = 0; i != 2; ++i) {
            ULARGE_INTEGER a{}, t{}, f{};
            if (GetDiskFreeSpaceExW(i ? L"D:\\" : L"C:\\", &a, &t, &f)) { free[i] = a.QuadPart; total[i] = t.QuadPart; }
        }
        lastDisk = GetTickCount64();
    }
    const wchar_t* status(int id) const {
        auto state = static_cast<State>(result.groups[id].state);
        switch (state) {
        case State::Ready: return L"可清理";
        case State::Partial: return L"部分结果";
        case State::InUse: return L"使用中";
        case State::Unavailable: return L"需权限 / 不可用";
        case State::Canceled: return L"已停止";
        case State::Complete: return L"已完成";
        default: return ready ? L"无可清理项" : L"待扫描";
        }
    }
    uint64_t selectedBytes() const {
        uint64_t value = 0;
        for (int i = 0; i < groupCount; ++i) if (selection & (1U << i))
            value += result.groups[i].kind == Kind::Package ? result.groups[i].bytes : result.groups[i].eligible;
        return value;
    }
    void notify(const std::wstring& value, bool error = false) {
        toast = value; toastError = error; toastUntil = GetTickCount64() + 5000; InvalidateRect(window, nullptr, FALSE);
    }
    void filterRows() {
        filtered.clear();
        auto lower = [](std::wstring value) { for (auto& ch : value) ch = static_cast<wchar_t>(towlower(ch)); return value; };
        auto term = lower(query);
        for (int i = 0; i < groupCount; ++i) {
            if (!term.empty() && lower(specs[i].name).find(term) == std::wstring::npos && lower(result.groups[i].note).find(term) == std::wstring::npos) continue;
            if ((filter == 1 && !usable(i)) || (filter == 2 && usable(i)) || (filter == 3 && specs[i].kind != Kind::System)) continue;
            filtered.push_back(i);
        }
        if (std::find(filtered.begin(), filtered.end(), focusedGroup) == filtered.end() && !filtered.empty()) focusedGroup = filtered.front();
    }
    void updateDetail() {
        std::wstring value;
        if (page == 1 && !history.empty()) {
            auto& row = history[historyIndex];
            value = row.label + L"\r\n\r\n已记录清理量  " + sizeText(row.bytes) +
                (row.interrupted ? L"\r\n状态：中断或部分完成" : L"\r\n状态：已完成") +
                L"\r\n\r\n记录值不等同于磁盘可用空间增量。\r\n\r\n备注\r\n" + (row.note.empty() ? L"暂无备注" : row.note) + row.details + L"\r\n\r\n记录文件\r\n" + row.path;
        } else if (page == 0 && ready && !filtered.empty()) {
            auto& g = result.groups[focusedGroup];
            value = std::wstring(specs[focusedGroup].name) + L"\r\n" + status(focusedGroup) + L"  ·  " +
                sizeText(g.kind == Kind::Package ? g.bytes : g.eligible) + L"\r\n\r\n" + g.note + L"\r\n\r\n";
            if (detailTab) {
                value += L"扫描范围\r\n";
                if (g.kind == Kind::System) value += L"Windows 更新清理\r\n传递优化缓存\r\n临时安装文件\r\n\r\n使用 Windows 官方清理组件。清理后可能需要重启；旧更新可能无法卸载。";
                else {
                    for (auto& root : ruleRoots(focusedGroup)) value += root + L"\r\n";
                    value += L"\r\n保护规则\r\n保留使用中的文件、链接、虚拟磁盘。\r\n普通文件创建和修改均超过 7 天才进入候选。\r\n清理前再次核对路径、文件身份和修改时间。";
                    if (g.kind == Kind::Package) value += L"\r\n包缓存由原工具清理，不采用 7 天限制。";
                    if (g.kind == Kind::Shader) value += L"\r\n清理后，游戏可能需要重新编译着色器。";
                }
            } else {
                value += g.kind == Kind::System ? L"Windows 项目\r\n" : L"文件预览（最多 12 项）\r\n";
                for (DWORD i = 0; i < std::min<DWORD>(g.previewSize, previewCount); ++i) {
                    std::wstring path = g.previews[i].path;
                    if (!expanded && g.kind != Kind::System) path = path.substr(path.find_last_of(L"\\/") + 1);
                    value += sizeText(g.previews[i].bytes) + L"  " + path + L"\r\n\r\n";
                }
                if (!g.previewSize) value += L"没有可显示的文件。";
            }
        }
        if (value != detailValue) { detailValue = value; SetWindowTextW(detail, value.c_str()); }
    }
    void drive(HDC dc, int i, RECT rect) {
        rounded(dc, rect, surface);
        text(dc, i ? L"D 盘 · 可用空间" : L"C 盘 · 可用空间", {rect.left + px(16), rect.top + px(10), rect.right - px(16), rect.top + px(30)}, normal, muted);
        text(dc, total[i] ? sizeText(free[i]) : L"未连接", {rect.left + px(16), rect.top + px(31), rect.right - px(16), rect.top + px(63)}, large, ink);
        RECT bar{rect.left + px(16), rect.bottom - px(14), rect.right - px(16), rect.bottom - px(10)};
        fill(dc, bar, RGB(233,237,243));
        auto used = total[i] ? std::clamp(1.0 - static_cast<double>(free[i]) / total[i], 0.0, 1.0) : 0;
        bar.right = bar.left + static_cast<int>((bar.right - bar.left) * used); fill(dc, bar, accent);
    }
    void paint(HDC dc) {
        fill(dc, {0,0,width,height}, canvas);
        int inset = contentLeft(), left = inset + px(20), right = width - px(20);
        fill(dc, {0,0,inset,height}, navigationSurface);
        fill(dc, {inset,0,width,px(64)}, sideNavigation ? surface : navigationSurface);
        if (sideNavigation) text(dc, L"CleanTool", {px(16),height-px(44),inset-px(8),height-px(16)}, normal, muted);
        if (page == 0) {
            int third = (right-left-px(24))/3;
            drive(dc,0,{left,px(80),left+third,px(172)});
            drive(dc,1,{left+third+px(12),px(80),left+third*2+px(12),px(172)});
            RECT summary{left+third*2+px(24),px(80),right,px(172)};
            rounded(dc,summary,surface);
            text(dc, finished ? L"已记录清理量" : L"已选清理量", {summary.left+px(16),px(90),right-px(12),px(110)},normal,muted);
            uint64_t amount = selectedBytes();
            if (finished) { amount=0; for (auto& g:result.groups) amount += g.removed; }
            text(dc,sizeText(amount),{summary.left+px(16),px(115),right-px(12),px(151)},large,accent);
            RECT body{left,px(226),right,height-px(70)};
            rounded(dc,body,surface);
            if (ready) {
                fill(dc,{left+px(302),body.top,left+px(303),body.bottom},RGB(227,231,237));
                if (filtered.empty()) text(dc,L"没有符合条件的项目",{left,body.top+px(60),right,body.bottom},normal,muted,DT_CENTER|DT_TOP|DT_SINGLELINE);
            } else {
                int cy=(body.top+body.bottom)/2;
                std::wstring title = L"扫描应用缓存与 Windows 清理项目";
                std::wstring description = L"扫描后统一选择，一次确认清理。";
                if (working()) {
                    auto state = task->shared();
                    title = operation == Operation::Scan ? L"正在扫描" : stopping ? L"正在停止" : operation == Operation::Analyze ? L"正在分析系统组件" : L"正在清理";
                    if (state && state->active >= 0 && state->active < groupCount) description = specs[state->active].name;
                    RECT bar{left+px(48),cy+px(55),right-px(48),cy+px(59)};
                    fill(dc,bar,RGB(233,237,243));
                    int len=(bar.right-bar.left)/4, offset=motion ? static_cast<int>(frame*px(5)%std::max<int>(1,bar.right-bar.left-len)) : 0;
                    fill(dc,{bar.left+offset,bar.top,bar.left+offset+len,bar.bottom},accent);
                } else if (finished) {
                    uint64_t failures=0, skipped=0; for(auto& g:result.groups){failures+=g.failed;skipped+=g.skipped;}
                    title = result.error ? L"清理未完成" : result.canceled ? L"清理已停止" : failures ? L"部分项目未完成" : L"清理完成";
                    description = L"保留 " + std::to_wstring(skipped) + L" 项 · 失败 " + std::to_wstring(failures) + L" 项；重新扫描可查看剩余内容。";
                    if (operation == Operation::Analyze || operation == Operation::ComponentClean) {
                        title = result.error ? L"系统操作未完成" : result.canceled ? L"系统操作已停止" : operation == Operation::Analyze ? L"组件分析完成" : L"组件清理完成";
                        description = std::wstring(result.message) + L" · 详细结果已保存到日志目录。";
                    }
                } else if (!message.empty()) { title=L"操作未完成";description=message; }
                text(dc,title,{left+px(24),cy-px(30),right-px(24),cy+px(4)},menuFont,ink,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                text(dc,description,{left+px(24),cy+px(10),right-px(24),cy+px(42)},normal,muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }
            text(dc,ready ? L"勾选清理范围；使用中的项目会保留。" : L"下载、回收站、个人文件和旧系统不自动清理。",
                {left,height-px(33),right,height-px(10)},normal,muted);
        } else {
            rounded(dc,{left,px(80),right,height-px(70)},surface);
            if (page == 1) {
                if (history.empty()) text(dc,L"暂无清理记录",{left,px(160),right,height-px(100)},normal,muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                else fill(dc,{left+px(302),px(80),left+px(303),height-px(70)},RGB(227,231,237));
            } else {
                text(dc,L"常用维护",{left+px(20),px(96),right-px(20),px(126)},menuFont,ink);
                text(dc,L"Windows 清理项目已合并到首页扫描。",{left+px(20),px(130),right-px(20),px(158)},normal,muted);
                text(dc,L"组件清理与卸载会改变系统，请按需使用。",{left+px(20),px(450),right-px(20),px(478)},normal,muted);
            }
        }
        if (!toast.empty() && GetTickCount64()<toastUntil) {
            RECT box{std::max(left,right-px(430)),height-px(65),right,height-px(27)};
            rounded(dc,box,toastError?RGB(254,232,232):RGB(229,247,236)); text(dc,toast,{box.left+px(12),box.top,box.right-px(12),box.bottom},normal,toastError?RGB(199,53,53):RGB(22,131,74));
        }
    }
    void drawButton(DRAWITEMSTRUCT& item) {
        int id=static_cast<int>(item.CtlID);
        bool focus=(item.itemState&ODS_FOCUS)!=0, disabled=(item.itemState&ODS_DISABLED)!=0, down=(item.itemState&ODS_SELECTED)!=0;
        bool nav=id>=navOverview&&id<=navTools, group=id>=groupBase&&id<groupBase+groupCount;
        bool selected=nav ? id==navOverview+page : group ? id==groupBase+focusedGroup :
            id>=historyBase ? id==historyBase+historyIndex : id==filesTab ? detailTab==0 : id==ruleTab && detailTab==1;
        COLORREF background = nav || id == layoutId || (!sideNavigation && (id == primaryId || id == resetId)) ? navigationSurface : surface;
        fill(item.hDC,item.rcItem,background);
        RECT box=item.rcItem; InflateRect(&box,-px(2),-px(2));
        bool primary=id==primaryId;
        COLORREF buttonColor = primary ? (down ? RGB(208,208,213) : RGB(229,229,234)) :
            nav ? (selected ? surface : navigationSurface) : id == layoutId ? navigationSurface :
            selected || down ? RGB(234,240,252) : surface;
        rounded(item.hDC,box,buttonColor,primary ? 18 : 8);
        COLORREF color=disabled ? muted : nav ? (selected ? RGB(0,0,0) : navigationInk) : primary ? ink : selected ? accent : ink;
        if(group){
            int i=id-groupBase; bool checked=(selection&(1U<<i))!=0;
            RECT check{box.left+px(8),box.top+px(13),box.left+px(24),box.top+px(29)};
            rounded(item.hDC,check,checked ? accent : RGB(227,231,237),4);
            if(checked) symbol(item.hDC,6,check.left,check.top,px(15),surface);
            text(item.hDC,specs[i].name,{box.left+px(34),box.top+px(2),box.right-px(8),box.top+px(24)},normal,color);
            text(item.hDC,sizeText(result.groups[i].kind==Kind::Package?result.groups[i].bytes:result.groups[i].eligible),
                {box.left+px(34),box.top+px(23),box.right-px(110),box.bottom-px(2)},normal,muted);
            RECT badge{box.right-px(108),box.top+px(23),box.right-px(6),box.bottom-px(2)};
            rounded(item.hDC,badge,usable(i)?RGB(229,247,236):RGB(238,240,243),10);
            text(item.hDC,status(i),badge,normal,usable(i)?RGB(22,131,74):muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        } else if(id>=historyBase){
            auto i=static_cast<size_t>(id-historyBase);
            if(i<history.size()){
                text(item.hDC,history[i].label,{box.left+px(12),box.top+px(3),box.right-px(12),box.top+px(25)},normal,color);
                text(item.hDC,(history[i].interrupted?L"部分完成 · ":L"已完成 · ")+sizeText(history[i].bytes),{box.left+px(12),box.top+px(25),box.right-px(12),box.bottom},normal,muted);
            }
        } else if(id==layoutId) symbol(item.hDC,sideNavigation ? 13 : 12,(box.left+box.right-px(22))/2,(box.top+box.bottom-px(22))/2,px(22),navigationInk);
        else {
            wchar_t value[128]{};GetWindowTextW(item.hwndItem,value,128);
            if(nav){ const wchar_t* names[]{L"空间清理",L"清理记录",L"维护工具"};wcscpy_s(value,names[id-navOverview]); }
            if (nav) {
                const int icons[]{2,3,4};
                symbol(item.hDC,icons[id-navOverview],box.left+px(10),(box.top+box.bottom-px(20))/2,px(20),color);
                RECT label=box;label.left+=px(38);label.right-=px(6);
                text(item.hDC,value,label,selected?selectedMenuFont:normal,color);
            } else text(item.hDC,value,box,normal,color,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(id==filesTab||id==ruleTab){ if(selected)fill(item.hDC,{box.left,box.bottom-px(2),box.right,box.bottom},accent); }
        }
        if(focus && !nav && id!=layoutId){InflateRect(&box,-px(3),-px(3));DrawFocusRect(item.hDC,&box);}
    }
    const wchar_t* primaryText() const {
        if (servicing()) return L"系统处理中…";
        if (working()) return stopping ? L"正在停止…" : L"停止";
        if (!ready) return L"开始扫描";
        uint32_t available=0;
        for(int i=0;i<groupCount;++i) if(usable(i)) available|=1U<<i;
        return available && selection==available ? L"选择全部" : L"清理选中";
    }
    void layout() {
        if(tip)ShowWindow(tip,SW_HIDE);tipTarget=0;
        int inset=contentLeft(),left=inset+px(20),right=width-px(20);
        auto put=[&](int id,int x,int y,int w,int h,bool show){
            HWND b=GetDlgItem(window,id);
            RECT current{};GetWindowRect(b,&current);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&current),2);
            bool visible=(GetWindowLongPtrW(b,GWL_STYLE)&WS_VISIBLE)!=0;
            UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW;
            if(current.left==x&&current.top==y)flags|=SWP_NOMOVE;
            if(current.right-current.left==w&&current.bottom-current.top==h)flags|=SWP_NOSIZE;
            if(visible!=show)flags|=show?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
            if((flags&(SWP_NOMOVE|SWP_NOSIZE))!=(SWP_NOMOVE|SWP_NOSIZE)||visible!=show)SetWindowPos(b,nullptr,x,y,w,h,flags);
        };
        put(layoutId,px(12),px(15),px(32),px(32),true);
        for(int i=0;i<3;++i)put(navOverview+i,sideNavigation?px(10):px(60+i*134),sideNavigation?px(84+i*46):px(15),sideNavigation?px(144):px(128),px(36),true);
        // The old application-location control is not created or exposed.
        put(primaryId,right-px(118),px(15),px(118),px(34),page==0||working());
        put(resetId,right-px(204),px(15),px(78),px(34),page==0&&ready&&!working());
        setName(primaryId,primaryText());
        EnableWindow(GetDlgItem(window,primaryId),!servicing()&&(!ready||selection||working()));
        filterRows();visibleGroups=std::max(1,(height-px(308))/px(46));
        int count=static_cast<int>(filtered.size());firstGroup=std::max(0,std::min(firstGroup,std::max(0,((count-1)/visibleGroups)*visibleGroups)));
        put(searchId,left,px(186),px(264),px(30),page==0&&ready&&!working());
        put(filterId,left+px(276),px(184),px(130),px(34),page==0&&ready&&!working());
        put(selectId,right-px(122),px(184),px(122),px(34),page==0&&ready&&!working());
        const wchar_t* filters[]{L"全部项目 ▾",L"可清理 ▾",L"保留 / 未就绪 ▾",L"Windows 系统 ▾"};setName(filterId,filters[filter]);
        bool list=page==0&&ready&&!working();
        for(int i=0;i<groupCount;++i){
            auto found=std::find(filtered.begin(),filtered.end(),i);
            int n=static_cast<int>(found-filtered.begin());
            bool shown=list&&found!=filtered.end()&&n>=firstGroup&&n<firstGroup+visibleGroups;
            put(groupBase+i,left+px(8),px(234)+std::max(0,n-firstGroup)*px(46),px(286),px(44),shown);
            if(shown)setName(groupBase+i,std::wstring(specs[i].name)+L" · "+status(i)+((selection&(1U<<i))?L" · 已选":L" · 未选"));
        }
        int historyVisible=std::max(1,(height-px(170))/px(52));
        for(size_t i=0;i<historyButtons.size();++i)put(historyBase+static_cast<int>(i),left+px(8),px(88)+(static_cast<int>(i)-historyFirst)*px(52),px(286),px(50),page==1&&static_cast<int>(i)>=historyFirst&&static_cast<int>(i)<historyFirst+historyVisible);
        put(previousId,left, height-px(64),px(96),px(28),list||(page==1&&!history.empty()));
        put(nextId,left+px(108),height-px(64),px(96),px(28),list||(page==1&&!history.empty()));
        EnableWindow(GetDlgItem(window,previousId),page==1?historyFirst>0:firstGroup>0);
        EnableWindow(GetDlgItem(window,nextId),page==1?historyFirst+historyVisible<static_cast<int>(history.size()):firstGroup+visibleGroups<count);
        bool showDetail=(list&&!filtered.empty())||(page==1&&!history.empty());int dx=left+px(317),dy=page==1?px(90):px(236);
        put(filesTab,dx,dy,px(84),px(30),list&&!filtered.empty());put(ruleTab,dx+px(90),dy,px(84),px(30),list&&!filtered.empty());
        put(detailId,dx,dy+px(44),right-dx-px(14),height-px(126)-dy-px(44),showDetail);
        put(toggleId,dx,height-px(115),px(112),px(32),list&&!filtered.empty());
        put(expandId,dx+px(118),height-px(115),px(96),px(32),list&&!filtered.empty());
        put(copyId,right-px(78),height-px(115),px(66),px(32),showDetail);
        put(noteId,dx,height-px(115),px(112),px(32),page==1&&!history.empty());
        setName(toggleId,(selection&(1U<<focusedGroup))?L"取消选择":L"加入清理");EnableWindow(GetDlgItem(window,toggleId),usable(focusedGroup));
        setName(expandId,expanded?L"收起路径":L"展开路径");
        for(int i=0;i<6;++i){int cell=(right-left-px(48))/2;put(toolBase+i,left+px(16)+(i%2)*(cell+px(16)),px(182+i/2*80),cell,px(64),page==2);EnableWindow(toolButtons[i],!working()&&!demo);}
        updateDetail();RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    bool captureUi(const std::wstring& path) {
        HDC screenDc = GetDC(window), dc = CreateCompatibleDC(screenDc);
        HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
        auto old = SelectObject(dc, bitmap);
        paint(dc);
        for (HWND control : buttons) {
            if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT r{}; GetWindowRect(control, &r); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
            int saved = SaveDC(dc); SetViewportOrgEx(dc, r.left, r.top, nullptr);
            DRAWITEMSTRUCT item{}; item.CtlID = GetDlgCtrlID(control); item.hwndItem = control; item.hDC = dc;
            item.itemState = IsWindowEnabled(control) ? 0 : ODS_DISABLED;
            item.rcItem = {0, 0, r.right - r.left, r.bottom - r.top};
            drawButton(item); RestoreDC(dc, saved);
        }
        for(HWND control:{search,detail}) {
            if(!(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE))continue;
            RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
            int saved=SaveDC(dc);SetViewportOrgEx(dc,r.left,r.top,nullptr);
            SendMessageW(control,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
            RestoreDC(dc,saved);
        }
        SelectObject(dc, old);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        bool ok = GetDIBits(dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) != 0;
        FILE* image = nullptr;
        if (ok && !_wfopen_s(&image, path.c_str(), L"wb")) {
            BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
            ok = fwrite(&header, sizeof(header), 1, image) == 1 &&
                fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, image) == 1 &&
                fwrite(pixels.data(), 1, pixels.size(), image) == pixels.size();
            fclose(image);
        } else ok = false;
        DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window, screenDc);
        return ok;
    }
    // UI-only checks: never create a worker or invoke a scan/cleanup operation.
    bool checkNavigation(FILE* report) {
        bool pass=true;
        auto check=[&](const char* name,bool ok){fprintf(report,"%s: %s\n",name,ok?"PASS":"FAIL");pass=pass&&ok;};
        check("App-location control removed",GetDlgItem(window,folderId)==nullptr);
        check("Admin scan control removed",GetDlgItem(window,124)==nullptr);
        check("Sidebar is default",sideNavigation&&contentLeft()==px(164));
        auto primaryIs=[&](const wchar_t* expected){wchar_t value[128]{};GetWindowTextW(GetDlgItem(window,primaryId),value,128);return !wcscmp(value,expected);};
        check("Initial scan label",primaryIs(L"开始扫描"));
        check("Ready empty render",captureUi(reportPath+L".home.bmp"));
        ready=true;
        for(int i=0;i<groupCount;++i){result.groups[i].kind=specs[i].kind;result.groups[i].state=static_cast<LONG>(State::Ready);result.groups[i].candidates=1;result.groups[i].eligible=(i+1)*1024*1024;result.groups[i].bytes=result.groups[i].eligible;}
        result.groups[0].previewSize=1;wcscpy_s(result.groups[0].previews[0].path,L"C:\\Example\\cache\\example.tmp");
        result.groups[0].previews[0].bytes=1024;
        layout();click(groupBase+31);click(toggleId);
        check("Highest selection bit and native action",selection==(1U<<31));
        check("Partial selection label",primaryIs(L"清理选中"));
        selection=UINT32_MAX;layout();check("All available selected label",primaryIs(L"选择全部"));
        result.groups[3].state=static_cast<LONG>(State::Unavailable);selection&=~(1U<<3);layout();
        check("All excludes unavailable items",primaryIs(L"选择全部"));
        result.groups[3].state=static_cast<LONG>(State::Ready);
        selection=0;layout();check("Empty selection disables cleanup",!IsWindowEnabled(GetDlgItem(window,primaryId)));
        selection=1U<<31;layout();
        SendMessageW(groupButtons[0], WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(12),px(12)));
        SendMessageW(groupButtons[0], WM_LBUTTONUP, 0, MAKELPARAM(px(12),px(12)));
        check("Checkbox release toggles row",selection==((1U<<31)|1U));
        SendMessageW(groupButtons[0], WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(12),px(12)));
        SendMessageW(groupButtons[0], WM_LBUTTONUP, 0, MAKELPARAM(-10,-10));
        check("Release outside keeps selection",selection==((1U<<31)|1U));
        click(toggleId);
        click(groupBase);click(ruleTab);check("Rule tab",detailTab==1&&detailValue.find(L"保护规则")!=std::wstring::npos);
        click(filesTab);click(expandId);check("Expandable file paths",expanded&&detailValue.find(L"C:\\Example")!=std::wstring::npos);
        SetWindowTextW(search,L"Windows");check("Search",filtered.size()==1&&filtered[0]==31&&selection==(1U<<31));
        SetWindowTextW(search,L"no-matching-rule");check("Empty search",filtered.empty());
        SetWindowTextW(search,L"");filter=3;layout();check("System filter",filtered.size()==1&&filtered[0]==31);filter=0;
        bool geometry=true;
        for(int scale:{96,144}){
            dpi=scale;fonts();
            for(int logicalWidth:{924,1044}){
                width=px(logicalWidth);height=px(680);
                for(bool side:{true,false}){sideNavigation=side;
                for(int start=0;start<groupCount;start+=visibleGroups){
                    firstGroup=start;layout();
                    for(HWND control:buttons){if(!(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE))continue;RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);geometry=geometry&&r.left>=0&&r.right<=width&&r.top>=0&&r.bottom<=height;}
                }
                }
            }
        }
        check("Both layouts at 96/144 DPI minimum/default geometry and pagination",geometry);
        dpi=96;fonts();width=1044;height=720;firstGroup=0;focusedGroup=0;sideNavigation=true;layout();
        check("Master detail render",captureUi(reportPath+L".results.bmp"));
        page=2;layout();check("Tools render",captureUi(reportPath+L".tools.bmp"));
        motion=false;toggleNavigation();check("Top toolbar switch",!sideNavigation&&contentLeft()==0);
        page=0;layout();check("Top toolbar render",captureUi(reportPath+L".top.bmp"));
        toggleNavigation();check("Sidebar restored",sideNavigation&&contentLeft()==px(164));
        ready=false;finished=true;layout();check("Completed scan label restored",primaryIs(L"开始扫描"));
        check("Completed render",captureUi(reportPath+L".completed.bmp"));finished=false;
        page=0;ready=false;selection=0;layout();
        check("No worker or cleanup started",!task&&session.empty());return pass;
    }
    void showError(const std::wstring& value) {
        message = value;
        notify(value, true);
        if (!hidden) MessageBoxW(window, value.c_str(), L"未完成", MB_OK | MB_ICONWARNING);
        InvalidateRect(window, nullptr, FALSE);
    }
    void begin(Operation op) {
        if (servicing()) return;
        if (working()) { task->stop(); stopping = true; layout(); return; }
        if (op == Operation::Clean && (!ready || !selection)) return;
        if (op == Operation::Clean && !hidden) {
            uint64_t bytes = 0, cache = 0;
            for (int i = 0; i != groupCount; ++i) if (selection & (1U << i)) {
                bytes += result.groups[i].eligible;
                if (result.groups[i].kind == Kind::Package) cache += result.groups[i].bytes;
            }
            auto prompt = L"文件：" + sizeText(bytes);
            if (cache) prompt += L"\n开发缓存：" + sizeText(cache) + L"（由工具处理）";
            if (selection & (1U << 31)) prompt += L"\n包含 Windows 更新、传递优化和临时安装文件。旧更新可能无法卸载，可能需要重启。";
            prompt += L"\n直接删除，不进入回收站。继续？";
            if (MessageBoxW(window, prompt.c_str(), L"确认清理", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
        }
        if (op == Operation::ComponentClean && !hidden &&
            MessageBoxW(window, L"运行 Windows 组件清理？", L"确认清理", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
        try {
            if (session.empty()) session = join(join(projectRoot(), L"scratch\\sessions"), uniqueName());
            task.reset(new Task);
            operation = op; stopping = false; finished = false; message.clear();
            task->start(op, session, demo, selection);
            taskStarted = GetTickCount64();
            ready = false; layout();
        } catch (const std::exception& e) { ready = false; showError(std::wstring(e.what(), e.what() + strlen(e.what()))); }
    }
    static uint64_t number(const std::string& data, const std::string& key) {
        auto at = data.find("\"" + key + "\"");
        if (at == std::string::npos) return 0;
        at = data.find(':', at);
        if (at == std::string::npos) return 0;
        return _strtoui64(data.c_str() + at + 1, nullptr, 10);
    }
    void loadHistory() {
        for (HWND b : historyButtons) {
            buttons.erase(std::remove(buttons.begin(), buttons.end(), b), buttons.end());
            DestroyWindow(b);
        }
        historyButtons.clear(); history.clear(); historyIndex=0; historyFirst=0;
        auto logs = demo ? session : join(projectRoot(), L"logs");
        if (logs.empty() || !plainPath(logs)) return;
        WIN32_FIND_DATAW info{};
        HANDLE find = FindFirstFileW(join(logs, L"*").c_str(), &info);
        if (find == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> names;
        do {
            if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::wstring name = info.cFileName;
            if (name.find(L".summary") == name.size() - std::min<size_t>(8, name.size()) ||
                name.find(L"-cleanup.json") != std::wstring::npos) {
                names.push_back(name);
                if (names.size() > 40) { std::sort(names.begin(), names.end(), std::greater<std::wstring>()); names.resize(40); }
            }
        } while (FindNextFileW(find, &info));
        FindClose(find);
        std::sort(names.begin(), names.end(), std::greater<std::wstring>());
        for (const auto& name : names) {
            History row; row.path = join(logs, name); row.label = name.substr(0, 15);
            if (name.find(L".summary") != std::wstring::npos) {
                Shared summary{};
                if (!readSnapshot(row.path, summary)) continue;
                row.interrupted = !summary.done || summary.canceled || summary.error;
                for (int i = 0; i < groupCount; ++i) {
                    const auto& g = summary.groups[i]; row.bytes += g.removed;
                    if (g.failed || g.state == static_cast<LONG>(State::Canceled)) row.interrupted = true;
                    if (g.removed || g.failed || g.skipped || g.state == static_cast<LONG>(State::Complete)) {
                        row.details += L"\r\n\r\n" + std::wstring(specs[i].name) + L" · 已记录 " + sizeText(g.removed) +
                            L"\r\n保留 " + std::to_wstring(g.skipped) + L" 项 · 失败 " + std::to_wstring(g.failed) + L" 项\r\n" + g.note;
                    }
                }
            } else {
                // The legacy outcome is at the start of each log; never load its full file list.
                FILE* file = nullptr;
                if (_wfopen_s(&file, row.path.c_str(), L"rb")) continue;
                char prefix[4096]{};
                size_t n = fread(prefix, 1, sizeof(prefix), file); fclose(file);
                std::string head(prefix, n); row.bytes = number(head, "RemovedBytes");
                auto p = head.find("\"Canceled\"");
                if (p != std::string::npos) row.interrupted = head.substr(p, 24).find("true") != std::string::npos;
            }
            auto notePath=row.path+L".note";
            if(plainPath(notePath)){FILE* note=nullptr;if(!_wfopen_s(&note,notePath.c_str(),L"rb")){wchar_t value[1001]{};size_t length=fread(value,sizeof(wchar_t),1000,note);fclose(note);row.note.assign(value,length);}}
            history.push_back(row);
            historyButtons.push_back(button(historyBase + static_cast<int>(history.size()) - 1, (row.label + L" " + sizeText(row.bytes)).c_str()));
        }
    }
    void tick() {
        hover();
        if (toastUntil && GetTickCount64() >= toastUntil) { toastUntil=0; toast.clear(); InvalidateRect(window,nullptr,FALSE); }
        bool wasWorking = working();
        if (working()) {
            setName(primaryId, primaryText());
            EnableWindow(GetDlgItem(window, primaryId), !servicing());
            if (operation == Operation::Scan && GetTickCount64() - taskStarted > 150000 && !stopping) { task->stop(); stopping = true; }
        }
        if (task && taskStarted && task->poll()) {
            auto value = task->shared();
            result = *value;
            bool forceStopped = !value->done || task->exitCode() != 0;
            if (forceStopped) {
                result.canceled = stopping; result.error = !stopping;
                message = stopping ? L"已停止。部分操作可能已完成，请重新扫描。" : L"后台异常退出，请重新扫描。";
            }
            ready = operation == Operation::Scan && !result.canceled && !result.error && !forceStopped;
            finished = operation != Operation::Scan;
            selection = 0;
            for (int i = 0; i != groupCount; ++i) {
                if (ready && usable(i) && ((i < 10 && result.groups[i].kind != Kind::Shader) || result.groups[i].kind == Kind::System)) selection |= 1U << i;
                std::wstring label = std::wstring(specs[i].name) + L" · " + sizeText(result.groups[i].bytes);
                label += usable(i) ? L" · 可选择" : L" · 已保留";
                SetWindowTextW(groupButtons[i], label.c_str());
            }
            taskStarted = 0;
            uint64_t failures = 0; for (auto& group : result.groups) failures += group.failed;
            notify(result.error ? L"操作未完成，请查看错误信息" : result.canceled ? L"操作已停止" : failures ? L"部分项目未完成，请查看详情" : ready ? L"扫描完成" : L"操作已结束", result.error || failures);
            refreshDisks(); layout();
            if (result.error) showError(*result.message ? result.message : message);
        }
        if (!IsIconic(window) && GetTickCount64() - lastDisk >= 5000) { refreshDisks(); InvalidateRect(window, nullptr, FALSE); }
        if (wasWorking && motion && !IsIconic(window) && (hidden || GetForegroundWindow() == window)) {
            ++frame; InvalidateRect(window, nullptr, FALSE);
        }
    }
    void copyDetails() {
        if (!OpenClipboard(window)) { showError(L"剪贴板暂时不可用，请重试。"); return; }
        auto bytes=(detailValue.size()+1)*sizeof(wchar_t); HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,bytes);
        bool copied=false;
        if(data){auto target=GlobalLock(data);if(target){memcpy(target,detailValue.c_str(),bytes);GlobalUnlock(data);if(EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,data))copied=true;}}
        if(data&&!copied)GlobalFree(data);CloseClipboard();
        if(copied)notify(L"详情已复制");else showError(L"复制失败，请重试。");
    }
    void choices(bool selectionMenu) {
        HMENU menu=CreatePopupMenu();
        const wchar_t* ranges[]{L"选择建议项目",L"选择全部可清理项目",L"清空选择"};
        const wchar_t* filters[]{L"全部项目",L"可清理",L"保留 / 未就绪",L"Windows 系统"};
        for(int i=0;i<(selectionMenu?3:4);++i)AppendMenuW(menu,MF_STRING|(!selectionMenu&&filter==i?MF_CHECKED:0),i+1,selectionMenu?ranges[i]:filters[i]);
        RECT box{};GetWindowRect(GetDlgItem(window,selectionMenu?selectId:filterId),&box);
        int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTALIGN,box.right,box.bottom,0,window,nullptr);DestroyMenu(menu);
        if(!choice)return;
        if(selectionMenu){
            selection=0;
            if(choice!=3)for(int i=0;i<groupCount;++i)if(usable(i)&&(choice==2||(i<10&&specs[i].kind!=Kind::Shader)||specs[i].kind==Kind::System))selection|=1U<<i;
            notify(L"清理范围已更新");
        }else{filter=choice-1;firstGroup=0;}
        layout();
    }
    void click(int id) {
        if(id==layoutId){toggleNavigation();return;}
        if(id>=navOverview&&id<=navTools){page=id-navOverview;if(page==1&&!working())loadHistory();layout();return;}
        if(id==filterId){choices(false);return;}if(id==selectId){choices(true);return;}
        if(id==filesTab||id==ruleTab){detailTab=id-ruleTab+1;layout();return;}
        if(id==copyId){copyDetails();return;}
        if(id==expandId){expanded=!expanded;layout();return;}
        if(id==toggleId&&ready){if(usable(focusedGroup))selection^=1U<<focusedGroup;layout();return;}
        if(id==noteId&&!history.empty()){
            auto& row=history[historyIndex];NoteEditor editor{row.note};
            if(!hidden&&editor.show(window)){
                auto path=row.path+L".note";
                if(!plainPath(path)){showError(L"备注路径不可写");return;}
                FILE* file=nullptr;
                if(_wfopen_s(&file,path.c_str(),L"wb")){showError(L"无法保存备注");return;}
                bool ok=fwrite(editor.value.data(),sizeof(wchar_t),editor.value.size(),file)==editor.value.size();
                ok=fclose(file)==0&&ok;
                if(ok){row.note=editor.value;notify(L"备注已保存");layout();}else showError(L"备注未能完整写入");
            }return;
        }
        if(id==previousId||id==nextId){
            int direction=id==previousId?-1:1;
            if(page==1){int count=std::max(1,(height-px(170))/px(52));historyFirst=std::max(0,historyFirst+direction*count);}
            else if(ready){firstGroup=std::max(0,firstGroup+direction*visibleGroups);if(firstGroup<static_cast<int>(filtered.size()))focusedGroup=filtered[firstGroup];}
            layout();return;
        }
        if(id==primaryId){begin(ready?Operation::Clean:Operation::Scan);return;}
        if(id==resetId){begin(Operation::Scan);return;}
        if(id>=groupBase&&id<groupBase+groupCount&&ready){focusedGroup=id-groupBase;layout();return;}
        if(id>=historyBase&&id<historyBase+static_cast<int>(history.size())){historyIndex=id-historyBase;layout();return;}
        if(id>=toolBase&&id<toolBase+6&&!working()&&!demo){
            if(id==toolBase+4||id==toolBase+5){
                if(!elevated()){showError(L"需要管理员权限，请关闭后重新打开应用并允许权限请求。");return;}
                page=0;begin(id==toolBase+4?Operation::Analyze:Operation::ComponentClean);return;
            }
            const wchar_t* programs[]{L"ms-settings:storagesense",L"cleanmgr.exe",L"ms-settings:appsfeatures",L"explorer.exe"};
            if(!hidden)shellOpen(window,programs[id-toolBase],id==toolBase+3?L"shell:RecycleBinFolder":nullptr);
        }
    }
    void motionPreference() {
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0); motion = enabled != FALSE;
        HIGHCONTRASTW contrast{sizeof(contrast)};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)) motion = false;
    }
    void create() {
        fonts(); motionPreference();
        tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", nullptr, WS_POPUP | WS_BORDER | SS_LEFT | SS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tip) throw std::runtime_error("Cannot create tooltip");
        button(layoutId, L"菜单切换到顶部 · Ctrl+B");
        button(navOverview, L"概览 · Ctrl+1"); button(navHistory, L"记录 · Ctrl+2"); button(navTools, L"工具 · Ctrl+3");
        button(primaryId, L"开始扫描"); button(resetId, L"重新扫描");
        button(filterId,L"全部项目 ▾");button(selectId,L"选择范围 ▾");
        button(filesTab,L"文件预览");button(ruleTab,L"清理规则");button(copyId,L"复制");
        button(toggleId,L"加入清理");button(expandId,L"展开路径");button(noteId,L"编辑备注");
        search=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(searchId)),nullptr,nullptr);
        detail=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(detailId)),nullptr,nullptr);
        for(HWND control:{search,detail})SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(normal),FALSE);
        SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索清理项目"));
        SendMessageW(search,EM_LIMITTEXT,120,0);
        button(previousId, L"上一页规则"); button(nextId, L"下一页规则");
        for (int i = 0; i != groupCount; ++i) groupButtons[i] = button(groupBase + i, specs[i].name);
        const wchar_t* toolNames[]{L"存储设置", L"磁盘清理", L"卸载应用", L"回收站", L"组件分析", L"组件清理"};
        for (int i = 0; i != 6; ++i) toolButtons[i] = button(toolBase + i, toolNames[i]);
        refreshDisks(); SetTimer(window, timerId, 80, nullptr); layout();
    }
    LRESULT handle(UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_CREATE: create(); return 0;
        case WM_MOUSEWHEEL:
            if ((page == 0 && ready && !working()) || page == 1) click(GET_WHEEL_DELTA_WPARAM(w) > 0 ? previousId : nextId);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: width = LOWORD(l); height = HIWORD(l); if (w != SIZE_MINIMIZED) layout(); return 0;
        case WM_GETMINMAXINFO: { auto p = reinterpret_cast<MINMAXINFO*>(l); p->ptMinTrackSize = {px(940), px(680)}; return 0; }
        case WM_DPICHANGED: {
            dpi = HIWORD(w); fonts(); discardBuffer();
            auto r = reinterpret_cast<RECT*>(l);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(); return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
            if (!memory || bufferWidth != width || bufferHeight != height) {
                discardBuffer(); memory = CreateCompatibleDC(dc); buffer = CreateCompatibleBitmap(dc, std::max(1, width), std::max(1, height));
                oldBitmap = SelectObject(memory, buffer); bufferWidth = width; bufferHeight = height;
            }
            if (memory && buffer) { paint(memory); BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY); }
            EndPaint(window, &ps); return 0;
        }
        case WM_DRAWITEM: drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
        case WM_COMMAND:
            if(LOWORD(w)==searchId&&HIWORD(w)==EN_CHANGE){wchar_t value[121]{};GetWindowTextW(search,value,121);query=value;firstGroup=0;layout();return 0;}
            click(LOWORD(w)); return 0;
        case WM_TIMER:
            if (w == timerId) tick();
            return 0;
        case WM_SETTINGCHANGE: motionPreference(); return 0;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(l) == detail) { SetTextColor(reinterpret_cast<HDC>(w),ink);SetBkColor(reinterpret_cast<HDC>(w),surface);return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH)); }
            SetTextColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_INFOTEXT));
            SetBkColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_INFOBK));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_INFOBK));
        case WM_CLOSE:
            if (servicing()) { notify(L"Windows 正在处理系统文件，请等待完成后关闭"); return 0; }
            if (task) { task->stop(); task->terminate(); }
            DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, timerId); task.reset(); discardBuffer();
            if (normal) { DeleteObject(normal); normal = nullptr; }
            if (large) { DeleteObject(large); large = nullptr; }
            if (menuFont) { DeleteObject(menuFont); menuFont = nullptr; }
            if (selectedMenuFont) { DeleteObject(selectedMenuFont); selectedMenuFont = nullptr; }
            if (!session.empty()) removeSession(session, demo);
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, msg, w, l);
    }
public:
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        Ui* self = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, w, l);
        try { return self->handle(message, w, l); }
        catch (const std::exception& e) {
            self->message.assign(e.what(), e.what() + strlen(e.what()));
            if (self->task) self->task->stop();
            return DefWindowProcW(hwnd, message, w, l);
        }
    }
    int run(bool isDemo, bool hiddenTest, const std::wstring& report, bool navigationTest) {
        demo = isDemo; hidden = hiddenTest; reportPath = report;
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = demo ? L"CleanTool.Native.Demo" : elevated() ? L"CleanTool.Native.Admin" : L"CleanTool.Native";
        cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(101));
        cls.hIconSm = static_cast<HICON>(LoadImageW(cls.hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        RegisterClassExW(&cls);
        HDC screen = GetDC(nullptr); dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen);
        window = CreateWindowExW(WS_EX_COMPOSITED, cls.lpszClassName, demo ? L"CleanTool · 演示" : L"CleanTool", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, px(1060), px(760), nullptr, nullptr, cls.hInstance, this);
        if (!window) throw std::runtime_error("Cannot create main window");
        if (!hidden) { ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window); if (wcsstr(GetCommandLineW(), L"--scan")) PostMessageW(window, WM_COMMAND, resetId, 0); }
        if (hidden) {
            // No ShowWindow, focus acquisition, message boxes, or desktop capture.
            FILE* file = nullptr; _wfopen_s(&file, reportPath.c_str(), L"wb");
            if (!file) { DestroyWindow(window); return 1; }
            if (navigationTest) {
                bool ok = checkNavigation(file);
                fclose(file); DestroyWindow(window); return ok ? 0 : 1;
            }
            if (!demo) {
                SendMessageW(window, WM_PAINT, 0, 0);
                begin(Operation::Scan);
                SIZE_T peakMain = 0, peakWorker = 0, peakSum = 0, privateMain = 0, privateWorker = 0;
                ULONGLONG started = GetTickCount64(), deadline = started + 35000;
                while (taskStarted && GetTickCount64() < deadline) {
                    PROCESS_MEMORY_COUNTERS_EX mainMem{sizeof(mainMem)}, workerMem{sizeof(workerMem)};
                    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mainMem), sizeof(mainMem));
                    GetProcessMemoryInfo(task->process(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&workerMem), sizeof(workerMem));
                    peakMain = std::max(peakMain, mainMem.WorkingSetSize); peakWorker = std::max(peakWorker, workerMem.WorkingSetSize);
                    peakSum = std::max(peakSum, mainMem.WorkingSetSize + workerMem.WorkingSetSize);
                    privateMain = std::max(privateMain, mainMem.PrivateUsage); privateWorker = std::max(privateWorker, workerMem.PrivateUsage);
                    tick(); Sleep(10);
                }
                bool ok = ready && !working();
                // Render the real drawing functions and child controls without showing a desktop window.
                HDC screenDc = GetDC(window), renderDc = CreateCompatibleDC(screenDc);
                HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
                auto previousBitmap = SelectObject(renderDc, bitmap);
                for (int offset : {0, 26}) {
                    firstGroup = offset; layout(); focusedGroup = firstGroup; paint(renderDc);
                    for (HWND control : buttons) {
                        if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE)) continue;
                        RECT r{}; GetWindowRect(control, &r); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
                        int saved = SaveDC(renderDc); SetViewportOrgEx(renderDc, r.left, r.top, nullptr);
                        DRAWITEMSTRUCT item{}; item.CtlID = GetDlgCtrlID(control); item.hwndItem = control; item.hDC = renderDc;
                        item.itemState = IsWindowEnabled(control) ? 0 : ODS_DISABLED; item.rcItem = {0, 0, r.right - r.left, r.bottom - r.top};
                        drawButton(item); RestoreDC(renderDc, saved);
                    }
                    SelectObject(renderDc, previousBitmap);
                    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
                    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
                    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
                    if (GetDIBits(renderDc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS)) {
                        auto imagePath = reportPath + (offset ? L".last-page.bmp" : L".first-page.bmp");
                        FILE* image = nullptr;
                        if (!_wfopen_s(&image, imagePath.c_str(), L"wb")) {
                            BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
                            header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
                            fwrite(&header, sizeof(header), 1, image); fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, image);
                            fwrite(pixels.data(), 1, pixels.size(), image); fclose(image);
                        }
                    }
                    SelectObject(renderDc, bitmap);
                }
                SelectObject(renderDc, previousBitmap); DeleteObject(bitmap); DeleteDC(renderDc); ReleaseDC(window, screenDc);
                firstGroup = 0; focusedGroup = 0; layout();
                fprintf(file, "Read-only real scan: %s\nElapsed: %llu ms\nMain peak working set: %llu\nWorker peak working set: %llu\nConcurrent sum peak: %llu\nMain peak private: %llu\nWorker peak private: %llu\n",
                    ok ? "PASS" : "FAIL", GetTickCount64() - started,
                    static_cast<unsigned long long>(peakMain), static_cast<unsigned long long>(peakWorker),
                    static_cast<unsigned long long>(peakSum), static_cast<unsigned long long>(privateMain), static_cast<unsigned long long>(privateWorker));
                for (int i = 0; i != groupCount; ++i)
                    fprintf(file, "Group %d: state=%ld files=%llu candidates=%llu bytes=%llu\n", i, result.groups[i].state, result.groups[i].files, result.groups[i].candidates, result.groups[i].bytes);
                bool pagesOk = true;
                for (int scale : {96, 144}) {
                    dpi = scale; fonts(); width = px(924); height = px(680); firstGroup = 0; layout();
                    for (int begin = 0; begin < groupCount; begin += visibleGroups) {
                        firstGroup = begin; layout();
                        for (int i = 0; i < groupCount; ++i) {
                            bool shown = (GetWindowLongPtrW(groupButtons[i], GWL_STYLE) & WS_VISIBLE) != 0;
                            pagesOk = pagesOk && shown == (i >= begin && i < begin + visibleGroups);
                            if (shown) {
                                RECT r{}; GetWindowRect(groupButtons[i], &r);
                                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
                                pagesOk = pagesOk && r.top >= px(234) && r.bottom <= height - px(70);
                            }
                        }
                    }
                }
                firstGroup = 0; layout();
                pagesOk = pagesOk && (selection & ~(((1U << 10) - 1) | (1U << 31))) == 0 && (selection & ((1U << 4) | (1U << 5))) == 0;
                ok = ok && pagesOk;
                fprintf(file, "Rule pagination at 96/144 DPI and opt-in defaults: %s\n", pagesOk ? "PASS" : "FAIL");
                begin(Operation::Scan);
                deadline = GetTickCount64() + 2000;
                while (working() && task->shared()->active < 0 && GetTickCount64() < deadline) Sleep(1);
                bool activeScan = task->running() && task->shared()->active >= 0;
                auto cancelAt = GetTickCount64();
                SendMessageW(window, WM_COMMAND, primaryId, 0);
                while (taskStarted && GetTickCount64() - cancelAt < 1500) { tick(); Sleep(1); }
                auto cancelMs = GetTickCount64() - cancelAt;
                ok = ok && activeScan && !working() && !ready && cancelMs < 1200;
                fprintf(file, "Real scan stop: %s; %llu ms\n",
                    activeScan && !working() && !ready && cancelMs < 1200 ? "PASS" : "FAIL", cancelMs);
                begin(Operation::Scan);
                deadline = GetTickCount64() + 2000;
                while (working() && task->shared()->active < 0 && GetTickCount64() < deadline) Sleep(1);
                activeScan = task->running() && task->shared()->active >= 0;
                HANDLE workerHandle = nullptr;
                DuplicateHandle(GetCurrentProcess(), task->process(), GetCurrentProcess(), &workerHandle, SYNCHRONIZE, FALSE, 0);
                Handle runningWorker(workerHandle);
                auto closeAt = GetTickCount64();
                SendMessageW(window, WM_CLOSE, 0, 0);
                auto closeMs = GetTickCount64() - closeAt;
                bool closed = activeScan && !IsWindow(window) && runningWorker &&
                    WaitForSingleObject(runningWorker, 500) == WAIT_OBJECT_0 && closeMs < 500;
                fprintf(file, "Real scan close and worker exit: %s; %llu ms\n", closed ? "PASS" : "FAIL", closeMs);
                fclose(file); return ok && closed ? 0 : 1;
            }
            bool pass = !IsWindowVisible(window);
            DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            HDC dc = GetDC(window), offscreen = CreateCompatibleDC(dc);
            HBITMAP image = CreateCompatibleBitmap(dc, 1440, 1080); auto old = SelectObject(offscreen, image);
            for (int scale : {96, 144}) {
                dpi = scale; fonts(); width = px(900); height = px(680);
                for (int p = 0; p < 3; ++p) {
                    page = p; layout(); paint(offscreen);
                    for (HWND b : buttons) {
                        wchar_t name[512]{};
                        pass = pass && GetWindowTextW(b, name, 512) > 0;
                        pass = pass && !tipText(GetDlgCtrlID(b)).empty();
                        RECT r{}; GetClientRect(b, &r);
                        DRAWITEMSTRUCT draw{}; draw.CtlID = GetDlgCtrlID(b); draw.hDC = offscreen; draw.rcItem = r;
                        drawButton(draw);
                    }
                }
            }
            SelectObject(offscreen, old); DeleteObject(image); DeleteDC(offscreen); ReleaseDC(window, dc);
            motion = false; page = 0; begin(Operation::Scan);
            ULONGLONG deadline = GetTickCount64() + 10000;
            while (taskStarted && GetTickCount64() < deadline) { tick(); Sleep(10); }
            pass = pass && ready && !working() && selection == 1 && !wcscmp(primaryText(),L"选择全部");
            begin(Operation::Clean);
            deadline = GetTickCount64() + 10000;
            while (taskStarted && GetTickCount64() < deadline) { tick(); Sleep(10); }
            pass = pass && finished && result.groups[0].deleted == 1 && result.groups[0].skipped >= 1 && !wcscmp(primaryText(),L"开始扫描");
            begin(Operation::Scan);
            deadline = GetTickCount64() + 10000;
            while (taskStarted && GetTickCount64() < deadline) { tick(); Sleep(10); }
            pass = pass && ready;
            begin(Operation::Scan); task->stop(); stopping = true;
            deadline = GetTickCount64() + 2000;
            while (taskStarted && GetTickCount64() < deadline) { tick(); Sleep(10); }
            pass = pass && !working() && !ready;
            DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
            fprintf(file, "Hidden UI: %s\nVisible: %d\nDPI render: 96, 144\nGDI delta: %ld\nWorking set: %llu\nPrivate bytes: %llu\n",
                pass ? "PASS" : "FAIL", IsWindowVisible(window), static_cast<long>(gdiAfter) - gdiBefore,
                static_cast<unsigned long long>(mem.WorkingSetSize), static_cast<unsigned long long>(mem.PrivateUsage));
            begin(Operation::Scan);
            auto start = GetTickCount64(); SendMessageW(window, WM_CLOSE, 0, 0);
            auto closeMs = GetTickCount64() - start;
            bool removed = GetFileAttributesW(session.c_str()) == INVALID_FILE_ATTRIBUTES;
            fprintf(file, "Demo close: %llu ms; owned session removed: %s\n", closeMs, removed ? "PASS" : "FAIL");
            fclose(file);
            return pass && closeMs < 500 && removed ? 0 : 1;
        }
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_F5 && page == 0) { begin(ready ? Operation::Clean : Operation::Scan); continue; }
                if (msg.wParam == VK_SPACE && msg.hwnd == groupButtons[focusedGroup] && ready) { click(toggleId); continue; }
                if (msg.wParam == VK_ESCAPE && working()) { if (!servicing()) { task->stop(); stopping = true; } continue; }
                if (GetKeyState(VK_CONTROL) < 0 && msg.wParam >= '1' && msg.wParam <= '3') { click(navOverview + static_cast<int>(msg.wParam - '1')); continue; }
                if (GetKeyState(VK_CONTROL) < 0 && msg.wParam == 'B') { click(layoutId); continue; }
            }
            if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        return static_cast<int>(msg.wParam);
    }
};
} // namespace
int runUi(bool demo, bool hiddenTest, const std::wstring& report, bool navigationTest) {
    Ui ui; return ui.run(demo, hiddenTest, report, navigationTest);
}
} // namespace storage
