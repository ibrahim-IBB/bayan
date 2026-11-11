// SnapTranslate - press a hotkey, drag a box over any text on screen, get the translation.
//
//   Idle:      lives in the system tray (the "^" area next to the clock), blocked in GetMessage()
//              -> 0% CPU. SDL is not even loaded until you capture.
//   Capture:   freeze screen -> drag selection -> overlay closes -> Gemini -> popup.
//   Tray:      left-click = translate now, right-click = menu (Start with Windows / Quit).
//   --once:    do a single capture and exit (no tray icon).
//
// Config (environment variables):
//   GEMINI_API_KEY   required
//   GEMINI_MODEL     optional, one model or a comma-separated fallback list. Tried in order; when one
//                    hits its limit the next is used. Default:
//                    "gemini-3.5-flash-lite,gemini-3.1-flash-lite,gemini-3.5-flash"
//   TARGET_LANG      optional, default "Arabic" (any language name Gemini understands)
//   POPUP_SECONDS    optional, default 10
//   OFFLINE_FALLBACK optional, set to 0 to disable the Windows-OCR + Google-Translate fallback
//   OCR_LANG         optional, OCR language tag for the fallback, e.g. "ja", "zh-Hans-CN", "en-US"
//   TARGET_LANG_CODE optional, language code for the fallback translator (default: derived from TARGET_LANG)
//   SNAP_NOFOCUS     optional, set to 1 for games that minimize when the overlay appears

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture.hpp"
#include "overlay.hpp"
#include "gemini.hpp"
#include "popup.hpp"
#include "fallback.hpp"

// ---- change the hotkeys here ----
static const UINT kCaptureMods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
static const UINT kCaptureKey  = 'T';       // Ctrl+Alt+T  = translate a region
static const UINT kQuitKey     = 'Q';       // Ctrl+Alt+Q  = quit the program

struct Config {
    std::string key;
    std::vector<std::string> models;
    std::string lang;        // e.g. "Arabic" (sent to Gemini)
    std::string langCode;    // e.g. "ar"     (sent to the fallback translator)
    std::string ocrLang;     // optional Windows OCR language tag
    bool fallback = true;
};

// Text size chosen from the tray menu (percent), remembered between runs.
static const int kTextSizes[] = { 80, 100, 125, 150, 200, 250 };
static const wchar_t* kTextSizeLabels[] = { L"Small  (80%)", L"Normal  (100%)", L"Large  (125%)",
                                            L"Larger  (150%)", L"Huge  (200%)", L"Giant  (250%)" };
static int g_textPercent = 100;
static const wchar_t* kSettingsKey = L"Software\\SnapTranslate";

static int LoadTextPercent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"TextPercent", RRF_RT_REG_DWORD,
                     nullptr, &v, &sz) == ERROR_SUCCESS && v >= 50 && v <= 300)
        return int(v);
    return 100;
}

static void SaveTextPercent(int percent) {
    DWORD v = DWORD(percent);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TextPercent", REG_DWORD, &v, sizeof(v));
}

static std::string EnvVar(const char* name) {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

// ---------------------------------------------------------------- capture workflow

static void RunCapture(const Config& cfg) {
    // 1) Freeze the screen FIRST, before any window exists, so we see exactly what the user saw.
    auto shot = std::make_shared<Screenshot>();
    if (!CaptureRect(MonitorUnderCursor(), *shot)) return;

    if (!SDL_Init(SDL_INIT_VIDEO)) return;

    // 2) Selection overlay. It is already destroyed when SelectRegion returns.
    RectI sel;
    if (SelectRegion(*shot, sel)) {
        // 3) Encode + upload on a worker thread while the popup shows "Translating...".
        auto job = std::make_shared<Job>();
        std::thread([job, shot, sel, cfg] {
            GeminiResult g = TranslateImage(EncodeJpeg(*shot, sel), cfg.key, cfg.models, cfg.lang);
            if (g.ok) {
                job->text = g.text;
            } else {
                job->notice = "Gemini could not translate: " + g.reason + ".";
                if (!cfg.fallback) {
                    job->text = "Translation failed.";
                } else {
                    // Fallback: Windows OCR (offline) -> Google's free translator.
                    OcrResult2 o = OcrRecognize(*shot, sel, cfg.ocrLang);
                    if (!o.ok) {
                        job->text = "The offline OCR fallback also failed: " + o.error + ".";
                    } else if (o.text.find_first_not_of(" \r\n\t") == std::string::npos) {
                        job->text = "The offline OCR fallback found no text in the selection.";
                    } else {
                        GoogleResult t = GoogleTranslate(o.text, cfg.langCode);
                        if (t.ok) {
                            job->text = t.text;
                            job->notice += " Showing Windows OCR + Google Translate instead.";
                        } else {
                            job->text = o.text;
                            job->notice += " " + t.error + ", so this is the untranslated text read by Windows OCR.";
                        }
                    }
                }
            }
            job->done.store(true, std::memory_order_release);
        }).detach();

        RectI anchor{ sel.x + shot->bounds.x, sel.y + shot->bounds.y, sel.w, sel.h };
        ShowPopup(anchor, shot->bounds, job, g_textPercent);
    }
    SDL_Quit();   // give everything back until the next capture
}

// Shows a sample popup so you can see the text size you just picked.
static void RunSizeDemo() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return;
    POINT p{};
    GetCursorPos(&p);
    auto job = std::make_shared<Job>();
    job->text = "Sample text: this is how your translations will look.";
    job->done.store(true, std::memory_order_release);
    ShowPopup(RectI{ p.x, p.y, 1, 1 }, MonitorUnderCursor(), job, g_textPercent);
    SDL_Quit();
}

// ---------------------------------------------------------------- tray icon + hidden window

static const UINT WM_TRAY = WM_APP + 1;
enum : UINT { ID_TRANSLATE = 1001, ID_AUTOSTART = 1002, ID_QUIT = 1003, ID_SIZE_BASE = 2000 };
enum : int  { HK_CAPTURE = 1, HK_QUIT = 2 };

static bool g_request = false;        // a capture was asked for (hotkey, tray click or menu)
static bool g_busy = false;           // a capture is running right now
static bool g_demo = false;           // show the text-size sample popup
static DWORD g_delayMs = 0;           // let a closing menu/flyout disappear before we freeze the screen
static UINT g_taskbarCreated = 0;
static NOTIFYICONDATAW g_nid{};

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"SnapTranslate";

static bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(path) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, value.c_str(),
                        DWORD((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
    }
}

// The icon comes from the exe's resources (built from the icon\ folder), else the default app icon.
static HICON LoadAppIcon() {
    const int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                            cx, cy, LR_DEFAULTCOLOR));
    if (!h) {                                         // fallback: SnapTranslate.ico next to the exe
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (n > 0 && path[n - 1] != L'\\') --n;
        path[n] = 0;
        std::wstring ico = std::wstring(path) + L"SnapTranslate.ico";
        h = static_cast<HICON>(LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, cx, cy, LR_LOADFROMFILE));
    }
    return h ? h : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

static void AddTrayIcon(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadAppIcon();
    lstrcpynW(g_nid.szTip, L"SnapTranslate  (Ctrl+Alt+T)", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRANSLATE, L"Translate now\tCtrl+Alt+T");
    HMENU sizes = CreatePopupMenu();
    const int nSizes = int(ARRAYSIZE(kTextSizes));
    int selected = 1;
    for (int i = 0; i < nSizes; ++i) {
        AppendMenuW(sizes, MF_STRING, ID_SIZE_BASE + i, kTextSizeLabels[i]);
        if (kTextSizes[i] == g_textPercent) selected = i;
    }
    CheckMenuRadioItem(sizes, 0, nSizes - 1, selected, MF_BYPOSITION);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"Text size");
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit");

    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);                       // required so the menu closes when you click away
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (!g_busy) {
            if (wp == HK_CAPTURE) g_request = true;
            else if (wp == HK_QUIT) PostQuitMessage(0);
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (!g_busy) { g_request = true; g_delayMs = 250; }
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRANSLATE: if (!g_busy) { g_request = true; g_delayMs = 250; } break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_QUIT:      PostQuitMessage(0); break;
        default: {
            int i = int(LOWORD(wp)) - int(ID_SIZE_BASE);
            if (i >= 0 && i < int(ARRAYSIZE(kTextSizes)) && !g_busy) {
                g_textPercent = kTextSizes[i];
                SaveTextPercent(g_textPercent);
                g_demo = true;
            }
            break;
        }
        }
        return 0;
    default:
        if (msg == g_taskbarCreated) {                // Explorer restarted: put the icon back
            Shell_NotifyIconW(NIM_ADD, &g_nid);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- entry point

int main(int argc, char** argv) {
    // Physical-pixel coordinates everywhere (GDI capture and SDL windows agree on 125%/150% displays).
    SetProcessDpiAwarenessContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));   // PER_MONITOR_AWARE_V2

    Config cfg;
    cfg.key = EnvVar("GEMINI_API_KEY");
    std::string models = EnvVar("GEMINI_MODEL");
    if (models.empty()) models = "gemini-3.5-flash-lite,gemini-3.1-flash-lite,gemini-3.5-flash";
    cfg.models = SplitModels(models);
    cfg.lang = EnvVar("TARGET_LANG");
    if (cfg.lang.empty()) cfg.lang = "Arabic";
    cfg.langCode = EnvVar("TARGET_LANG_CODE");
    if (cfg.langCode.empty()) cfg.langCode = LangCode(cfg.lang);
    cfg.ocrLang = EnvVar("OCR_LANG");
    cfg.fallback = EnvVar("OFFLINE_FALLBACK") != "0";
    g_textPercent = LoadTextPercent();

    if (cfg.key.empty()) {
        MessageBoxA(nullptr,
            "GEMINI_API_KEY is not set.\n\nOpen a terminal and run:\n  setx GEMINI_API_KEY \"your-key\"\nthen start SnapTranslate again.",
            "SnapTranslate", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (argc > 1 && std::strcmp(argv[1], "--once") == 0) {
        RunCapture(cfg);
        return 0;
    }

    // Only one resident copy at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SnapTranslateSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Hidden window: receives the hotkey and tray-icon messages.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnapTranslateHiddenWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SnapTranslate", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    if (!RegisterHotKey(hwnd, HK_CAPTURE, kCaptureMods, kCaptureKey)) {
        MessageBoxA(nullptr, "Could not register Ctrl+Alt+T (another program is using it).",
                    "SnapTranslate", MB_OK | MB_ICONERROR);
        return 1;
    }
    RegisterHotKey(hwnd, HK_QUIT, kCaptureMods, kQuitKey);
    AddTrayIcon(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {      // sleeps here between captures
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_request) {
            g_request = false;
            g_busy = true;
            if (g_delayMs) { Sleep(g_delayMs); g_delayMs = 0; }
            RunCapture(cfg);
            g_busy = false;
        }
        if (g_demo) {
            g_demo = false;
            g_busy = true;
            RunSizeDemo();
            g_busy = false;
        }
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    UnregisterHotKey(hwnd, HK_CAPTURE);
    UnregisterHotKey(hwnd, HK_QUIT);
    DestroyWindow(hwnd);
    if (mutex) CloseHandle(mutex);
    return 0;
}
