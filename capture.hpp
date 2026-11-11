// capture.hpp - grab a frozen copy of the monitor under the mouse cursor.
// Uses plain GDI BitBlt (same API family the Snipping Tool uses). No hooks, no injection.
#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <vector>

struct RectI { int x = 0, y = 0, w = 0, h = 0; };

// One monitor, frozen at the moment the hotkey was pressed. Pixels are BGRX, top-down.
struct Screenshot {
    RectI bounds;                 // monitor rectangle in virtual-desktop pixels
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;    // w * h * 4 bytes
};

inline RectI MonitorUnderCursor() {
    POINT p{};
    GetCursorPos(&p);
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    return { mi.rcMonitor.left, mi.rcMonitor.top,
             mi.rcMonitor.right - mi.rcMonitor.left,
             mi.rcMonitor.bottom - mi.rcMonitor.top };
}

inline bool CaptureRect(const RectI& r, Screenshot& out) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = r.w;
    bi.bmiHeader.biHeight      = -r.h;          // negative = top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        ok = BitBlt(mem, 0, 0, r.w, r.h, screen, r.x, r.y, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(mem, old);
        if (ok) {
            out.bounds = r;
            out.w = r.w;
            out.h = r.h;
            const uint8_t* p = static_cast<const uint8_t*>(bits);
            out.bgra.assign(p, p + static_cast<size_t>(r.w) * r.h * 4);
        }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}
