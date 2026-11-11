// gemini.hpp - crop -> JPEG -> Gemini generateContent -> English text.
// Dependencies: WinHTTP (built into Windows) + stb_image_write.h (single header).
#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "capture.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ---------- image ----------

// Crop `r` out of the screenshot and encode it as JPEG.
inline std::vector<uint8_t> EncodeJpeg(const Screenshot& s, const RectI& r, int quality = 90) {
    std::vector<uint8_t> rgb(static_cast<size_t>(r.w) * r.h * 3);
    for (int y = 0; y < r.h; ++y) {
        const uint8_t* src = &s.bgra[(static_cast<size_t>(r.y + y) * s.w + r.x) * 4];
        uint8_t* dst = &rgb[static_cast<size_t>(y) * r.w * 3];
        for (int x = 0; x < r.w; ++x, src += 4, dst += 3) {
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];   // BGRX -> RGB
        }
    }
    std::vector<uint8_t> out;
    auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    stbi_write_jpg_to_func(sink, &out, r.w, r.h, 3, rgb.data(), quality);
    return out;
}

inline std::string Base64(const std::vector<uint8_t>& d) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8) | d[i + 2];
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == d.size()) {
        uint32_t v = uint32_t(d[i]) << 16;
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == d.size()) {
        uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8);
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

// ---------- minimal JSON string extraction (enough for Gemini's reply) ----------

inline void AppendUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
        o += char(cp);
    } else if (cp < 0x800) {
        o += char(0xC0 | (cp >> 6));
        o += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += char(0xE0 | (cp >> 12));
        o += char(0x80 | ((cp >> 6) & 0x3F));
        o += char(0x80 | (cp & 0x3F));
    } else {
        o += char(0xF0 | (cp >> 18));
        o += char(0x80 | ((cp >> 12) & 0x3F));
        o += char(0x80 | ((cp >> 6) & 0x3F));
        o += char(0x80 | (cp & 0x3F));
    }
}

// Decoded value of the first `"key": "..."` string in the JSON text, or "" if none.
inline std::string JsonString(const std::string& j, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return {};
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = j.find('"', p);
    if (p == std::string::npos) return {};
    ++p;

    auto hex4 = [&](size_t at) { return uint32_t(std::strtoul(j.substr(at, 4).c_str(), nullptr, 16)); };
    std::string out;
    while (p < j.size() && j[p] != '"') {
        char c = j[p++];
        if (c != '\\') { out += c; continue; }
        if (p >= j.size()) break;
        char e = j[p++];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': case 'b': case 'f': break;
        case 'u': {
            uint32_t cp = hex4(p); p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= j.size() && j[p] == '\\' && j[p + 1] == 'u') {
                uint32_t lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            AppendUtf8(out, cp);
            break;
        }
        default: out += e; break;      // \" \\ \/
        }
    }
    return out;
}

// ---------- HTTPS ----------

struct HttpResult { int status = 0; std::string body; };

inline HttpResult HttpsPost(const std::wstring& host, const std::wstring& path,
                            const std::wstring& headers, const std::string& body,
                            const wchar_t* userAgent = L"SnapTranslate/1.0") {
    HttpResult r;
    HINTERNET ses = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return r;
    WinHttpSetTimeouts(ses, 5000, 5000, 10000, 25000);   // resolve, connect, send, receive (ms)
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req &&
        WinHttpSendRequest(req, headers.c_str(), DWORD(-1L), const_cast<char*>(body.data()),
                           DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = int(code);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail) {
            size_t old = r.body.size();
            r.body.resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &r.body[old], avail, &got)) { r.body.resize(old); break; }
            r.body.resize(old + got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return r;
}

// ---------- the one call the app needs ----------

// "a, b,c" -> {"a","b","c"}
inline std::vector<std::string> SplitModels(const std::string& list) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string m = list.substr(i, j - i);
        size_t a = m.find_first_not_of(" \t"), b = m.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(m.substr(a, b - a + 1));
        i = j + 1;
    }
    return out;
}

struct GeminiResult {
    bool ok = false;
    std::string text;      // the translation when ok
    std::string reason;    // short human-readable cause when !ok (shown to the user)
};

inline std::string Shorten(const std::string& s, size_t n = 150) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

inline std::string GeminiReason(int status, const std::string& msg) {
    std::string why;
    if (status == 429)                              why = "quota or rate limit reached";
    else if (status == 400 && msg.find("API key") != std::string::npos) why = "the API key is invalid";
    else if (status == 400)                         why = "the request was rejected";
    else if (status == 401 || status == 403)        why = "the API key was rejected or lacks permission";
    else if (status == 404)                         why = "the model is not available";
    else if (status >= 500)                         why = "Google's server is overloaded or down";
    else                                            why = "unexpected error";
    return why + " (HTTP " + std::to_string(status) + ")" + (msg.empty() ? "" : ": " + Shorten(msg));
}

// Tries the models in order. If one is out of quota (429), unavailable (404/403) or overloaded (5xx),
// the next one is used. A model that hit its limit is tried last for the next few minutes.
inline GeminiResult TranslateImage(const std::vector<uint8_t>& jpeg, const std::string& apiKey,
                                   const std::vector<std::string>& models, const std::string& lang) {
    const std::string prompt =
        "Translate any text found in this image directly to " + lang + ". "
        "Return ONLY the final translation, with absolutely no conversational filler or markdown notes.";

    const std::string body = std::string("{\"contents\":[{\"parts\":[{\"text\":\"") + prompt +
        "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"" + Base64(jpeg) +
        "\"}}]}],\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":1024}}";

    // The key travels in a header (never in the URL, so it can't leak into logs or history).
    const std::wstring headers = L"Content-Type: application/json\r\nx-goog-api-key: " +
                                 std::wstring(apiKey.begin(), apiKey.end()) + L"\r\n";

    static std::mutex mu;
    static std::map<std::string, ULONGLONG> coolUntil;      // model -> tick count when it may be used again

    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(mu);
        const ULONGLONG now = GetTickCount64();
        for (const auto& m : models) if (coolUntil[m] <= now) order.push_back(m);
        for (const auto& m : models) if (coolUntil[m] >  now) order.push_back(m);
    }

    GeminiResult out;
    out.reason = "no model configured";
    for (const std::string& model : order) {
        const std::wstring path = L"/v1beta/models/" + std::wstring(model.begin(), model.end()) + L":generateContent";
        HttpResult res = HttpsPost(L"generativelanguage.googleapis.com", path, headers, body);

        if (res.status == 0) {
            out.reason = "no internet connection, or Google could not be reached";
            return out;
        }

        if (res.status == 200) {
            std::string text = JsonString(res.body, "text");
            size_t a = text.find_first_not_of(" \r\n\t");
            size_t b = text.find_last_not_of(" \r\n\t");
            text = (a == std::string::npos) ? std::string() : text.substr(a, b - a + 1);
            if (!text.empty()) { out.ok = true; out.text = text; return out; }
            if (res.body.find("SAFETY") != std::string::npos || res.body.find("blockReason") != std::string::npos) {
                out.reason = "Gemini blocked this image (safety filter)";
                return out;
            }
            out.ok = true; out.text = "(no text found)";     // a normal answer: the image simply has no text
            return out;
        }

        std::string msg = JsonString(res.body, "message");
        out.reason = "[" + model + "] " + GeminiReason(res.status, msg);

        if (res.status == 429) {                             // out of quota: rest this model for a while
            const bool daily = res.body.find("PerDay") != std::string::npos;
            std::lock_guard<std::mutex> lock(mu);
            coolUntil[model] = GetTickCount64() + (daily ? 3600000ULL : 60000ULL);
        }
        const bool tryNext = res.status == 429 || res.status == 404 || res.status == 403 || res.status >= 500;
        if (!tryNext) break;                                 // e.g. 400 / 401: the next model won't help
    }
    return out;
}
