// overlay.hpp - the Win+Shift+S-style selection screen.
// Shows the frozen screenshot, dimmed, in a borderless always-on-top window.
// The user drags a rectangle (shown at full brightness). Esc / right-click cancels.
#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include "capture.hpp"

// Returns true if the user selected a region. `sel` is in screenshot pixel coordinates.
inline bool SelectRegion(const Screenshot& shot, RectI& sel) {
    SDL_Window* win = SDL_CreateWindow("SnapTranslate", shot.w, shot.h,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_HIDDEN | SDL_WINDOW_UTILITY);
    if (!win) return false;
    SDL_SetWindowPosition(win, shot.bounds.x, shot.bounds.y);

    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) { SDL_DestroyWindow(win); return false; }
    // Work in screenshot pixels no matter what the window/DPI scale is.
    SDL_SetRenderLogicalPresentation(ren, shot.w, shot.h, SDL_LOGICAL_PRESENTATION_STRETCH);

    SDL_Surface* surf = SDL_CreateSurfaceFrom(shot.w, shot.h, SDL_PIXELFORMAT_XRGB8888,
                                              const_cast<uint8_t*>(shot.bgra.data()), shot.w * 4);
    SDL_Texture* tex = surf ? SDL_CreateTextureFromSurface(ren, surf) : nullptr;
    if (surf) SDL_DestroySurface(surf);
    if (!tex) { SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); return false; }

    SDL_Cursor* cross = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    if (cross) SDL_SetCursor(cross);

    auto draw = [&](bool hasSel, SDL_FRect r) {
        SDL_RenderTexture(ren, tex, nullptr, nullptr);              // frozen screen
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
        SDL_RenderFillRect(ren, nullptr);                           // dim everything
        if (hasSel && r.w >= 1 && r.h >= 1) {
            SDL_RenderTexture(ren, tex, &r, &r);                    // selection at full brightness
            SDL_SetRenderDrawColor(ren, 76, 163, 255, 255);
            SDL_RenderRect(ren, &r);
            SDL_FRect inner{ r.x + 1, r.y + 1, r.w - 2, r.h - 2 };
            if (inner.w > 0 && inner.h > 0) SDL_RenderRect(ren, &inner);
        }
        SDL_RenderPresent(ren);
    };

    draw(false, {});          // first frame is ready before the window becomes visible
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);

    bool dragging = false, accepted = false, done = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    auto clampx = [&](float v) { return std::clamp(v, 0.0f, float(shot.w)); };
    auto clampy = [&](float v) { return std::clamp(v, 0.0f, float(shot.h)); };
    auto current = [&]() {
        return SDL_FRect{ std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0) };
    };

    while (!done) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) break;           // sleeps until something happens: 0% CPU
        bool dirty = false;
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                done = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) done = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                SDL_ConvertEventToRenderCoordinates(ren, &e);
                if (e.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    x0 = x1 = clampx(e.button.x);
                    y0 = y1 = clampy(e.button.y);
                    dirty = true;
                } else {
                    done = true;                 // right / middle click cancels
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                SDL_ConvertEventToRenderCoordinates(ren, &e);
                if (dragging) { x1 = clampx(e.motion.x); y1 = clampy(e.motion.y); dirty = true; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                SDL_ConvertEventToRenderCoordinates(ren, &e);
                if (dragging && e.button.button == SDL_BUTTON_LEFT) {
                    x1 = clampx(e.button.x); y1 = clampy(e.button.y);
                    accepted = true; done = true;
                }
                break;
            default: break;
            }
        } while (!done && SDL_PollEvent(&e));    // drain queued mouse moves, draw once
        if (dirty && !done) draw(dragging, current());
    }

    SDL_HideWindow(win);                         // screen is back to normal immediately
    if (cross) SDL_DestroyCursor(cross);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);

    if (!accepted) return false;
    SDL_FRect r = current();
    sel = { int(r.x), int(r.y), int(r.w), int(r.h) };
    if (sel.x + sel.w > shot.w) sel.w = shot.w - sel.x;
    if (sel.y + sel.h > shot.h) sel.h = shot.h - sel.y;
    return sel.w >= 8 && sel.h >= 8;             // ignore accidental clicks
}
