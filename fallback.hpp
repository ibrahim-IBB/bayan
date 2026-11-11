// fallback.hpp - used only when Gemini fails:
//   1) Windows' built-in OCR (Windows.Media.Ocr): offline, no model to download, uses the OCR
//      language packs installed in Windows.
//   2) Google's free (unofficial) translate endpoint: no key. It can be rate-limited or change
//      without notice, which is fine for a fallback.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "capture.hpp"
#include "gemini.hpp"      // HttpsPost, AppendUtf8

// ---------------------------------------------------------------- Windows OCR

struct OcrResult2 {
    bool ok = false;
    std::string text;      // recognized text (UTF-8)
    std::string error;     // why it failed
};

// Windows OCR puts a space between every CJK character; remove those so translation reads naturally.
inline std::string CollapseCjkSpaces(const std::string& in) {
    std::vector<uint32_t> cp;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        uint32_t v; int n;
        if (c < 0x80)           { v = c;        n = 1; }
        else if ((c >> 5) == 6) { v = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14){ v = c & 0x0F; n = 3; }
        else                    { v = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < in.size(); ++k) v = (v << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
        cp.push_back(v);
        i += n;
    }
    auto cjk = [](uint32_t v) { return (v >= 0x3000 && v <= 0x9FFF) || (v >= 0xFF00 && v <= 0xFFEF); };
    std::string out;
    for (size_t i = 0; i < cp.size(); ++i) {
        if (cp[i] == ' ' && i > 0 && i + 1 < cp.size() && cjk(cp[i - 1]) && cjk(cp[i + 1])) continue;
        AppendUtf8(out, cp[i]);
    }
    return out;
}

#if __has_include(<winrt/Windows.Media.Ocr.h>)
#include <unknwn.h>
#ifdef small
#undef small
#endif
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

// ocrLang: optional BCP-47 tag such as "ja", "zh-Hans-CN", "en-US". Empty = your Windows languages.
inline OcrResult2 OcrRecognize(const Screenshot& s, const RectI& r, const std::string& ocrLang) {
    OcrResult2 res;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Storage::Streams;

        // Small text is read much better when enlarged.
        int k = r.h < 60 ? 3 : (r.h < 120 ? 2 : 1);
        const int W = r.w * k, H = r.h * k;
        if (uint32_t(W) > OcrEngine::MaxImageDimension() || uint32_t(H) > OcrEngine::MaxImageDimension()) {
            res.error = "the selection is too large for Windows OCR";
            return res;
        }

        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
        for (int y = 0; y < H; ++y) {
            const uint8_t* src = &s.bgra[(static_cast<size_t>(r.y + y / k) * s.w + r.x) * 4];
            uint8_t* dst = &px[static_cast<size_t>(y) * W * 4];
            for (int x = 0; x < W; ++x) {
                const uint8_t* p = src + (x / k) * 4;
                dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst[3] = 255;   // GDI leaves alpha at 0
                dst += 4;
            }
        }

        DataWriter writer;
        writer.WriteBytes(winrt::array_view<const uint8_t>(px.data(), static_cast<uint32_t>(px.size())));
        IBuffer buffer = writer.DetachBuffer();
        SoftwareBitmap bmp = SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8, W, H);

        OcrEngine engine{ nullptr };
        if (!ocrLang.empty()) {
            std::wstring w(ocrLang.begin(), ocrLang.end());
            engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(winrt::hstring(w)));
            if (!engine) { res.error = "no Windows OCR language pack for \"" + ocrLang + "\""; return res; }
        } else {
            engine = OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine) { res.error = "no Windows OCR language pack is installed"; return res; }
        }

        auto result = engine.RecognizeAsync(bmp).get();
        res.text = CollapseCjkSpaces(winrt::to_string(result.Text()));
        res.ok = true;
    } catch (const winrt::hresult_error& e) {
        res.error = winrt::to_string(e.message());
    } catch (...) {
        res.error = "Windows OCR failed";
    }
    return res;
}
#else
inline OcrResult2 OcrRecognize(const Screenshot&, const RectI&, const std::string&) {
    OcrResult2 res;
    res.error = "this build has no Windows OCR support (C++/WinRT headers not found)";
    return res;
}
#endif

// ---------------------------------------------------------------- Google Translate (free endpoint)

// "Arabic" -> "ar". Anything that already looks like a code ("ar", "zh-CN") is passed through.
inline std::string LangCode(const std::string& name) {
    std::string n;
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    static const struct { const char* name; const char* code; } kMap[] = {
        {"arabic","ar"}, {"english","en"}, {"swedish","sv"}, {"french","fr"}, {"german","de"},
        {"spanish","es"}, {"turkish","tr"}, {"russian","ru"}, {"japanese","ja"}, {"korean","ko"},
        {"chinese","zh-CN"}, {"persian","fa"}, {"kurdish","ku"}, {"italian","it"}, {"portuguese","pt"},
        {"hindi","hi"}, {"urdu","ur"}, {"dutch","nl"}, {"polish","pl"}, {"ukrainian","uk"},
    };
    for (const auto& e : kMap) if (n == e.name) return e.code;
    if (n.size() <= 6 && n.find(' ') == std::string::npos) return name;   // already a code
    return "en";
}

inline std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// The reply looks like [[["translated part","original part",...],["next part","..."]],null,"en",...].
// Collect the first string of every inner array.
inline std::string ParseGoogleReply(const std::string& j) {
    std::string out;
    int depth = 0, idx = 0, top2 = 0;     // top2 counts the arrays directly inside the outer one
    for (size_t i = 0; i < j.size(); ++i) {
        char c = j[i];
        if (c == '[') {
            ++depth;
            if (depth == 2) ++top2;
            if (depth == 3) idx = 0;
        } else if (c == ']') {
            --depth;
            if (depth <= 0) break;
        } else if (c == ',') {
            if (depth == 3) ++idx;
        } else if (c == '"') {
            std::string str;
            for (++i; i < j.size() && j[i] != '"'; ++i) {
                char d = j[i];
                if (d != '\\') { str += d; continue; }
                if (++i >= j.size()) break;
                char e = j[i];
                if (e == 'n') str += '\n';
                else if (e == 't') str += '\t';
                else if (e == 'u' && i + 4 < j.size()) {
                    uint32_t cp = uint32_t(std::strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                        uint32_t lo = uint32_t(std::strtoul(j.substr(i + 3, 4).c_str(), nullptr, 16));
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    AppendUtf8(str, cp);
                }
                else if (e != 'r') str += e;
            }
            if (depth == 3 && idx == 0 && top2 == 1) out += str;   // only the translation segments
        }
    }
    return out;
}

struct GoogleResult {
    bool ok = false;
    std::string text;
    std::string error;
};

inline GoogleResult GoogleTranslate(const std::string& text, const std::string& targetCode) {
    GoogleResult r;
    std::string clipped = text.size() > 4000 ? text.substr(0, 4000) : text;
    std::wstring path = L"/translate_a/single?client=gtx&sl=auto&dt=t&tl=" +
                        std::wstring(targetCode.begin(), targetCode.end());
    std::string body = "q=" + UrlEncode(clipped);
    HttpResult res = HttpsPost(L"translate.googleapis.com", path,
                               L"Content-Type: application/x-www-form-urlencoded\r\n", body, L"Mozilla/5.0");
    if (res.status == 0) { r.error = "could not reach Google Translate"; return r; }
    if (res.status != 200) { r.error = "Google Translate returned HTTP " + std::to_string(res.status); return r; }
    r.text = ParseGoogleReply(res.body);
    if (r.text.empty()) { r.error = "Google Translate returned no text"; return r; }
    r.ok = true;
    return r;
}
