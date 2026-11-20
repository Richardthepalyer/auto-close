// =============================================================================
//  Hotkey Closer  -  closes open apps with a global hotkey (Windows 10/11)
//  Pure Win32 / C++17, no third-party libraries.
//
//  Sections (search for the banner comments to jump around):
//    1. Settings & constants you may want to tweak
//    2. Small utilities (strings, files, JSON)
//    3. Config load/save
//    4. Process / window discovery + the "never close these" list
//    5. The closing engine (graceful -> optional force)
//    6. Tray icon, notifications, auto-start
//    7. Hotkey registration + hotkey recorder
//    8. Settings window UI (controls, dark mode, app picker)
//    9. Window procedure + WinMain
// =============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "version.lib")
// Enables the modern (v6) common controls look
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// =============================================================================
// 1. SETTINGS & CONSTANTS
// =============================================================================
static const wchar_t* APP_NAME   = L"Hotkey Closer";
static const wchar_t* WND_CLASS  = L"HotkeyCloserMainWnd";
static const wchar_t* MUTEX_NAME = L"Local\\HotkeyCloser_SingleInstance";
static const wchar_t* RUN_VALUE  = L"HotkeyCloser";            // registry autostart value name

static const int DEFAULT_TIMEOUT_SEC = 5;                      // force-close timeout default
static const int MAX_TIMEOUT_SEC     = 120;

// Processes that are NEVER closed (graceful or forced), in any mode.
// Add names here (lowercase) if you want more built-in protection.
// NOTE: ApplicationFrameHost.exe hosts Store/UWP app windows (Calculator, Settings...),
// so UWP apps are not closable by this tool (killing the host would be unsafe).
static const wchar_t* kCritical[] = {
    L"system", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
    L"services.exe", L"lsass.exe", L"svchost.exe", L"dwm.exe", L"explorer.exe",
    L"taskmgr.exe", L"fontdrvhost.exe", L"sihost.exe", L"ctfmon.exe", L"conhost.exe",
    L"searchhost.exe", L"searchapp.exe", L"searchindexer.exe", L"startmenuexperiencehost.exe",
    L"shellexperiencehost.exe", L"textinputhost.exe", L"applicationframehost.exe",
    L"runtimebroker.exe", L"lockapp.exe", L"audiodg.exe", L"spoolsv.exe", L"wudfhost.exe",
    L"dllhost.exe", L"smartscreen.exe", L"securityhealthservice.exe",
    L"securityhealthsystray.exe", L"msmpeng.exe", L"nissrv.exe", L"mpdefendercoreservice.exe",
    L"msedgewebview2.exe",
    // common third-party antivirus
    L"avp.exe", L"avgui.exe", L"avastui.exe", L"avastsvc.exe", L"bdagent.exe",
    L"mbamservice.exe", L"mbam.exe", L"nortonsecurity.exe", L"mcshield.exe", L"ekrn.exe",
    L"egui.exe", L"sophosui.exe", L"savservice.exe",
};

// Dark / light palette
static COLORREF ColBg();
static COLORREF ColCtrl();
static COLORREF ColText();

// =============================================================================
// 2. UTILITIES
// =============================================================================
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}
// "  C:\Apps\Chrome.EXE " / "chrome" -> "chrome.exe"
static std::wstring NormExe(std::wstring s) {
    s = Trim(s);
    s.erase(std::remove(s.begin(), s.end(), L'"'), s.end());
    size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s = s.substr(p + 1);
    s = Lower(Trim(s));
    if (s.empty()) return s;
    if (s.find(L'.') == std::wstring::npos) s += L".exe";
    return s;
}
static bool Contains(const std::vector<std::wstring>& v, const std::wstring& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
static void Erase(std::vector<std::wstring>& v, const std::wstring& x) {
    v.erase(std::remove(v.begin(), v.end(), x), v.end());
}

static bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    bool ok = sz.QuadPart == 0 || ReadFile(h, &out[0], (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);
    return ok;
}
static bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

// ---- Minimal JSON (just enough for our config file) -------------------------
struct JV {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JV> a;
    std::map<std::string, JV> o;
};
struct JParser {
    const std::string& s;
    size_t i = 0;
    explicit JParser(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(i, n, w) == 0) { i += n; return true; }
        return false;
    }
    bool str(std::string& r) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { r += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case 'n': r += '\n'; break;
                case 't': r += '\t'; break;
                case 'r': r += '\r'; break;
                case 'b': r += '\b'; break;
                case 'f': r += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    std::wstring w(1, (wchar_t)cp);
                    r += ToUtf8(w);
                    break;
                }
                default: r += e;
            }
        }
        return false;
    }
    bool parse(JV& out) {
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') {
            i++; out.t = JV::Obj; ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                i++;
                JV v;
                if (!parse(v)) return false;
                out.o[k] = std::move(v);
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            i++; out.t = JV::Arr; ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            while (true) {
                JV v;
                if (!parse(v)) return false;
                out.a.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') { out.t = JV::Str; return str(out.s); }
        if (lit("true"))  { out.t = JV::Bool; out.b = true;  return true; }
        if (lit("false")) { out.t = JV::Bool; out.b = false; return true; }
        if (lit("null"))  { out.t = JV::Null; return true; }
        char* end = nullptr;
        out.n = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return false;
        out.t = JV::Num;
        i = (size_t)(end - s.c_str());
        return true;
    }
};
static std::string JEsc(const std::string& in) {
    std::string r;
    for (unsigned char c : in) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); r += b; }
        else r += (char)c;
    }
    return r;
}
static std::string JArr(const std::vector<std::wstring>& v) {
    std::string r = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) r += ", ";
        r += "\"" + JEsc(ToUtf8(v[i])) + "\"";
    }
    return r + "]";
}

// =============================================================================
// 3. CONFIG
// =============================================================================
struct Config {
    UINT mods = MOD_CONTROL | MOD_ALT;      // default hotkey: Ctrl+Alt+Q
    UINT vk   = 'Q';
    int  mode = 0;                          // 0 = close all, 1 = close only selected
    std::vector<std::wstring> selected;     // exe names ticked in the picker
    std::vector<std::wstring> manual;       // exe names added by hand (always listed)
    std::vector<std::wstring> protectedApps;// never closed in "Close all" mode
    bool confirm   = true;
    bool force     = true;
    int  timeoutSec = DEFAULT_TIMEOUT_SEC;
    bool autostart = false;
    bool dark      = true;
};
static Config g_cfg;

static std::wstring ConfigPath() {
    wchar_t buf[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf);
    std::wstring dir = std::wstring(buf) + L"\\HotkeyCloser";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}
static void SaveConfig() {
    std::ostringstream o;
    o << "{\n";
    o << "  \"hotkey\": { \"modifiers\": " << g_cfg.mods << ", \"vk\": " << g_cfg.vk << " },\n";
    o << "  \"mode\": \"" << (g_cfg.mode == 0 ? "all" : "selected") << "\",\n";
    o << "  \"selected\": " << JArr(g_cfg.selected) << ",\n";
    o << "  \"manual\": " << JArr(g_cfg.manual) << ",\n";
    o << "  \"protected\": " << JArr(g_cfg.protectedApps) << ",\n";
    o << "  \"confirm\": " << (g_cfg.confirm ? "true" : "false") << ",\n";
    o << "  \"force\": " << (g_cfg.force ? "true" : "false") << ",\n";
    o << "  \"timeout_seconds\": " << g_cfg.timeoutSec << ",\n";
    o << "  \"autostart\": " << (g_cfg.autostart ? "true" : "false") << ",\n";
    o << "  \"dark_mode\": " << (g_cfg.dark ? "true" : "false") << "\n";
    o << "}\n";
    WriteAll(ConfigPath(), o.str());
}
static const JV* JGet(const JV& v, const char* k) {
    auto it = v.o.find(k);
    return it == v.o.end() ? nullptr : &it->second;
}
static std::vector<std::wstring> JList(const JV& v, const char* k) {
    std::vector<std::wstring> r;
    if (auto* a = JGet(v, k))
        if (a->t == JV::Arr)
            for (auto& e : a->a)
                if (e.t == JV::Str) {
                    std::wstring n = NormExe(FromUtf8(e.s));
                    if (!n.empty() && !Contains(r, n)) r.push_back(n);
                }
    return r;
}
static void LoadConfig() {
    std::string txt;
    if (!ReadAll(ConfigPath(), txt)) return;
    JV root;
    JParser p(txt);
    if (!p.parse(root) || root.t != JV::Obj) return;
    if (auto* h = JGet(root, "hotkey")) {
        if (auto* m = JGet(*h, "modifiers")) g_cfg.mods = (UINT)m->n;
        if (auto* k = JGet(*h, "vk")) g_cfg.vk = (UINT)k->n;
    }
    if (auto* m = JGet(root, "mode")) g_cfg.mode = (m->s == "selected") ? 1 : 0;
    g_cfg.selected     = JList(root, "selected");
    g_cfg.manual       = JList(root, "manual");
    g_cfg.protectedApps = JList(root, "protected");
    if (auto* v = JGet(root, "confirm"))  g_cfg.confirm = v->b;
    if (auto* v = JGet(root, "force"))    g_cfg.force = v->b;
    if (auto* v = JGet(root, "timeout_seconds")) g_cfg.timeoutSec = std::max(1, std::min(MAX_TIMEOUT_SEC, (int)v->n));
    if (auto* v = JGet(root, "autostart")) g_cfg.autostart = v->b;
    if (auto* v = JGet(root, "dark_mode")) g_cfg.dark = v->b;
}

// =============================================================================
// 4. PROCESS / WINDOW DISCOVERY
// =============================================================================
static std::wstring g_selfExe;   // lowercase name of our own exe, filled in WinMain

static bool IsAlwaysProtected(const std::wstring& lowerName) {
    if (lowerName == g_selfExe) return true;                 // never close ourselves
    for (auto* c : kCritical) if (lowerName == c) return true;
    return false;
}

struct ProcSnap {
    std::map<std::wstring, std::vector<DWORD>> byName;       // lowercase exe -> pids
    std::map<DWORD, std::wstring> byPid;
};
static ProcSnap Snapshot() {
    ProcSnap r;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(s, &pe)) {
        do {
            std::wstring n = Lower(pe.szExeFile);
            r.byName[n].push_back(pe.th32ProcessID);
            r.byPid[pe.th32ProcessID] = n;
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return r;
}
static std::wstring ExePathOfPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(h);
    return r;
}

// A "user app window": visible, top-level, unowned, titled, not a tool window, not cloaked.
static bool IsAppWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    if (GetWindow(h, GW_OWNER)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    DWORD cloaked = 0;                                       // 14 = DWMWA_CLOAKED (hidden UWP ghosts, other desktops)
    if (SUCCEEDED(DwmGetWindowAttribute(h, 14, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return true;
}
struct AppWin { HWND h; DWORD pid; };
static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    if (!IsAppWindow(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    ((std::vector<AppWin>*)lp)->push_back({ h, pid });
    return TRUE;
}
static std::vector<AppWin> GetAppWindows() {
    std::vector<AppWin> v;
    EnumWindows(EnumProc, (LPARAM)&v);
    return v;
}

// Decide which exe names would be closed right now for a given config.
static std::vector<std::wstring> BuildTargets(const Config& c) {
    ProcSnap snap = Snapshot();
    std::set<std::wstring> prot, out;
    for (auto& p : c.protectedApps) prot.insert(NormExe(p));
    if (c.mode == 0) {                                       // CLOSE ALL
        for (auto& w : GetAppWindows()) {
            auto it = snap.byPid.find(w.pid);
            if (it == snap.byPid.end()) continue;
            const std::wstring& n = it->second;
            if (IsAlwaysProtected(n) || prot.count(n)) continue;
            out.insert(n);
        }
    } else {                                                 // CLOSE ONLY SELECTED
        for (auto& s : c.selected) {
            std::wstring n = NormExe(s);
            if (n.empty() || IsAlwaysProtected(n)) continue;
            if (snap.byName.count(n)) out.insert(n);         // only if currently running
        }
    }
    return std::vector<std::wstring>(out.begin(), out.end());
}

// =============================================================================
// 5. CLOSING ENGINE  (runs on a worker thread so the UI/hotkey stay responsive)
// =============================================================================
enum { WM_TRAY = WM_APP + 1, WM_RECORDED, WM_RECORD_CANCEL, WM_CLOSE_DONE, WM_SHOW_EXISTING };

static std::atomic<bool> g_busy{ false };

static void CloseWorker(std::vector<std::wstring> targets, bool force, int timeoutSec, HWND notify) {
    // --- Phase 1: graceful. Post WM_CLOSE so apps can ask "save changes?" ----
    {
        ProcSnap s = Snapshot();
        std::set<DWORD> pids;
        for (auto& t : targets) {
            auto it = s.byName.find(t);
            if (it != s.byName.end()) pids.insert(it->second.begin(), it->second.end());
        }
        for (auto& w : GetAppWindows())
            if (pids.count(w.pid)) PostMessageW(w.h, WM_CLOSE, 0, 0);
    }
    auto stillRunning = [&]() {
        ProcSnap s = Snapshot();
        std::vector<std::wstring> r;
        for (auto& t : targets) if (s.byName.count(t)) r.push_back(t);
        return r;
    };
    // --- Phase 2: wait up to the timeout for apps to exit on their own -------
    DWORD start = GetTickCount();
    std::vector<std::wstring> rem;
    while (true) {
        rem = stillRunning();
        if (rem.empty()) break;
        if (GetTickCount() - start >= (DWORD)timeoutSec * 1000) break;
        Sleep(150);
    }
    // --- Phase 3: optional force-close of leftovers --------------------------
    if (!rem.empty() && force) {
        ProcSnap s = Snapshot();
        for (auto& t : rem) {
            auto it = s.byName.find(t);
            if (it == s.byName.end()) continue;
            for (DWORD pid : it->second) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                if (h) { TerminateProcess(h, 1); WaitForSingleObject(h, 1500); CloseHandle(h); }
            }
        }
        Sleep(200);
        rem = stillRunning();
    }
    int failed = (int)rem.size();
    int closed = (int)targets.size() - failed;
    PostMessageW(notify, WM_CLOSE_DONE, (WPARAM)closed, (LPARAM)failed);
}

// =============================================================================
// Globals shared by the UI sections
// =============================================================================
static HINSTANCE g_hInst;
static HWND g_hwnd;
static int g_dpi = 96;
static HFONT g_font, g_fontBig, g_fontBold;
static HBRUSH g_brBg, g_brCtrl;
static HICON g_icoBig, g_icoSmall;
static UINT g_wmTaskbar;
static bool g_recording = false, g_populating = false, g_initUI = false, g_confirming = false;
static int g_statusKind = 0;   // 0 normal, 1 ok (green), 2 warning (red)
static HHOOK g_hook = nullptr;

enum {
    IDC_HOTKEY_TEXT = 100, IDC_RECORD, IDC_STATUS, IDC_RADIO_ALL, IDC_RADIO_SEL,
    IDC_CHK_CONFIRM, IDC_CHK_FORCE, IDC_TIMEOUT, IDC_CHK_AUTO, IDC_CHK_DARK,
    IDC_CLOSE_NOW, IDC_HIDE, IDC_APPLIST, IDC_REFRESH, IDC_MANUAL_EDIT, IDC_MANUAL_ADD,
    IDC_MANUAL_REMOVE, IDC_PROT_LIST, IDC_PROT_EDIT, IDC_PROT_ADD, IDC_PROT_REMOVE,
    ID_TRAY_OPEN = 2001, ID_TRAY_CLOSE, ID_TRAY_QUIT, HK_ID = 1
};
static HWND hHotkey, hRecord, hStatus, hRadioAll, hRadioSel, hChkConfirm, hChkForce, hTimeout,
            hChkAuto, hChkDark, hList, hManualEdit, hProtList, hProtEdit, hTimeoutLbl;
static std::vector<HWND> g_push, g_checks, g_edits;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// =============================================================================
// 6. TRAY, NOTIFICATIONS, AUTOSTART
// =============================================================================
static std::wstring KeyName(UINT vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {   // "extended" keys need bit 24 for the right name
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
        case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            lp |= (1 << 24);
    }
    wchar_t buf[64] = {};
    if (GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    wchar_t t[24]; wsprintfW(t, L"VK 0x%02X", vk);
    return t;
}
static std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    return s + KeyName(vk);
}

static void TrayCommon(NOTIFYICONDATAW& n) {
    n.cbSize = sizeof(n); n.hWnd = g_hwnd; n.uID = 1;
}
static void UpdateTrayTip() {
    NOTIFYICONDATAW n{}; TrayCommon(n);
    n.uFlags = NIF_TIP;
    std::wstring tip = std::wstring(APP_NAME) + L"  [" + FormatHotkey(g_cfg.mods, g_cfg.vk) + L"]";
    lstrcpynW(n.szTip, tip.c_str(), ARRAYSIZE(n.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
static void AddTray() {
    NOTIFYICONDATAW n{}; TrayCommon(n);
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_icoSmall;
    lstrcpynW(n.szTip, APP_NAME, ARRAYSIZE(n.szTip));
    Shell_NotifyIconW(NIM_ADD, &n);
    UpdateTrayTip();
}
static void RemoveTray() {
    NOTIFYICONDATAW n{}; TrayCommon(n);
    Shell_NotifyIconW(NIM_DELETE, &n);
}
// Small notification ("Closed 6 apps") - shown as a Windows toast/balloon
static void Notify(const std::wstring& title, const std::wstring& msg) {
    NOTIFYICONDATAW n{}; TrayCommon(n);
    n.uFlags = NIF_INFO;
    lstrcpynW(n.szInfoTitle, title.c_str(), ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, msg.c_str(), ARRAYSIZE(n.szInfo));
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
static void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\" --tray";   // --tray = start hidden
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

// ---- Trigger: build target list, optionally confirm, then run the worker ----
static void TriggerClose() {
    if (g_busy || g_confirming) return;
    std::vector<std::wstring> targets = BuildTargets(g_cfg);
    if (targets.empty()) { Notify(APP_NAME, L"No apps to close"); return; }
    if (g_cfg.confirm) {
        std::wstring msg = L"Close " + std::to_wstring(targets.size()) + (targets.size() == 1 ? L" app?" : L" apps?");
        msg += L"\n";
        for (size_t i = 0; i < targets.size() && i < 10; i++) msg += L"\n  - " + targets[i];
        if (targets.size() > 10) msg += L"\n  ...and " + std::to_wstring(targets.size() - 10) + L" more";
        g_confirming = true;
        int r = MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
        g_confirming = false;
        if (r != IDYES) return;
    }
    g_busy = true;
    std::thread(CloseWorker, targets, g_cfg.force, g_cfg.timeoutSec, g_hwnd).detach();
}

// =============================================================================
// 7. HOTKEY REGISTRATION + RECORDER
// =============================================================================
static void SetStatus(const std::wstring& t, int kind) {
    g_statusKind = kind;
    SetWindowTextW(hStatus, t.c_str());
    InvalidateRect(hStatus, nullptr, TRUE);
}
static void UpdateHotkeyText() {
    SetWindowTextW(hHotkey, (L" " + FormatHotkey(g_cfg.mods, g_cfg.vk)).c_str());
    UpdateTrayTip();
}
static bool RegisterCurrentHotkey() {
    UnregisterHotKey(g_hwnd, HK_ID);
    return RegisterHotKey(g_hwnd, HK_ID, g_cfg.mods | MOD_NOREPEAT, g_cfg.vk) != 0;
}

// Low-level keyboard hook, active ONLY while recording. It swallows the final
// (non-modifier) key so the combo doesn't trigger anything else.
static LRESULT CALLBACK LLKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_recording) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        UINT vk = k->vkCode;
        bool isMod = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
                     vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
                     vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
        if (!isMod) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                UINT mods = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
                if (vk == VK_ESCAPE && mods == 0) PostMessageW(g_hwnd, WM_RECORD_CANCEL, 0, 0);
                else PostMessageW(g_hwnd, WM_RECORDED, vk, mods);
            }
            return 1;   // swallow
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static void StopRecording() {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_recording = false;
    SetWindowTextW(hRecord, L"Record hotkey");
}
static void StartRecording() {
    if (g_recording) return;
    UnregisterHotKey(g_hwnd, HK_ID);   // so pressing the current combo doesn't fire while recording
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyProc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) { RegisterCurrentHotkey(); SetStatus(L"Could not start recording.", 2); return; }
    g_recording = true;
    SetWindowTextW(hRecord, L"Press combo... (Esc cancels)");
    SetStatus(L"Listening... hold Ctrl / Alt / Shift / Win and press a key.", 0);
}
static void CancelRecording() {
    StopRecording();
    RegisterCurrentHotkey();
    SetStatus(L"Recording cancelled. Hotkey unchanged.", 0);
}
static void OnRecorded(UINT vk, UINT mods) {
    if (!g_recording) return;
    StopRecording();
    std::wstring name = FormatHotkey(mods, vk);
    if (mods == 0) {   // validation: need at least one modifier
        RegisterCurrentHotkey();
        SetStatus(L"Invalid: \"" + name + L"\" has no modifier. Use Ctrl, Alt, Shift or Win plus a key.", 2);
        return;
    }
    UnregisterHotKey(g_hwnd, HK_ID);
    if (RegisterHotKey(g_hwnd, HK_ID, mods | MOD_NOREPEAT, vk)) {
        g_cfg.mods = mods; g_cfg.vk = vk;
        SaveConfig();
        UpdateHotkeyText();
        SetStatus(L"Hotkey set to " + name + L".", 1);
    } else {
        DWORD err = GetLastError();
        RegisterCurrentHotkey();   // restore the previous hotkey
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED)
            SetStatus(name + L" is already taken by Windows or another app. Pick a different combo.", 2);
        else
            SetStatus(name + L" can't be used as a global hotkey (error " + std::to_wstring(err) + L").", 2);
    }
}

// =============================================================================
// 8. SETTINGS WINDOW UI
// =============================================================================
static COLORREF ColBg()   { return g_cfg.dark ? RGB(31, 31, 31) : RGB(243, 243, 243); }
static COLORREF ColCtrl() { return g_cfg.dark ? RGB(45, 45, 45) : RGB(255, 255, 255); }
static COLORREF ColText() { return g_cfg.dark ? RGB(235, 235, 235) : RGB(20, 20, 20); }

static HFONT MakeFont(int pt, bool bold) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}
static HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
               int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

// Draws a simple red circle with a white X (used for window + tray icon)
static HICON MakeAppIcon(int sz) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi); bi.bV5Width = sz; bi.bV5Height = -sz; bi.bV5Planes = 1;
    bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    uint32_t* px = (uint32_t*)bits;
    float c = (sz - 1) / 2.0f, lo = sz * 0.28f, hi = sz * 0.72f, th = std::max(1.0f, sz * 0.07f);
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++) {
            float dx = x - c, dy = y - c;
            uint32_t argb = 0;
            if (std::sqrt(dx * dx + dy * dy) <= sz * 0.47f) argb = 0xFFC9403F;
            if (x >= lo && x <= hi && y >= lo && y <= hi &&
                (std::fabs((float)(x - y)) <= th || std::fabs((float)(x + y - (sz - 1))) <= th))
                argb = 0xFFFFFFFF;
            px[y * sz + x] = argb;
        }
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{}; ii.fIcon = TRUE; ii.hbmMask = mask; ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(color);
    return ic;
}

// ---- App picker ------------------------------------------------------------
struct Row { std::wstring exe, display, path; bool running = false; };
static std::vector<Row> g_rows;

static std::wstring FileDesc(const std::wstring& path, const std::wstring& fallback) {
    if (path.empty()) return fallback;
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return fallback;
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return fallback;
    struct LT { WORD lang, cp; }* tr = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &len) || len < sizeof(LT)) return fallback;
    wchar_t q[128];
    wsprintfW(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
    wchar_t* d = nullptr;
    if (VerQueryValueW(buf.data(), q, (LPVOID*)&d, &len) && d && *d) return d;
    return fallback;
}

static void RefreshAppList() {
    std::map<std::wstring, Row> m;
    ProcSnap snap = Snapshot();
    // 1) running apps that own a visible window (critical processes are hidden from the list)
    for (auto& w : GetAppWindows()) {
        auto it = snap.byPid.find(w.pid);
        if (it == snap.byPid.end()) continue;
        const std::wstring& n = it->second;
        if (IsAlwaysProtected(n) || m.count(n)) continue;
        Row r; r.exe = n; r.path = ExePathOfPid(w.pid); r.display = FileDesc(r.path, n); r.running = true;
        m[n] = r;
    }
    // 2) remembered apps (manual + ticked) even when not running
    auto addSaved = [&](const std::wstring& s) {
        std::wstring n = NormExe(s);
        if (n.empty() || m.count(n)) return;
        Row r; r.exe = n; r.display = n;
        auto it = snap.byName.find(n);
        if (it != snap.byName.end() && !it->second.empty()) {
            r.running = true; r.path = ExePathOfPid(it->second[0]); r.display = FileDesc(r.path, n);
        }
        m[n] = r;
    };
    for (auto& s : g_cfg.manual) addSaved(s);
    for (auto& s : g_cfg.selected) addSaved(s);

    g_rows.clear();
    for (auto& kv : m) g_rows.push_back(kv.second);
    std::sort(g_rows.begin(), g_rows.end(), [](const Row& a, const Row& b) { return Lower(a.display) < Lower(b.display); });

    g_populating = true;
    ListView_DeleteAllItems(hList);
    int sm = GetSystemMetrics(SM_CXSMICON);
    HIMAGELIST il = ImageList_Create(sm, sm, ILC_COLOR32 | ILC_MASK, 16, 16);
    int defIdx = ImageList_AddIcon(il, LoadIconW(nullptr, IDI_APPLICATION));
    for (int i = 0; i < (int)g_rows.size(); i++) {
        const Row& r = g_rows[i];
        int img = defIdx;
        if (!r.path.empty()) {
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(r.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
                img = ImageList_AddIcon(il, sfi.hIcon);
                DestroyIcon(sfi.hIcon);
            }
        }
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = i; it.iImage = img; it.lParam = i;
        it.pszText = (LPWSTR)r.display.c_str();
        ListView_InsertItem(hList, &it);
        ListView_SetItemText(hList, i, 1, (LPWSTR)r.exe.c_str());
        ListView_SetItemText(hList, i, 2, (LPWSTR)(r.running ? L"Running" : L"Not running"));
        ListView_SetCheckState(hList, i, Contains(g_cfg.selected, r.exe));
    }
    HIMAGELIST old = ListView_SetImageList(hList, il, LVSIL_SMALL);
    if (old) ImageList_Destroy(old);
    g_populating = false;
}
static void SetSelected(const std::wstring& exe, bool on) {
    if (on && !Contains(g_cfg.selected, exe)) g_cfg.selected.push_back(exe);
    if (!on) Erase(g_cfg.selected, exe);
    SaveConfig();
}
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}
static void AddManual() {
    std::wstring n = NormExe(GetText(hManualEdit));
    if (n.empty()) return;
    if (IsAlwaysProtected(n)) {
        MessageBoxW(g_hwnd, (n + L" is a protected system/security process (or this app) and can't be added.").c_str(), APP_NAME, MB_ICONWARNING);
        return;
    }
    if (!Contains(g_cfg.manual, n)) g_cfg.manual.push_back(n);
    if (!Contains(g_cfg.selected, n)) g_cfg.selected.push_back(n);
    SaveConfig();
    SetWindowTextW(hManualEdit, L"");
    RefreshAppList();
}
static void RemoveEntry() {
    int i = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (i < 0 || i >= (int)g_rows.size()) return;
    std::wstring n = g_rows[i].exe;
    Erase(g_cfg.manual, n);
    Erase(g_cfg.selected, n);
    SaveConfig();
    RefreshAppList();
}

// ---- Protected list --------------------------------------------------------
static void FillProtList() {
    SendMessageW(hProtList, LB_RESETCONTENT, 0, 0);
    for (auto& p : g_cfg.protectedApps) SendMessageW(hProtList, LB_ADDSTRING, 0, (LPARAM)p.c_str());
}
static void ProtAdd() {
    std::wstring n = NormExe(GetText(hProtEdit));
    if (n.empty()) return;
    if (!Contains(g_cfg.protectedApps, n)) { g_cfg.protectedApps.push_back(n); SaveConfig(); FillProtList(); }
    SetWindowTextW(hProtEdit, L"");
}
static void ProtRemove() {
    int i = (int)SendMessageW(hProtList, LB_GETCURSEL, 0, 0);
    if (i < 0 || i >= (int)g_cfg.protectedApps.size()) return;
    g_cfg.protectedApps.erase(g_cfg.protectedApps.begin() + i);
    SaveConfig(); FillProtList();
}

// ---- Theme (dark / light) --------------------------------------------------
static void ApplyTheme() {
    bool d = g_cfg.dark;
    HBRUSH nb = CreateSolidBrush(ColBg()), nc = CreateSolidBrush(ColCtrl());
    HBRUSH ob = g_brBg, oc = g_brCtrl;
    g_brBg = nb; g_brCtrl = nc;
    if (ob) DeleteObject(ob);
    if (oc) DeleteObject(oc);

    BOOL v = d ? TRUE : FALSE;                                // dark title bar (Win10 1809+ / Win11)
    DwmSetWindowAttribute(g_hwnd, 20, &v, sizeof(v));
    DwmSetWindowAttribute(g_hwnd, 19, &v, sizeof(v));

    for (HWND h : g_push)   SetWindowTheme(h, d ? L"DarkMode_Explorer" : nullptr, nullptr);
    // Check/radio boxes ignore text colour when themed, so un-theme them in dark mode
    for (HWND h : g_checks) SetWindowTheme(h, d ? L"" : nullptr, d ? L"" : nullptr);
    for (HWND h : g_edits)  SetWindowTheme(h, d ? L"DarkMode_CFD" : nullptr, d ? L"Combobox" : nullptr);
    SetWindowTheme(hProtList, d ? L"DarkMode_Explorer" : nullptr, nullptr);

    SetWindowTheme(hList, d ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(ListView_GetHeader(hList), d ? L"DarkMode_ItemsView" : nullptr, nullptr);
    ListView_SetBkColor(hList, ColCtrl());
    ListView_SetTextBkColor(hList, ColCtrl());
    ListView_SetTextColor(hList, ColText());

    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void CreateUI() {
    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    g_font = MakeFont(9, false); g_fontBig = MakeFont(18, true); g_fontBold = MakeFont(9, true);
    g_initUI = true;

    auto label = [&](const wchar_t* t, int x, int y, int w, int h, bool bold) {
        HWND c = Mk(L"STATIC", t, SS_LEFT, x, y, w, h, -1);
        if (bold) SendMessageW(c, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        return c;
    };
    auto sep = [&](int x, int y, int w) { Mk(L"STATIC", L"", SS_ETCHEDHORZ, x, y, w, 2, -1); };
    auto push = [&](const wchar_t* t, int x, int y, int w, int h, int id) {
        HWND c = Mk(L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id);
        g_push.push_back(c); return c;
    };
    auto check = [&](const wchar_t* t, int x, int y, int w, int id, DWORD extra = 0) {
        HWND c = Mk(L"BUTTON", t, extra | WS_TABSTOP, x, y, w, 22, id);
        g_checks.push_back(c); return c;
    };
    auto edit = [&](int x, int y, int w, int h, int id, DWORD extra = 0) {
        HWND c = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | extra, x, y, w, h, id, WS_EX_CLIENTEDGE);
        g_edits.push_back(c); return c;
    };

    // ----- LEFT COLUMN: hotkey, mode, options -----
    label(L"HOTKEY", 20, 16, 380, 18, true);
    hHotkey = Mk(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 20, 38, 380, 34, IDC_HOTKEY_TEXT);
    SendMessageW(hHotkey, WM_SETFONT, (WPARAM)g_fontBig, TRUE);
    hRecord = push(L"Record hotkey", 20, 78, 170, 30, IDC_RECORD);
    hStatus = Mk(L"STATIC", L"", SS_LEFT, 20, 114, 380, 40, IDC_STATUS);
    sep(20, 160, 380);

    label(L"CLOSE MODE", 20, 168, 380, 18, true);
    hRadioAll = check(L"Close all apps (except protected)", 20, 192, 380, IDC_RADIO_ALL, BS_AUTORADIOBUTTON | WS_GROUP);
    hRadioSel = check(L"Close only selected apps", 20, 216, 380, IDC_RADIO_SEL, BS_AUTORADIOBUTTON);
    sep(20, 248, 380);

    label(L"OPTIONS", 20, 256, 380, 18, true);
    hChkConfirm = check(L"Ask for confirmation before closing", 20, 280, 380, IDC_CHK_CONFIRM, BS_AUTOCHECKBOX);
    hChkForce   = check(L"Force-close apps that don't exit in time", 20, 304, 380, IDC_CHK_FORCE, BS_AUTOCHECKBOX);
    hTimeoutLbl = label(L"Wait before force-closing (seconds):", 42, 332, 230, 20, false);
    hTimeout    = edit(276, 329, 60, 24, IDC_TIMEOUT, ES_NUMBER | ES_CENTER);
    hChkAuto = check(L"Start with Windows (minimized to tray)", 20, 360, 380, IDC_CHK_AUTO, BS_AUTOCHECKBOX);
    hChkDark = check(L"Dark mode", 20, 384, 380, IDC_CHK_DARK, BS_AUTOCHECKBOX);
    sep(20, 418, 380);

    push(L"Close apps now", 20, 430, 190, 34, IDC_CLOSE_NOW);
    push(L"Hide to tray", 214, 430, 186, 34, IDC_HIDE);
    label(L"Settings are saved automatically.", 20, 474, 380, 18, false);

    // ----- RIGHT COLUMN: app picker + protected apps -----
    label(L"APP PICKER  (used in \"Close only selected apps\" mode)", 430, 16, 470, 18, true);
    hList = Mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
               430, 38, 470, 212, IDC_APPLIST);
    ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = { L"App", L"Process", L"Status" };
    int widths[] = { 210, 150, 80 };
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = S(widths[i]);
        ListView_InsertColumn(hList, i, &c);
    }
    push(L"Refresh", 430, 258, 80, 28, IDC_REFRESH);
    hManualEdit = edit(518, 259, 196, 26, IDC_MANUAL_EDIT);
    SendMessageW(hManualEdit, EM_SETCUEBANNER, FALSE, (LPARAM)L"add by name, e.g. chrome.exe");
    push(L"Add", 720, 258, 58, 28, IDC_MANUAL_ADD);
    push(L"Remove entry", 784, 258, 116, 28, IDC_MANUAL_REMOVE);
    sep(430, 298, 470);

    label(L"PROTECTED APPS  (never closed in \"Close all\" mode)", 430, 306, 470, 18, true);
    hProtList = Mk(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 430, 330, 270, 112, IDC_PROT_LIST, WS_EX_CLIENTEDGE);
    hProtEdit = edit(708, 330, 192, 26, IDC_PROT_EDIT);
    SendMessageW(hProtEdit, EM_SETCUEBANNER, FALSE, (LPARAM)L"e.g. discord.exe");
    push(L"Add to protected", 708, 362, 192, 28, IDC_PROT_ADD);
    push(L"Remove selected", 708, 396, 192, 28, IDC_PROT_REMOVE);
    label(L"Always protected (built in): explorer, Task Manager, system services, Windows "
          L"Security / common antivirus, and this app.", 430, 452, 470, 40, false);

    // ----- initial values -----
    SendMessageW(g_cfg.mode == 0 ? hRadioAll : hRadioSel, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(hChkConfirm, BM_SETCHECK, g_cfg.confirm ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hChkForce, BM_SETCHECK, g_cfg.force ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hChkAuto, BM_SETCHECK, g_cfg.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hChkDark, BM_SETCHECK, g_cfg.dark ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(hTimeout, std::to_wstring(g_cfg.timeoutSec).c_str());
    EnableWindow(hTimeout, g_cfg.force);
    EnableWindow(hTimeoutLbl, g_cfg.force);
    FillProtList();
    UpdateHotkeyText();
    g_initUI = false;
}

static void ShowSettings() {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
    RefreshAppList();
}
static bool Checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// =============================================================================
// 9. WINDOW PROCEDURE + WinMain
// =============================================================================
static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open settings");
    AppendMenuW(m, MF_STRING, ID_TRAY_CLOSE, L"Close apps now");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"Quit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);                  // required so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateUI();
        ApplyTheme();
        AddTray();
        RefreshAppList();
        if (!RegisterCurrentHotkey())
            SetStatus(L"Warning: " + FormatHotkey(g_cfg.mods, g_cfg.vk) +
                      L" is already taken by another app. Record a new hotkey.", 2);
        else
            SetStatus(L"Hotkey active. It works even when this window is hidden.", 1);
        return 0;

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brBg);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        COLORREF fg = ColText();
        if ((HWND)lp == hStatus) {
            if (g_statusKind == 1) fg = g_cfg.dark ? RGB(110, 210, 140) : RGB(20, 130, 60);
            else if (g_statusKind == 2) fg = g_cfg.dark ? RGB(255, 120, 120) : RGB(190, 30, 30);
        }
        SetTextColor(dc, fg);
        SetBkColor(dc, ColBg());
        SetBkMode(dc, OPAQUE);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, ColText());
        SetBkColor(dc, ColCtrl());
        return (LRESULT)g_brCtrl;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_RECORD:
            if (g_recording) CancelRecording(); else StartRecording();
            break;
        case IDC_RADIO_ALL:
        case IDC_RADIO_SEL:
            if (code == BN_CLICKED) { g_cfg.mode = (id == IDC_RADIO_SEL) ? 1 : 0; SaveConfig(); }
            break;
        case IDC_CHK_CONFIRM:
            if (code == BN_CLICKED) { g_cfg.confirm = Checked(hChkConfirm); SaveConfig(); }
            break;
        case IDC_CHK_FORCE:
            if (code == BN_CLICKED) {
                g_cfg.force = Checked(hChkForce);
                EnableWindow(hTimeout, g_cfg.force);
                EnableWindow(hTimeoutLbl, g_cfg.force);
                SaveConfig();
            }
            break;
        case IDC_TIMEOUT:
            if (code == EN_CHANGE && !g_initUI) {
                std::wstring t = GetText(hTimeout);
                if (!t.empty()) {
                    g_cfg.timeoutSec = std::max(1, std::min(MAX_TIMEOUT_SEC, _wtoi(t.c_str())));
                    SaveConfig();
                }
            } else if (code == EN_KILLFOCUS) {
                g_initUI = true;
                SetWindowTextW(hTimeout, std::to_wstring(g_cfg.timeoutSec).c_str());
                g_initUI = false;
            }
            break;
        case IDC_CHK_AUTO:
            if (code == BN_CLICKED) { g_cfg.autostart = Checked(hChkAuto); SetAutostart(g_cfg.autostart); SaveConfig(); }
            break;
        case IDC_CHK_DARK:
            if (code == BN_CLICKED) { g_cfg.dark = Checked(hChkDark); SaveConfig(); ApplyTheme(); }
            break;
        case IDC_REFRESH:       RefreshAppList(); break;
        case IDC_MANUAL_ADD:    AddManual(); break;
        case IDC_MANUAL_REMOVE: RemoveEntry(); break;
        case IDC_PROT_ADD:      ProtAdd(); break;
        case IDC_PROT_REMOVE:   ProtRemove(); break;
        case IDC_CLOSE_NOW: case ID_TRAY_CLOSE: TriggerClose(); break;
        case IDC_HIDE:          ShowWindow(hwnd, SW_HIDE); break;
        case ID_TRAY_OPEN:      ShowSettings(); break;
        case ID_TRAY_QUIT:      DestroyWindow(hwnd); break;
        case IDOK: {            // Enter key inside one of the edit boxes
            HWND f = GetFocus();
            if (f == hManualEdit) AddManual();
            else if (f == hProtEdit) ProtAdd();
            break;
        }
        case IDCANCEL:
            if (g_recording) CancelRecording();
            break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->idFrom == IDC_APPLIST && nm->code == LVN_ITEMCHANGED && !g_populating) {
            auto* lv = (NMLISTVIEW*)lp;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) &&
                lv->iItem >= 0 && lv->iItem < (int)g_rows.size()) {
                bool on = ((lv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                SetSelected(g_rows[lv->iItem].exe, on);
            }
        }
        return 0;
    }

    case WM_HOTKEY:
        if (wp == HK_ID && !g_recording) TriggerClose();
        return 0;

    case WM_RECORDED:       OnRecorded((UINT)wp, (UINT)lp); return 0;
    case WM_RECORD_CANCEL:  if (g_recording) CancelRecording(); return 0;

    case WM_CLOSE_DONE: {
        g_busy = false;
        int closed = (int)wp, failed = (int)lp;
        std::wstring m = L"Closed " + std::to_wstring(closed) + (closed == 1 ? L" app" : L" apps");
        if (failed > 0) m += L" (" + std::to_wstring(failed) + L" could not be closed)";
        Notify(APP_NAME, m);
        return 0;
    }

    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) ShowSettings();
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;

    case WM_SHOW_EXISTING:  // a second launch asked us to show the window
        ShowSettings();
        return 0;

    case WM_CLOSE:          // the X button hides to tray; use tray > Quit to exit
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        StopRecording();
        UnregisterHotKey(hwnd, HK_ID);
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_wmTaskbar && g_wmTaskbar) { AddTray(); return 0; }   // Explorer restarted
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    g_hInst = hInst;
    bool startTray = wcsstr(GetCommandLineW(), L"--tray") != nullptr;

    // Single instance: if already running, ask it to show its window and exit.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND e = FindWindowW(WND_CLASS, nullptr)) PostMessageW(e, WM_SHOW_EXISTING, 0, 0);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Our own exe name (so we never close ourselves)
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring sp = self;
    g_selfExe = Lower(sp.substr(sp.find_last_of(L"\\/") + 1));

    LoadConfig();
    if (g_cfg.autostart) SetAutostart(true);     // refresh the stored path in case the exe moved
    g_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    g_icoBig   = MakeAppIcon(32);
    g_icoSmall = MakeAppIcon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = WND_CLASS;
    wc.hIcon = g_icoBig;
    wc.hIconSm = g_icoSmall;
    RegisterClassExW(&wc);

    // Fixed-size window; client area = 920 x 510 design units, scaled for DPI
    HDC sdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, MulDiv(920, dpi, 96), MulDiv(510, dpi, 96) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    // Created hidden; shown below unless launched with --tray (autostart)
    HWND hwnd = CreateWindowExW(0, WND_CLASS, APP_NAME, style, x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    if (!startTray) {                            // normal launch: settings window opens first
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {     // gives us Tab navigation, Enter/Esc handling
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (mtx) CloseHandle(mtx);
    return (int)msg.wParam;
}
