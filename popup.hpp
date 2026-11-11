// popup.hpp - small dark notification that shows "Translating..." and then the result.
// Never takes focus (so a game keeps keyboard input).
//   [Keep]/[Unpin]  stops the countdown so the text stays until you close it (click again to resume)
//   [Close]         closes the popup immediately
#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include "capture.hpp"

// Filled in by the worker thread. `text` must be written before `done` is set.
struct Job {
    std::atomic<bool> done{false};
    std::string text;
    std::string notice;    // optional explanation shown above the text (e.g. why Gemini was not used)
};

// How long the result stays on screen. Set POPUP_SECONDS (1-600) to change it; default 10.
inline Uint32 PopupSeconds() {
    char buf[16];
    DWORD n = GetEnvironmentVariableA("POPUP_SECONDS", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        int v = std::atoi(buf);
        if (v >= 1 && v <= 600) return Uint32(v);
    }
    return 10;
}

inline void ShowPopup(const RectI& anchor, const RectI& monitor, std::shared_ptr<Job> job, int textPercent = 100) {
    if (!TTF_Init()) return;

    // Display scale -> readable font on 4K / 125% screens.
    SDL_Point pt{ anchor.x + anchor.w / 2, anchor.y + anchor.h / 2 };
    float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForPoint(&pt));
    if (scale <= 0.f) scale = 1.f;

    const float tp = std::clamp(textPercent, 50, 300) / 100.f;   // text size chosen in the tray menu
    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    TTF_Font* font = TTF_OpenFont(fontPath, 17.f * scale * tp);
    if (!font) { fontPath = "C:\\Windows\\Fonts\\arial.ttf"; font = TTF_OpenFont(fontPath, 17.f * scale * tp); }
    if (!font) { TTF_Quit(); return; }
    TTF_Font* uiFont = TTF_OpenFont(fontPath, 12.f * scale);       // buttons + countdown number
    TTF_Font* noticeFont = TTF_OpenFont(fontPath, 13.f * scale);   // the "why Gemini failed" note

    SDL_Window* win = SDL_CreateWindow("SnapTranslate", 200, 60,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_HIDDEN |
        SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_UTILITY);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!ren) {
        if (win) SDL_DestroyWindow(win);
        if (uiFont) TTF_CloseFont(uiFont);
        if (noticeFont) TTF_CloseFont(noticeFont);
        TTF_CloseFont(font); TTF_Quit();
        return;
    }

    // Rounded corners on Windows 11 (silently ignored on older versions).
    if (HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
        DWORD round = 2;                                  // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));   // DWMWA_WINDOW_CORNER_PREFERENCE
    }

    const int pad = int(14 * scale);
    const int maxTextW = std::min(int(520 * scale * std::max(1.f, tp)), monitor.w - 2 * pad - 40);
    const int footer = int(34 * scale);                   // room for buttons + progress bar
    const Uint32 totalMs = PopupSeconds() * 1000;

    SDL_Texture* textTex = nullptr;
    SDL_Texture* noticeTex = nullptr;
    int texW = 0, texH = 0, noticeW = 0, noticeH = 0, textTop = pad, winW = 0, winH = 0;

    bool showTimer = false;       // true once the translation is on screen
    bool pinned = false;          // true = countdown stopped, stays until Close
    bool hoverKeep = false, hoverClose = false;
    Uint64 deadline = 0;
    int lastSecs = -1;

    struct Label { SDL_Texture* tex = nullptr; int w = 0, h = 0; };
    auto makeLabel = [&](const char* s, SDL_Color c) {
        Label l;
        if (!uiFont) return l;
        if (SDL_Surface* surf = TTF_RenderText_Blended(uiFont, s, 0, c)) {
            l.tex = SDL_CreateTextureFromSurface(ren, surf);
            l.w = surf->w; l.h = surf->h;
            SDL_DestroySurface(surf);
        }
        return l;
    };
    const SDL_Color txtCol{ 225, 226, 235, 255 }, accent{ 76, 163, 255, 255 }, dimCol{ 140, 144, 160, 255 };
    Label keepL = makeLabel("Keep", txtCol), unpinL = makeLabel("Unpin", txtCol);
    Label closeL = makeLabel("Close", txtCol), pinnedL = makeLabel("Pinned", accent);
    Label timerL;                                          // "7s", rebuilt once per second

    // Button rectangles (window coordinates), bottom-right under the text.
    auto buttonRects = [&](SDL_FRect& keepR, SDL_FRect& closeR) {
        const float padX = 10.f * scale, gap = 8.f * scale;
        const float btnH = float(std::max(closeL.h, int(12 * scale))) + 6.f * scale;
        const float y = float(textTop + texH) + 3.f * scale;
        const float closeW = float(closeL.w) + 2 * padX;
        const float keepW = float(std::max(keepL.w, unpinL.w)) + 2 * padX;
        closeR = { float(winW - pad) - closeW, y, closeW, btnH };
        keepR  = { closeR.x - gap - keepW, y, keepW, btnH };
    };
    auto inside = [](const SDL_FRect& r, float x, float y) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    };
    auto drawButton = [&](const SDL_FRect& r, const Label& l, bool hover, bool active) {
        if (active)      SDL_SetRenderDrawColor(ren, 34, 78, 128, 255);
        else if (hover)  SDL_SetRenderDrawColor(ren, 62, 64, 80, 255);
        else             SDL_SetRenderDrawColor(ren, 40, 42, 52, 255);
        SDL_RenderFillRect(ren, &r);
        if (active) SDL_SetRenderDrawColor(ren, 76, 163, 255, 255);
        else        SDL_SetRenderDrawColor(ren, 78, 80, 96, 255);
        SDL_RenderRect(ren, &r);
        if (l.tex) {
            SDL_FRect d{ r.x + (r.w - float(l.w)) / 2, r.y + (r.h - float(l.h)) / 2, float(l.w), float(l.h) };
            SDL_RenderTexture(ren, l.tex, nullptr, &d);
        }
    };

    auto draw = [&]() {
        SDL_SetRenderDrawColor(ren, 28, 28, 34, 255);     // background
        SDL_RenderClear(ren);
        SDL_SetRenderDrawColor(ren, 70, 72, 86, 255);     // 1px border
        SDL_RenderRect(ren, nullptr);
        if (noticeTex) {
            SDL_FRect nd{ float(pad), float(pad), float(noticeW), float(noticeH) };
            SDL_RenderTexture(ren, noticeTex, nullptr, &nd);
            SDL_SetRenderDrawColor(ren, 70, 72, 86, 255);                 // thin divider under the note
            float ly = float(pad + noticeH) + 5.f * scale;
            SDL_RenderLine(ren, float(pad), ly, float(winW - pad), ly);
        }
        if (textTex) {
            SDL_FRect dst{ float(pad), float(textTop), float(texW), float(texH) };
            SDL_RenderTexture(ren, textTex, nullptr, &dst);
        }

        SDL_FRect keepR, closeR;
        buttonRects(keepR, closeR);

        Uint64 remain = 0;
        if (showTimer && !pinned) {
            Uint64 now = SDL_GetTicks();
            remain = deadline > now ? deadline - now : 0;
            int secs = int((remain + 999) / 1000);        // seconds left, rounded up
            if (secs != lastSecs) {
                lastSecs = secs;
                if (timerL.tex) { SDL_DestroyTexture(timerL.tex); timerL.tex = nullptr; }
                timerL = makeLabel((std::to_string(secs) + "s").c_str(), dimCol);
            }
        }
        // Status text on the left of the button row: countdown, or "Pinned".
        const Label& status = pinned ? pinnedL : timerL;
        if (showTimer && status.tex) {
            SDL_FRect d{ float(pad), keepR.y + (keepR.h - float(status.h)) / 2, float(status.w), float(status.h) };
            SDL_RenderTexture(ren, status.tex, nullptr, &d);
        }

        if (showTimer) drawButton(keepR, pinned ? unpinL : keepL, hoverKeep, pinned);
        drawButton(closeR, closeL, hoverClose, false);

        // Thin progress bar along the bottom edge that shrinks as time runs out.
        if (showTimer && !pinned) {
            float barH = std::max(2.f, 3.f * scale);
            SDL_FRect bar{ 1.f, float(winH) - barH - 1.f,
                           (float(winW) - 2.f) * float(remain) / float(totalMs), barH };
            SDL_SetRenderDrawColor(ren, 76, 163, 255, 255);
            SDL_RenderFillRect(ren, &bar);
        }
        SDL_RenderPresent(ren);
    };

    auto setText = [&](const std::string& text, const std::string& notice) {
        // Arabic letters are U+0600..U+06FF = UTF-8 lead bytes 0xD8..0xDB.
        bool rtl = false;
        for (unsigned char c : text) if (c >= 0xD8 && c <= 0xDB) { rtl = true; break; }
        TTF_SetFontDirection(font, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);
        TTF_SetFontScript(font, TTF_StringToTag(rtl ? "Arab" : "Latn"));
        TTF_SetFontWrapAlignment(font, rtl ? TTF_HORIZONTAL_ALIGN_RIGHT : TTF_HORIZONTAL_ALIGN_LEFT);

        SDL_Color fg{ 236, 236, 242, 255 };
        SDL_Surface* s = TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, fg, maxTextW);
        if (!s) return;
        if (textTex) SDL_DestroyTexture(textTex);
        textTex = SDL_CreateTextureFromSurface(ren, s);
        texW = s->w; texH = s->h;
        SDL_DestroySurface(s);

        // Optional note above the text, in a smaller amber font.
        if (noticeTex) { SDL_DestroyTexture(noticeTex); noticeTex = nullptr; }
        noticeW = noticeH = 0;
        textTop = pad;
        if (!notice.empty() && noticeFont) {
            SDL_Color amber{ 255, 196, 92, 255 };
            if (SDL_Surface* ns = TTF_RenderText_Blended_Wrapped(noticeFont, notice.c_str(), 0, amber, maxTextW)) {
                noticeTex = SDL_CreateTextureFromSurface(ren, ns);
                noticeW = ns->w; noticeH = ns->h;
                SDL_DestroySurface(ns);
                textTop = pad + noticeH + int(12 * scale);
            }
        }

        // Wide enough for the buttons even when the text is short.
        const int minW = int(220 * scale);
        int w = std::max(std::max(texW, noticeW) + 2 * pad, minW), h = textTop + texH + footer;
        winW = w; winH = h;
        int x = anchor.x;
        int y = anchor.y + anchor.h + int(10 * scale);                 // below the selection
        if (y + h > monitor.y + monitor.h) y = anchor.y - h - int(10 * scale);   // else above it
        x = std::clamp(x, monitor.x + 8, std::max(monitor.x + 8, monitor.x + monitor.w - w - 8));
        y = std::clamp(y, monitor.y + 8, std::max(monitor.y + 8, monitor.y + monitor.h - h - 8));
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, x, y);
        SDL_SyncWindow(win);
        draw();
    };

    setText("Translating...", "");
    SDL_ShowWindow(win);

    bool translating = true, quit = false;
    while (!quit) {
        bool dirty = false;
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50)) {                 // wakes 20x/s only while the popup is visible
            do {
                if (e.type == SDL_EVENT_QUIT) {
                    quit = true;
                } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                    SDL_FRect k, c;
                    buttonRects(k, c);
                    bool hk = showTimer && inside(k, e.motion.x, e.motion.y);
                    bool hc = inside(c, e.motion.x, e.motion.y);
                    if (hk != hoverKeep || hc != hoverClose) { hoverKeep = hk; hoverClose = hc; dirty = true; }
                } else if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
                    hoverKeep = hoverClose = false;
                    dirty = true;
                } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    SDL_FRect k, c;
                    buttonRects(k, c);
                    if (inside(c, e.button.x, e.button.y)) {
                        quit = true;                                  // Close
                    } else if (showTimer && inside(k, e.button.x, e.button.y)) {
                        pinned = !pinned;                             // Keep / Unpin
                        if (!pinned) { deadline = SDL_GetTicks() + totalMs; lastSecs = -1; }
                        dirty = true;
                    }
                } else if (e.type == SDL_EVENT_WINDOW_EXPOSED || e.type == SDL_EVENT_WINDOW_RESIZED) {
                    dirty = true;
                }
            } while (!quit && SDL_PollEvent(&e));
        }
        if (translating && job->done.load(std::memory_order_acquire)) {
            translating = false;
            deadline = SDL_GetTicks() + totalMs;
            showTimer = true;
            lastSecs = -1;
            setText(job->text, job->notice);                 // also redraws
            dirty = false;
        }
        if (showTimer && !pinned && SDL_GetTicks() >= deadline) quit = true;
        if (!quit && (dirty || (showTimer && !pinned))) draw();   // keeps countdown and bar moving
    }

    SDL_HideWindow(win);
    for (SDL_Texture* t : { textTex, noticeTex, keepL.tex, unpinL.tex, closeL.tex, pinnedL.tex, timerL.tex })
        if (t) SDL_DestroyTexture(t);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (uiFont) TTF_CloseFont(uiFont);
    if (noticeFont) TTF_CloseFont(noticeFont);
    TTF_CloseFont(font);
    TTF_Quit();
}
