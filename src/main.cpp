#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <immintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

enum class Theme : int { Light, Dark };
enum class MouseButton : int { Left, Right, Middle };
enum class TargetMode : int { Cursor, Anchor };

static constexpr int kAppIconId = 101;

struct Settings {
    Theme theme = Theme::Dark;
    bool sounds = true;
    bool softStart = true;
    double intervalMs = 15.0;
    int jitter = 4;
    int burst = 1;
    unsigned long long limit = 0;
    MouseButton button = MouseButton::Left;
    TargetMode target = TargetMode::Cursor;
    POINT anchor{ 0, 0 };
    bool hasAnchor = false;
    UINT hotkey = VK_F6;
};

struct Palette {
    COLORREF bg0;
    COLORREF bg1;
    COLORREF panel;
    COLORREF panel2;
    COLORREF surface;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF accent2;
    COLORREF good;
    COLORREF bad;
    COLORREF input;
    COLORREF shadow;
};

struct FieldLayout {
    RECT box{};
    HWND edit{};
    std::wstring label;
};

static int ClampInt(int v, int lo, int hi) {
    return std::min(std::max(v, lo), hi);
}

static double ClampDouble(double v, double lo, double hi) {
    return std::min(std::max(v, lo), hi);
}

static unsigned long long ClampU64(unsigned long long v, unsigned long long lo, unsigned long long hi) {
    return std::min(std::max(v, lo), hi);
}

static int64_t QpcNow() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

static std::wstring ToWide(int v) {
    wchar_t b[32]{};
    swprintf_s(b, L"%d", v);
    return b;
}

static std::wstring ToWideDouble(double v, int decimals = 3) {
    wchar_t b[64]{};
    swprintf_s(b, L"%.*f", decimals, v);
    std::wstring s = b;
    if (auto dot = s.find(L'.'); dot != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == L'.') {
            s.pop_back();
        }
    }
    return s;
}

static std::wstring ToWideU64(unsigned long long v) {
    wchar_t b[64]{};
    swprintf_s(b, L"%llu", v);
    return b;
}

static std::wstring PointText(POINT pt) {
    return ToWide(pt.x) + L", " + ToWide(pt.y);
}

static std::wstring ToRate(double v) {
    wchar_t b[64]{};
    if (v >= 100.0) {
        swprintf_s(b, L"%.0f cps", v);
    } else {
        swprintf_s(b, L"%.1f cps", v);
    }
    return b;
}

static std::wstring HotkeyText(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t b[16]{};
        swprintf_s(b, L"F%u", vk - VK_F1 + 1);
        return b;
    }
    return L"F6";
}

static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    auto m = [t](BYTE x, BYTE y) -> BYTE {
        return static_cast<BYTE>(std::lround((1.0 - t) * x + t * y));
    };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

static Palette PaletteFor(Theme theme) {
    if (theme == Theme::Dark) {
        return {
            RGB(10, 16, 28),
            RGB(20, 32, 54),
            RGB(20, 26, 42),
            RGB(30, 40, 66),
            RGB(16, 22, 36),
            RGB(62, 83, 118),
            RGB(239, 246, 255),
            RGB(149, 170, 201),
            RGB(56, 189, 248),
            RGB(34, 211, 238),
            RGB(74, 222, 128),
            RGB(248, 113, 113),
            RGB(15, 23, 42),
            RGB(2, 6, 23)
        };
    }
    return {
        RGB(248, 251, 255),
        RGB(230, 242, 255),
        RGB(255, 255, 255),
        RGB(244, 248, 255),
        RGB(236, 243, 252),
        RGB(198, 214, 233),
        RGB(14, 30, 55),
        RGB(86, 106, 137),
        RGB(14, 116, 215),
        RGB(6, 182, 212),
        RGB(22, 163, 74),
        RGB(220, 38, 38),
        RGB(245, 249, 255),
        RGB(142, 169, 205)
    };
}

static Color ToColor(COLORREF c, BYTE a = 255) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static RectF ToRectF(const RECT& rc) {
    return RectF(static_cast<REAL>(rc.left), static_cast<REAL>(rc.top), static_cast<REAL>(rc.right - rc.left), static_cast<REAL>(rc.bottom - rc.top));
}

static void Rounded(GraphicsPath& path, const RECT& rc, int radius) {
    REAL x = static_cast<REAL>(rc.left);
    REAL y = static_cast<REAL>(rc.top);
    REAL w = static_cast<REAL>(rc.right - rc.left);
    REAL h = static_cast<REAL>(rc.bottom - rc.top);
    REAL d = static_cast<REAL>(radius * 2);
    if (radius <= 0) {
        path.AddRectangle(RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static bool Contains(const RECT& rc, POINT pt) {
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

static bool Intersects(const RECT& a, const RECT& b) {
    RECT out{};
    return IntersectRect(&out, &a, &b) != FALSE;
}

static void NormalizeDecimalSeparator(wchar_t* text) {
    if (!text) {
        return;
    }
    while (*text != 0) {
        if (*text == L',') {
            *text = L'.';
        }
        ++text;
    }
}

static bool HasOnlyTrailingWhitespace(const wchar_t* text) {
    if (!text) {
        return false;
    }
    while (*text != 0) {
        if (*text != L' ' && *text != L'\t' && *text != L'\r' && *text != L'\n') {
            return false;
        }
        ++text;
    }
    return true;
}

static bool TryParseDouble(const wchar_t* text, double& value) {
    if (!text) {
        return false;
    }
    wchar_t* end = nullptr;
    double parsed = wcstod(text, &end);
    if (!end || end == text || !HasOnlyTrailingWhitespace(end)) {
        return false;
    }
    value = parsed;
    return true;
}

static bool TryParseInt(const wchar_t* text, int& value) {
    if (!text) {
        return false;
    }
    wchar_t* end = nullptr;
    long parsed = wcstol(text, &end, 10);
    if (!end || end == text || !HasOnlyTrailingWhitespace(end)) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

static bool TryParseU64(const wchar_t* text, unsigned long long& value) {
    if (!text) {
        return false;
    }
    wchar_t* end = nullptr;
    unsigned long long parsed = _wcstoui64(text, &end, 10);
    if (!end || end == text || !HasOnlyTrailingWhitespace(end)) {
        return false;
    }
    value = parsed;
    return true;
}

static bool CreateMemorySurface(HDC reference, int width, int height, HDC& dc, HBITMAP& bitmap, HGDIOBJ& oldBitmap, SIZE& size) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP newBitmap = CreateDIBSection(reference, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!newBitmap) {
        return false;
    }

    HDC newDc = CreateCompatibleDC(reference);
    if (!newDc) {
        DeleteObject(newBitmap);
        return false;
    }

    HGDIOBJ newOldBitmap = SelectObject(newDc, newBitmap);
    if (!newOldBitmap || newOldBitmap == HGDI_ERROR) {
        DeleteDC(newDc);
        DeleteObject(newBitmap);
        return false;
    }

    dc = newDc;
    bitmap = newBitmap;
    oldBitmap = newOldBitmap;
    size = SIZE{ width, height };
    return true;
}

static std::wstring ConfigPath() {
    PWSTR base = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)) && base) {
        result = base;
        CoTaskMemFree(base);
    } else {
        wchar_t fallback[MAX_PATH]{};
        GetTempPathW(MAX_PATH, fallback);
        result = fallback;
    }
    result += L"\\FlowAutoclicker";
    CreateDirectoryW(result.c_str(), nullptr);
    result += L"\\settings.ini";
    return result;
}

static int IniReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(section, key, def, path.c_str());
}

static double IniReadDouble(const std::wstring& path, const wchar_t* section, const wchar_t* key, double def) {
    wchar_t b[64]{};
    auto d = ToWideDouble(def);
    GetPrivateProfileStringW(section, key, d.c_str(), b, static_cast<DWORD>(std::size(b)), path.c_str());
    NormalizeDecimalSeparator(b);
    double value = def;
    return TryParseDouble(b, value) ? value : def;
}

static unsigned long long IniReadU64(const std::wstring& path, const wchar_t* section, const wchar_t* key, unsigned long long def) {
    wchar_t b[64]{};
    wchar_t d[64]{};
    swprintf_s(d, L"%llu", def);
    GetPrivateProfileStringW(section, key, d, b, static_cast<DWORD>(std::size(b)), path.c_str());
    unsigned long long value = def;
    return TryParseU64(b, value) ? value : def;
}

static void IniWriteInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int value) {
    auto s = ToWide(value);
    WritePrivateProfileStringW(section, key, s.c_str(), path.c_str());
}

static void IniWriteDouble(const std::wstring& path, const wchar_t* section, const wchar_t* key, double value) {
    auto s = ToWideDouble(value);
    WritePrivateProfileStringW(section, key, s.c_str(), path.c_str());
}

static void IniWriteU64(const std::wstring& path, const wchar_t* section, const wchar_t* key, unsigned long long value) {
    auto s = ToWideU64(value);
    WritePrivateProfileStringW(section, key, s.c_str(), path.c_str());
}

#pragma pack(push, 1)
struct WavHeader {
    char riff[4];
    uint32_t riffSize;
    char wave[4];
    char fmt_[4];
    uint32_t fmtSize;
    uint16_t format;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
};
#pragma pack(pop)

class AudioBank {
public:
    AudioBank() {
        start_ = Tone(520.0, 900.0, 90, 0.25);
        stop_ = Tone(840.0, 430.0, 110, 0.25);
        anchor_ = Tone(780.0, 1040.0, 70, 0.22);
        ui_ = Tone(660.0, 760.0, 42, 0.18);
        enabled_ = Tone(560.0, 860.0, 72, 0.23);
        disabled_ = Tone(660.0, 420.0, 78, 0.23);
        warn_ = Tone(430.0, 270.0, 88, 0.20);
    }

    void PlayStart(bool enabled) const {
        if (enabled) {
            Play(start_);
        }
    }

    void PlayStop(bool enabled) const {
        if (enabled) {
            Play(stop_);
        }
    }

    void PlayAnchor(bool enabled) const {
        if (enabled) {
            Play(anchor_);
        }
    }

    void PlayUi(bool enabled) const {
        if (enabled) {
            Play(ui_);
        }
    }

    void PlayEnabled(bool enabled) const {
        if (enabled) {
            Play(enabled_);
        }
    }

    void PlayDisabled(bool enabled) const {
        if (enabled) {
            Play(disabled_);
        }
    }

    void PlayWarn(bool enabled) const {
        if (enabled) {
            Play(warn_);
        }
    }

private:
    std::vector<BYTE> start_;
    std::vector<BYTE> stop_;
    std::vector<BYTE> anchor_;
    std::vector<BYTE> ui_;
    std::vector<BYTE> enabled_;
    std::vector<BYTE> disabled_;
    std::vector<BYTE> warn_;

    static std::vector<BYTE> Tone(double beginHz, double endHz, int ms, double gain) {
        const int sampleRate = 44100;
        const size_t samples = static_cast<size_t>(sampleRate * ms / 1000);
        std::vector<BYTE> out(sizeof(WavHeader) + samples * sizeof(int16_t));
        auto* header = reinterpret_cast<WavHeader*>(out.data());
        std::memcpy(header->riff, "RIFF", 4);
        std::memcpy(header->wave, "WAVE", 4);
        std::memcpy(header->fmt_, "fmt ", 4);
        std::memcpy(header->data, "data", 4);
        header->riffSize = static_cast<uint32_t>(out.size() - 8);
        header->fmtSize = 16;
        header->format = 1;
        header->channels = 1;
        header->sampleRate = sampleRate;
        header->byteRate = sampleRate * sizeof(int16_t);
        header->blockAlign = sizeof(int16_t);
        header->bitsPerSample = 16;
        header->dataSize = static_cast<uint32_t>(samples * sizeof(int16_t));
        auto* pcm = reinterpret_cast<int16_t*>(out.data() + sizeof(WavHeader));
        double phase = 0.0;
        for (size_t i = 0; i < samples; ++i) {
            double t = samples > 1 ? static_cast<double>(i) / static_cast<double>(samples - 1) : 0.0;
            double f = beginHz + (endHz - beginHz) * t;
            phase += 6.28318530717958647692 * f / static_cast<double>(sampleRate);
            double env = std::pow(std::sin(t * 3.14159265358979323846), 1.6);
            double s = std::sin(phase) * env * gain + std::sin(phase * 0.5) * env * gain * 0.16;
            pcm[i] = static_cast<int16_t>(std::lround(std::clamp(s, -1.0, 1.0) * 32767.0));
        }
        return out;
    }

    static void Play(const std::vector<BYTE>& wav) {
        PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
    }
};

class ClickEngine {
public:
    struct Config {
        double intervalMs = 15.0;
        int jitter = 4;
        int burst = 1;
        unsigned long long limit = 0;
        MouseButton button = MouseButton::Left;
        TargetMode target = TargetMode::Cursor;
        POINT anchor{ 0, 0 };
        bool hasAnchor = false;
        bool softStart = true;
    };

    ClickEngine() {
        wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (wake_ && stop_) {
            thread_ = std::thread([this] { Loop(); });
        }
    }

    ~ClickEngine() {
        exiting_.store(true);
        if (stop_) {
            SetEvent(stop_);
        }
        if (wake_) {
            SetEvent(wake_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (wake_) {
            CloseHandle(wake_);
        }
        if (stop_) {
            CloseHandle(stop_);
        }
    }

    void SetConfig(const Config& cfg) {
        std::scoped_lock lock(mutex_);
        config_ = cfg;
    }

    void Start() {
        if (!wake_ || !stop_) {
            return;
        }
        count_.store(0);
        avgLagUs_.store(0);
        ResetEvent(stop_);
        running_.store(true);
        SetEvent(wake_);
    }

    void Stop() {
        running_.store(false);
        if (stop_) {
            SetEvent(stop_);
        }
    }

    bool Running() const {
        return running_.load();
    }

    unsigned long long Count() const {
        return count_.load();
    }

    bool HighRes() const {
        return highRes_.load();
    }

    int AvgLagUs() const {
        return avgLagUs_.load();
    }

private:
    HANDLE wake_{};
    HANDLE stop_{};
    std::thread thread_;
    std::mutex mutex_;
    Config config_{};
    std::atomic<bool> running_{ false };
    std::atomic<bool> exiting_{ false };
    std::atomic<bool> highRes_{ false };
    std::atomic<unsigned long long> count_{ 0 };
    std::atomic<int> avgLagUs_{ 0 };

    static DWORD DownFlag(MouseButton button) {
        switch (button) {
        case MouseButton::Right:
            return MOUSEEVENTF_RIGHTDOWN;
        case MouseButton::Middle:
            return MOUSEEVENTF_MIDDLEDOWN;
        default:
            return MOUSEEVENTF_LEFTDOWN;
        }
    }

    static DWORD UpFlag(MouseButton button) {
        switch (button) {
        case MouseButton::Right:
            return MOUSEEVENTF_RIGHTUP;
        case MouseButton::Middle:
            return MOUSEEVENTF_MIDDLEUP;
        default:
            return MOUSEEVENTF_LEFTUP;
        }
    }

    static LONG NormalizeX(int x) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vw = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
        return static_cast<LONG>(std::lround((static_cast<double>(x - vx) * 65535.0) / static_cast<double>(std::max(vw - 1, 1))));
    }

    static LONG NormalizeY(int y) {
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vh = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
        return static_cast<LONG>(std::lround((static_cast<double>(y - vy) * 65535.0) / static_cast<double>(std::max(vh - 1, 1))));
    }

    static void Emit(const Config& cfg, int burst) {
        std::array<INPUT, 18> inputs{};
        int n = 0;
        auto addMove = [&](POINT pt) {
            INPUT i{};
            i.type = INPUT_MOUSE;
            i.mi.dx = NormalizeX(pt.x);
            i.mi.dy = NormalizeY(pt.y);
            i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            inputs[n++] = i;
        };
        DWORD down = DownFlag(cfg.button);
        DWORD up = UpFlag(cfg.button);
        if (cfg.target == TargetMode::Anchor && cfg.hasAnchor) {
            POINT current{};
            GetCursorPos(&current);
            addMove(cfg.anchor);
            for (int i = 0; i < burst; ++i) {
                INPUT d{};
                d.type = INPUT_MOUSE;
                d.mi.dwFlags = down;
                inputs[n++] = d;
                INPUT u{};
                u.type = INPUT_MOUSE;
                u.mi.dwFlags = up;
                inputs[n++] = u;
            }
            addMove(current);
        } else {
            for (int i = 0; i < burst; ++i) {
                INPUT d{};
                d.type = INPUT_MOUSE;
                d.mi.dwFlags = down;
                inputs[n++] = d;
                INPUT u{};
                u.type = INPUT_MOUSE;
                u.mi.dwFlags = up;
                inputs[n++] = u;
            }
        }
        if (n > 0) {
            SendInput(n, inputs.data(), sizeof(INPUT));
        }
    }

    static bool WaitUntil(HANDLE timer, HANDLE stop, int64_t target, int64_t coarse, int64_t spin, int64_t freq) {
        for (;;) {
            int64_t now = QpcNow();
            int64_t remain = target - now;
            if (remain <= 0) {
                return WaitForSingleObject(stop, 0) != WAIT_OBJECT_0;
            }
            if (remain > coarse && timer) {
                int64_t lead = remain - coarse;
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(std::max<int64_t>(1, static_cast<int64_t>(std::llround(static_cast<double>(lead) * 10000000.0 / static_cast<double>(freq)))));
                SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
                HANDLE handles[2]{ stop, timer };
                DWORD waited = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                CancelWaitableTimer(timer);
                if (waited == WAIT_OBJECT_0) {
                    return false;
                }
                continue;
            }
            while ((target - QpcNow()) > spin) {
                if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
                    return false;
                }
                SwitchToThread();
            }
            while (QpcNow() < target) {
                _mm_pause();
                if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
                    return false;
                }
            }
            return WaitForSingleObject(stop, 0) != WAIT_OBJECT_0;
        }
    }

    Config Snapshot() {
        std::scoped_lock lock(mutex_);
        return config_;
    }

    void Loop() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer) {
            highRes_.store(true);
        } else {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            highRes_.store(false);
        }
        LARGE_INTEGER fq{};
        QueryPerformanceFrequency(&fq);
        while (!exiting_.load()) {
            if (WaitForSingleObject(wake_, INFINITE) != WAIT_OBJECT_0) {
                break;
            }
            if (exiting_.load()) {
                break;
            }
            ResetEvent(wake_);
            if (!running_.load()) {
                continue;
            }
            count_.store(0);
            int64_t sessionStart = QpcNow();
            int64_t next = sessionStart;
            double phase = 0.1732050807;
            while (running_.load() && !exiting_.load()) {
                Config cfg = Snapshot();
                cfg.intervalMs = ClampDouble(cfg.intervalMs, 0.1, 60000.0);
                cfg.jitter = ClampInt(cfg.jitter, 0, 50);
                cfg.burst = ClampInt(cfg.burst, 1, 8);
                double speed = 1.0;
                if (cfg.softStart) {
                    double elapsedMs = static_cast<double>(QpcNow() - sessionStart) * 1000.0 / static_cast<double>(fq.QuadPart);
                    if (elapsedMs < 550.0) {
                        double t = elapsedMs / 550.0;
                        double s = t * t * (3.0 - 2.0 * t);
                        speed = 0.35 + 0.65 * s;
                    }
                }
                int64_t interval = std::max<int64_t>(1, static_cast<int64_t>(std::llround((cfg.intervalMs / speed) * static_cast<double>(fq.QuadPart) / 1000.0)));
                phase += 0.61803398875;
                if (phase >= 1.0) {
                    phase -= 1.0;
                }
                double phase2 = phase + 0.38196601125;
                if (phase2 >= 1.0) {
                    phase2 -= 1.0;
                }
                double noise = phase + phase2 - 1.0;
                int64_t jitter = static_cast<int64_t>(std::llround(static_cast<double>(interval) * (static_cast<double>(cfg.jitter) / 100.0) * noise * 0.45));
                next += std::max<int64_t>(1, interval + jitter);
                if (!WaitUntil(timer, stop_, next, fq.QuadPart / 600, fq.QuadPart / 5000, fq.QuadPart)) {
                    break;
                }
                int lag = static_cast<int>(std::llround(std::abs(static_cast<double>(QpcNow() - next)) * 1000000.0 / static_cast<double>(fq.QuadPart)));
                avgLagUs_.store((avgLagUs_.load() * 7 + lag) / 8);
                if (!running_.load() || exiting_.load()) {
                    break;
                }
                unsigned long long done = count_.load();
                unsigned long long remaining = cfg.limit == 0 ? ~0ull : (done < cfg.limit ? cfg.limit - done : 0ull);
                if (remaining == 0) {
                    running_.store(false);
                    break;
                }
                int burst = static_cast<int>(std::min<unsigned long long>(static_cast<unsigned long long>(cfg.burst), remaining));
                Emit(cfg, burst);
                count_.fetch_add(static_cast<unsigned long long>(burst));
                if (cfg.limit != 0 && count_.load() >= cfg.limit) {
                    running_.store(false);
                    break;
                }
            }
            ResetEvent(stop_);
            running_.store(false);
        }
        if (timer) {
            CloseHandle(timer);
        }
    }
};

enum HitId {
    HitNone,
    HitTheme,
    HitSound,
    HitSoft,
    HitStart,
    HitAnchor,
    HitHotkey,
    HitLeft,
    HitRight,
    HitMiddle,
    HitCursor,
    HitAnchorMode
};

static constexpr UINT_PTR kUiTimerId = 1;
static constexpr UINT kUiTimerAnchorMs = 33;
static constexpr UINT kUiTimerActiveMs = 50;
static constexpr UINT kUiTimerBannerMs = 200;

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    int Run(int show);

private:
    HINSTANCE instance_{};
    HWND hwnd_{};
    Settings settings_{};
    ClickEngine engine_{};
    AudioBank audio_{};
    std::wstring configPath_{ ConfigPath() };
    int dpi_{ 96 };
    HFONT editFont_{};
    HBRUSH editBrush_{};
    FieldLayout intervalField_{ {}, nullptr, L"Interval (ms)" };
    FieldLayout jitterField_{ {}, nullptr, L"Jitter (%)" };
    FieldLayout burstField_{ {}, nullptr, L"Burst" };
    FieldLayout limitField_{ {}, nullptr, L"Limit" };
    RECT timingCard_{};
    RECT modeCard_{};
    RECT liveCard_{};
    RECT themeRect_{};
    RECT soundRect_{};
    RECT softRect_{};
    RECT startRect_{};
    RECT anchorRect_{};
    RECT hotkeyRect_{};
    std::array<RECT, 3> buttonRects_{};
    std::array<RECT, 2> targetRects_{};
    HDC staticCacheDc_{};
    HBITMAP staticCacheBmp_{};
    HGDIOBJ staticCacheOldBmp_{};
    SIZE staticCacheSize_{};
    HDC paintBufferDc_{};
    HBITMAP paintBufferBmp_{};
    HGDIOBJ paintBufferOldBmp_{};
    SIZE paintBufferSize_{};
    bool staticCacheDirty_{ true };
    bool syncing_{ false };
    bool hotkeyCapture_{ false };
    bool hotkeyOk_{ true };
    bool hotkeyFallback_{ false };
    bool anchorCapture_{ false };
    bool anchorMouseDown_{ false };
    bool anchorEscapeDown_{ false };
    bool lastRunning_{ false };
    int hover_{ HitNone };
    std::wstring banner_;
    ULONGLONG bannerUntil_{ 0 };
    unsigned long long lastCountSample_{ 0 };
    ULONGLONG lastSampleTick_{ 0 };
    double liveRate_{ 0.0 };
    int lastLagSample_{ 0 };
    UINT uiTimerIntervalMs_{ 0 };

    static App* Self(HWND hwnd);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
    int Scale(int v) const;
    void OnCreate();
    void OnDestroy();
    void OnDpiChanged(UINT dpi, RECT* suggested);
    void CenterWindow();
    void RebuildFont();
    void CreateControls();
    void Layout();
    void PlaceEdit(const FieldLayout& field);
    void ApplyTheme();
    HBRUSH OnEditColor(HDC hdc);
    ClickEngine::Config BuildConfig() const;
    void LoadSettings();
    void SaveSettings();
    void SyncControlsFromSettings();
    static double ReadDoubleBox(HWND edit, double fallback);
    static int ReadIntBox(HWND edit, int fallback);
    static unsigned long long ReadU64Box(HWND edit, unsigned long long fallback);
    void SyncSettingsFromControls();
    bool RegisterHotkey();
    void BeginAnchorCapture();
    void CompleteAnchorCapture();
    void CancelAnchorCapture();
    void ToggleEngine();
    void Banner(const std::wstring& text);
    UINT DesiredUiTimerInterval() const;
    void UpdateUiTimer();
    void MarkStaticCacheDirty();
    void DestroyStaticCache();
    void EnsureStaticCache(HDC reference, const RECT& clientRc);
    void DestroyPaintBuffer();
    void EnsurePaintBuffer(HDC reference, const RECT& clientRc);
    RECT HitBounds(int hit) const;
    void InvalidateSection(const RECT& rc);
    void InvalidateLiveCard();
    void OnTimer();
    void OnMouseMove(int x, int y);
    void OnClick(int x, int y);
    void OnHotkeyCapture(UINT vk);
    int HitTest(POINT pt) const;
    void DrawStaticChrome(Graphics& g, const RECT& rc, const Palette& p);
    void DrawTimingOverlay(Graphics& g, const Palette& p);
    void DrawModeOverlay(Graphics& g, const Palette& p);
    void DrawLiveOverlay(Graphics& g, const Palette& p);
    void DrawTextBlock(Graphics& g, const std::wstring& text, const RectF& rc, const Font& font, COLORREF color, StringAlignment align = StringAlignmentNear, StringAlignment line = StringAlignmentNear, bool wrap = false, StringTrimming trimming = StringTrimmingEllipsisCharacter);
    void DrawShadow(Graphics& g, const RECT& rc, int radius, COLORREF color);
    void DrawCard(Graphics& g, const RECT& rc, const Palette& p, bool accent = false);
    void DrawPill(Graphics& g, const RECT& rc, const Palette& p, const std::wstring& label, bool selected, bool hovered);
    void DrawField(Graphics& g, const FieldLayout& field, const Palette& p);
    void OnPaint();
};

int App::Run(int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = App::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = L"FlowAutoclickerWindow";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    dpi_ = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT frame{ 0, 0, Scale(1060), Scale(670) };
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi_);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Flow Autoclicker", style, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return 0;
    }
    CenterWindow();
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

App* App::Self(HWND hwnd) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app = reinterpret_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = hwnd;
    }
    if (auto* app = Self(hwnd)) {
        return app->Handle(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT frame{ 0, 0, Scale(1060), Scale(670) };
        DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, dpi_);
        info->ptMinTrackSize.x = frame.right - frame.left;
        info->ptMinTrackSize.y = frame.bottom - frame.top;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            SetCursor(LoadCursorW(nullptr, HitTest(pt) == HitNone ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        if (hover_ != HitNone) {
            int previous = hover_;
            hover_ = HitNone;
            InvalidateSection(HitBounds(previous));
        }
        return 0;
    case WM_LBUTTONUP:
        OnClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_KEYDOWN:
        if (hotkeyCapture_) {
            OnHotkeyCapture(static_cast<UINT>(wParam));
            return 0;
        }
        if (anchorCapture_) {
            if (wParam == VK_ESCAPE) {
                CancelAnchorCapture();
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                CompleteAnchorCapture();
                return 0;
            }
        }
        break;
    case WM_HOTKEY:
        ToggleEngine();
        return 0;
    case WM_TIMER:
        if (wParam == kUiTimerId) {
            OnTimer();
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && !syncing_) {
            SyncSettingsFromControls();
        }
        return 0;
    case WM_CTLCOLOREDIT:
        return reinterpret_cast<LRESULT>(OnEditColor(reinterpret_cast<HDC>(wParam)));
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_DESTROY:
        OnDestroy();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

int App::Scale(int v) const {
    return MulDiv(v, dpi_, 96);
}

void App::OnCreate() {
    LoadSettings();
    CreateControls();
    RebuildFont();
    SyncControlsFromSettings();
    Layout();
    ApplyTheme();
    RegisterHotkey();
    lastSampleTick_ = GetTickCount64();
    lastCountSample_ = 0;
    liveRate_ = 0.0;
    lastLagSample_ = 0;
    banner_ = L"Autoclicker ready";
    bannerUntil_ = GetTickCount64() + 2800;
    engine_.SetConfig(BuildConfig());
    UpdateUiTimer();
}

void App::OnDestroy() {
    if (uiTimerIntervalMs_ != 0) {
        KillTimer(hwnd_, kUiTimerId);
        uiTimerIntervalMs_ = 0;
    }
    engine_.Stop();
    DestroyStaticCache();
    DestroyPaintBuffer();
    SaveSettings();
    UnregisterHotKey(hwnd_, 1);
    if (editBrush_) {
        DeleteObject(editBrush_);
    }
    if (editFont_) {
        DeleteObject(editFont_);
    }
    PostQuitMessage(0);
}

void App::OnDpiChanged(UINT dpi, RECT* suggested) {
    dpi_ = static_cast<int>(dpi);
    SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    RebuildFont();
    Layout();
    ApplyTheme();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CenterWindow() {
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::RebuildFont() {
    if (editFont_) {
        DeleteObject(editFont_);
    }
    editFont_ = CreateFontW(-MulDiv(16, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Comic Sans MS");
    for (HWND edit : { intervalField_.edit, jitterField_.edit, burstField_.edit, limitField_.edit }) {
        if (edit) {
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
        }
    }
}

void App::CreateControls() {
    DWORD decimalStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL;
    DWORD wholeStyle = decimalStyle | ES_NUMBER;
    intervalField_.edit = CreateWindowExW(0, L"EDIT", L"", decimalStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(101), instance_, nullptr);
    jitterField_.edit = CreateWindowExW(0, L"EDIT", L"", wholeStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(102), instance_, nullptr);
    burstField_.edit = CreateWindowExW(0, L"EDIT", L"", wholeStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(103), instance_, nullptr);
    limitField_.edit = CreateWindowExW(0, L"EDIT", L"", wholeStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(104), instance_, nullptr);
    for (HWND edit : { intervalField_.edit, jitterField_.edit, burstField_.edit, limitField_.edit }) {
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(8), Scale(8)));
        SetWindowTheme(edit, L"", L"");
    }
}

void App::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int m = Scale(24);
    int gap = Scale(18);
    int cardTop = Scale(118);
    int cardH = rc.bottom - cardTop - m;
    int timingW = Scale(320);
    int modeW = Scale(292);
    int liveW = rc.right - m * 2 - gap * 2 - timingW - modeW;
    timingCard_ = RECT{ m, cardTop, m + timingW, cardTop + cardH };
    modeCard_ = RECT{ timingCard_.right + gap, cardTop, timingCard_.right + gap + modeW, cardTop + cardH };
    liveCard_ = RECT{ modeCard_.right + gap, cardTop, rc.right - m, cardTop + cardH };

    themeRect_ = RECT{ rc.right - m - Scale(250), Scale(28), rc.right - m - Scale(128), Scale(70) };
    soundRect_ = RECT{ rc.right - m - Scale(116), Scale(28), rc.right - m, Scale(70) };

    int pad = Scale(18);
    int cellGap = Scale(14);
    int cellW = (timingCard_.right - timingCard_.left - pad * 2 - cellGap) / 2;
    int cellH = Scale(90);
    int cellTop = timingCard_.top + Scale(96);
    intervalField_.box = RECT{ timingCard_.left + pad, cellTop, timingCard_.left + pad + cellW, cellTop + cellH };
    jitterField_.box = RECT{ intervalField_.box.right + cellGap, cellTop, intervalField_.box.right + cellGap + cellW, cellTop + cellH };
    burstField_.box = RECT{ timingCard_.left + pad, intervalField_.box.bottom + cellGap, timingCard_.left + pad + cellW, intervalField_.box.bottom + cellGap + cellH };
    limitField_.box = RECT{ burstField_.box.right + cellGap, intervalField_.box.bottom + cellGap, burstField_.box.right + cellGap + cellW, intervalField_.box.bottom + cellGap + cellH };

    PlaceEdit(intervalField_);
    PlaceEdit(jitterField_);
    PlaceEdit(burstField_);
    PlaceEdit(limitField_);

    int modePad = Scale(18);
    int segH = Scale(46);
    int segGap = Scale(10);
    int segY = modeCard_.top + Scale(106);
    int segW = (modeCard_.right - modeCard_.left - modePad * 2 - segGap * 2) / 3;
    buttonRects_[0] = RECT{ modeCard_.left + modePad, segY, modeCard_.left + modePad + segW, segY + segH };
    buttonRects_[1] = RECT{ buttonRects_[0].right + segGap, segY, buttonRects_[0].right + segGap + segW, segY + segH };
    buttonRects_[2] = RECT{ buttonRects_[1].right + segGap, segY, buttonRects_[1].right + segGap + segW, segY + segH };

    int targetY = segY + segH + Scale(72);
    int targetW = (modeCard_.right - modeCard_.left - modePad * 2 - segGap) / 2;
    targetRects_[0] = RECT{ modeCard_.left + modePad, targetY, modeCard_.left + modePad + targetW, targetY + segH };
    targetRects_[1] = RECT{ targetRects_[0].right + segGap, targetY, targetRects_[0].right + segGap + targetW, targetY + segH };

    anchorRect_ = RECT{ modeCard_.left + modePad, targetRects_[1].bottom + Scale(64), modeCard_.right - modePad, targetRects_[1].bottom + Scale(112) };
    softRect_ = RECT{ modeCard_.left + modePad, anchorRect_.bottom + Scale(22), modeCard_.right - modePad, anchorRect_.bottom + Scale(64) };
    hotkeyRect_ = RECT{ modeCard_.left + modePad, softRect_.bottom + Scale(26), modeCard_.right - modePad, softRect_.bottom + Scale(78) };

    startRect_ = RECT{ liveCard_.left + Scale(24), liveCard_.bottom - Scale(108), liveCard_.right - Scale(24), liveCard_.bottom - Scale(34) };
    MarkStaticCacheDirty();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::PlaceEdit(const FieldLayout& field) {
    MoveWindow(field.edit, field.box.left + Scale(12), field.box.top + Scale(38), field.box.right - field.box.left - Scale(24), Scale(34), TRUE);
}

void App::ApplyTheme() {
    Palette p = PaletteFor(settings_.theme);
    if (editBrush_) {
        DeleteObject(editBrush_);
    }
    editBrush_ = CreateSolidBrush(p.input);
    BOOL dark = settings_.theme == Theme::Dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    for (HWND edit : { intervalField_.edit, jitterField_.edit, burstField_.edit, limitField_.edit }) {
        if (edit) {
            InvalidateRect(edit, nullptr, TRUE);
        }
    }
    MarkStaticCacheDirty();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

HBRUSH App::OnEditColor(HDC hdc) {
    Palette p = PaletteFor(settings_.theme);
    SetBkColor(hdc, p.input);
    SetTextColor(hdc, p.text);
    SetBkMode(hdc, OPAQUE);
    return editBrush_;
}

ClickEngine::Config App::BuildConfig() const {
    ClickEngine::Config cfg;
    cfg.intervalMs = settings_.intervalMs;
    cfg.jitter = settings_.jitter;
    cfg.burst = settings_.burst;
    cfg.limit = settings_.limit;
    cfg.button = settings_.button;
    cfg.target = settings_.target;
    cfg.anchor = settings_.anchor;
    cfg.hasAnchor = settings_.hasAnchor;
    cfg.softStart = settings_.softStart;
    return cfg;
}

void App::LoadSettings() {
    settings_.theme = IniReadInt(configPath_, L"ui", L"theme", 1) ? Theme::Dark : Theme::Light;
    settings_.sounds = IniReadInt(configPath_, L"ui", L"sounds", 1) != 0;
    settings_.softStart = IniReadInt(configPath_, L"ui", L"softStart", 1) != 0;
    settings_.intervalMs = ClampDouble(IniReadDouble(configPath_, L"engine", L"intervalMs", 15.0), 0.1, 60000.0);
    settings_.jitter = ClampInt(IniReadInt(configPath_, L"engine", L"jitter", 4), 0, 50);
    settings_.burst = ClampInt(IniReadInt(configPath_, L"engine", L"burst", 1), 1, 8);
    settings_.limit = ClampU64(IniReadU64(configPath_, L"engine", L"limit", 0), 0, 1000000000ull);
    settings_.button = static_cast<MouseButton>(ClampInt(IniReadInt(configPath_, L"engine", L"button", 0), 0, 2));
    settings_.target = static_cast<TargetMode>(ClampInt(IniReadInt(configPath_, L"engine", L"target", 0), 0, 1));
    settings_.anchor.x = IniReadInt(configPath_, L"engine", L"anchorX", 0);
    settings_.anchor.y = IniReadInt(configPath_, L"engine", L"anchorY", 0);
    settings_.hasAnchor = IniReadInt(configPath_, L"engine", L"hasAnchor", 0) != 0;
    settings_.hotkey = static_cast<UINT>(ClampInt(IniReadInt(configPath_, L"ui", L"hotkey", VK_F6), VK_F1, VK_F24));
}

void App::SaveSettings() {
    IniWriteInt(configPath_, L"ui", L"theme", settings_.theme == Theme::Dark ? 1 : 0);
    IniWriteInt(configPath_, L"ui", L"sounds", settings_.sounds ? 1 : 0);
    IniWriteInt(configPath_, L"ui", L"softStart", settings_.softStart ? 1 : 0);
    IniWriteInt(configPath_, L"ui", L"hotkey", static_cast<int>(settings_.hotkey));
    IniWriteDouble(configPath_, L"engine", L"intervalMs", settings_.intervalMs);
    IniWriteInt(configPath_, L"engine", L"jitter", settings_.jitter);
    IniWriteInt(configPath_, L"engine", L"burst", settings_.burst);
    IniWriteU64(configPath_, L"engine", L"limit", settings_.limit);
    IniWriteInt(configPath_, L"engine", L"button", static_cast<int>(settings_.button));
    IniWriteInt(configPath_, L"engine", L"target", static_cast<int>(settings_.target));
    IniWriteInt(configPath_, L"engine", L"anchorX", settings_.anchor.x);
    IniWriteInt(configPath_, L"engine", L"anchorY", settings_.anchor.y);
    IniWriteInt(configPath_, L"engine", L"hasAnchor", settings_.hasAnchor ? 1 : 0);
}

void App::SyncControlsFromSettings() {
    syncing_ = true;
    SetWindowTextW(intervalField_.edit, ToWideDouble(settings_.intervalMs).c_str());
    SetWindowTextW(jitterField_.edit, ToWide(settings_.jitter).c_str());
    SetWindowTextW(burstField_.edit, ToWide(settings_.burst).c_str());
    SetWindowTextW(limitField_.edit, ToWideU64(settings_.limit).c_str());
    syncing_ = false;
}

double App::ReadDoubleBox(HWND edit, double fallback) {
    wchar_t b[32]{};
    GetWindowTextW(edit, b, static_cast<int>(std::size(b)));
    if (b[0] == 0) {
        return fallback;
    }
    NormalizeDecimalSeparator(b);
    double value = fallback;
    return TryParseDouble(b, value) ? value : fallback;
}

int App::ReadIntBox(HWND edit, int fallback) {
    wchar_t b[32]{};
    GetWindowTextW(edit, b, static_cast<int>(std::size(b)));
    if (b[0] == 0) {
        return fallback;
    }
    int value = fallback;
    return TryParseInt(b, value) ? value : fallback;
}

unsigned long long App::ReadU64Box(HWND edit, unsigned long long fallback) {
    wchar_t b[64]{};
    GetWindowTextW(edit, b, static_cast<int>(std::size(b)));
    if (b[0] == 0) {
        return fallback;
    }
    unsigned long long value = fallback;
    return TryParseU64(b, value) ? value : fallback;
}

void App::SyncSettingsFromControls() {
    settings_.intervalMs = ClampDouble(ReadDoubleBox(intervalField_.edit, settings_.intervalMs), 0.1, 60000.0);
    settings_.jitter = ClampInt(ReadIntBox(jitterField_.edit, settings_.jitter), 0, 50);
    settings_.burst = ClampInt(ReadIntBox(burstField_.edit, settings_.burst), 1, 8);
    settings_.limit = ClampU64(ReadU64Box(limitField_.edit, settings_.limit), 0, 1000000000ull);
    engine_.SetConfig(BuildConfig());
    InvalidateSection(timingCard_);
    InvalidateLiveCard();
}

bool App::RegisterHotkey() {
    UnregisterHotKey(hwnd_, 1);
    hotkeyFallback_ = false;
    UINT requested = settings_.hotkey;
    hotkeyOk_ = RegisterHotKey(hwnd_, 1, MOD_NOREPEAT, requested) != FALSE;
    if (!hotkeyOk_ && requested != VK_F6) {
        settings_.hotkey = VK_F6;
        hotkeyOk_ = RegisterHotKey(hwnd_, 1, MOD_NOREPEAT, settings_.hotkey) != FALSE;
        hotkeyFallback_ = hotkeyOk_;
    }
    return hotkeyOk_;
}

void App::BeginAnchorCapture() {
    if (anchorCapture_) {
        return;
    }
    if (engine_.Running()) {
        engine_.Stop();
    }
    hotkeyCapture_ = false;
    anchorCapture_ = true;
    anchorMouseDown_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    anchorEscapeDown_ = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    Banner(L"Move to the target and left-click once");
    audio_.PlayUi(settings_.sounds);
    ShowWindow(hwnd_, SW_MINIMIZE);
    InvalidateSection(modeCard_);
    InvalidateLiveCard();
}

void App::CompleteAnchorCapture() {
    if (!anchorCapture_) {
        return;
    }
    anchorCapture_ = false;
    anchorMouseDown_ = false;
    anchorEscapeDown_ = false;
    GetCursorPos(&settings_.anchor);
    settings_.hasAnchor = true;
    settings_.target = TargetMode::Anchor;
    engine_.SetConfig(BuildConfig());
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    audio_.PlayAnchor(settings_.sounds);
    Banner(L"Lock point saved at " + PointText(settings_.anchor));
    InvalidateSection(modeCard_);
    InvalidateLiveCard();
}

void App::CancelAnchorCapture() {
    if (!anchorCapture_) {
        return;
    }
    anchorCapture_ = false;
    anchorMouseDown_ = false;
    anchorEscapeDown_ = false;
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    audio_.PlayWarn(settings_.sounds);
    Banner(L"Lock point capture canceled");
    InvalidateSection(modeCard_);
    InvalidateLiveCard();
}

void App::ToggleEngine() {
    SyncSettingsFromControls();
    if (engine_.Running()) {
        engine_.Stop();
        lastRunning_ = false;
        liveRate_ = 0.0;
        lastLagSample_ = 0;
        lastCountSample_ = engine_.Count();
        lastSampleTick_ = GetTickCount64();
        audio_.PlayStop(settings_.sounds);
        Banner(L"Clicks paused");
    } else {
        if (settings_.target == TargetMode::Anchor && !settings_.hasAnchor) {
            audio_.PlayWarn(settings_.sounds);
            Banner(L"Save a lock point first");
            InvalidateLiveCard();
            return;
        }
        engine_.SetConfig(BuildConfig());
        lastSampleTick_ = GetTickCount64();
        lastCountSample_ = 0;
        liveRate_ = 0.0;
        lastLagSample_ = 0;
        engine_.Start();
        lastRunning_ = true;
        audio_.PlayStart(settings_.sounds);
        Banner(settings_.target == TargetMode::Anchor && settings_.hasAnchor ? L"Locked point mode is live" : L"Cursor mode is live");
    }
    UpdateUiTimer();
    InvalidateLiveCard();
}

void App::Banner(const std::wstring& text) {
    banner_ = text;
    bannerUntil_ = GetTickCount64() + 2400;
    UpdateUiTimer();
}

UINT App::DesiredUiTimerInterval() const {
    if (anchorCapture_) {
        return kUiTimerAnchorMs;
    }
    if (engine_.Running()) {
        return kUiTimerActiveMs;
    }
    if (!banner_.empty()) {
        return kUiTimerBannerMs;
    }
    return 0;
}

void App::UpdateUiTimer() {
    if (!hwnd_) {
        return;
    }

    UINT desired = DesiredUiTimerInterval();
    if (desired == uiTimerIntervalMs_) {
        return;
    }

    if (uiTimerIntervalMs_ != 0) {
        KillTimer(hwnd_, kUiTimerId);
        uiTimerIntervalMs_ = 0;
    }

    if (desired != 0) {
        SetTimer(hwnd_, kUiTimerId, desired, nullptr);
        uiTimerIntervalMs_ = desired;
    }
}

void App::MarkStaticCacheDirty() {
    staticCacheDirty_ = true;
}

void App::DestroyStaticCache() {
    if (staticCacheDc_) {
        if (staticCacheOldBmp_) {
            SelectObject(staticCacheDc_, staticCacheOldBmp_);
        }
        DeleteDC(staticCacheDc_);
        staticCacheDc_ = nullptr;
        staticCacheOldBmp_ = nullptr;
    }
    if (staticCacheBmp_) {
        DeleteObject(staticCacheBmp_);
        staticCacheBmp_ = nullptr;
    }
    staticCacheSize_ = SIZE{};
    staticCacheDirty_ = true;
}

void App::DestroyPaintBuffer() {
    if (paintBufferDc_) {
        if (paintBufferOldBmp_) {
            SelectObject(paintBufferDc_, paintBufferOldBmp_);
        }
        DeleteDC(paintBufferDc_);
        paintBufferDc_ = nullptr;
        paintBufferOldBmp_ = nullptr;
    }
    if (paintBufferBmp_) {
        DeleteObject(paintBufferBmp_);
        paintBufferBmp_ = nullptr;
    }
    paintBufferSize_ = SIZE{};
}

void App::EnsureStaticCache(HDC reference, const RECT& clientRc) {
    int width = clientRc.right - clientRc.left;
    int height = clientRc.bottom - clientRc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    if (!staticCacheDc_ || staticCacheSize_.cx != width || staticCacheSize_.cy != height) {
        DestroyStaticCache();
        if (!CreateMemorySurface(reference, width, height, staticCacheDc_, staticCacheBmp_, staticCacheOldBmp_, staticCacheSize_)) {
            return;
        }
        staticCacheDirty_ = true;
    }

    if (!staticCacheDirty_ || !staticCacheDc_) {
        return;
    }

    Graphics g(staticCacheDc_);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    DrawStaticChrome(g, clientRc, PaletteFor(settings_.theme));
    staticCacheDirty_ = false;
}

void App::EnsurePaintBuffer(HDC reference, const RECT& clientRc) {
    int width = clientRc.right - clientRc.left;
    int height = clientRc.bottom - clientRc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    if (paintBufferDc_ && paintBufferSize_.cx == width && paintBufferSize_.cy == height) {
        return;
    }

    DestroyPaintBuffer();
    CreateMemorySurface(reference, width, height, paintBufferDc_, paintBufferBmp_, paintBufferOldBmp_, paintBufferSize_);
}

RECT App::HitBounds(int hit) const {
    switch (hit) {
    case HitTheme: return themeRect_;
    case HitSound: return soundRect_;
    case HitSoft: return softRect_;
    case HitStart: return liveCard_;
    case HitAnchor: return anchorRect_;
    case HitHotkey: return hotkeyRect_;
    case HitLeft: return buttonRects_[0];
    case HitRight: return buttonRects_[1];
    case HitMiddle: return buttonRects_[2];
    case HitCursor: return targetRects_[0];
    case HitAnchorMode: return targetRects_[1];
    default: return RECT{};
    }
}

void App::InvalidateSection(const RECT& rc) {
    if (!hwnd_ || IsRectEmpty(&rc)) {
        return;
    }
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::InvalidateLiveCard() {
    InvalidateSection(liveCard_);
}

void App::OnTimer() {
    bool needsRepaint = false;
    ULONGLONG now = GetTickCount64();

    if (anchorCapture_) {
        bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (escapeDown && !anchorEscapeDown_) {
            CancelAnchorCapture();
            return;
        }
        anchorEscapeDown_ = escapeDown;
        if (anchorCapture_) {
            bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (mouseDown && !anchorMouseDown_) {
                CompleteAnchorCapture();
                return;
            }
            anchorMouseDown_ = mouseDown;
        }
    }

    bool running = engine_.Running();
    unsigned long long count = engine_.Count();
    double previousRate = liveRate_;
    unsigned long long previousCount = lastCountSample_;
    int lagUs = engine_.AvgLagUs();
    if (lastSampleTick_ != 0 && now > lastSampleTick_) {
        liveRate_ = static_cast<double>(count - lastCountSample_) * 1000.0 / static_cast<double>(now - lastSampleTick_);
    }
    lastCountSample_ = count;
    lastSampleTick_ = now;

    if (running) {
        needsRepaint = count != previousCount || std::fabs(liveRate_ - previousRate) >= 0.05 || lagUs != lastLagSample_;
    } else if (liveRate_ != 0.0) {
        liveRate_ = 0.0;
        needsRepaint = true;
    }
    lastLagSample_ = lagUs;

    if (running != lastRunning_) {
        if (!running && lastRunning_) {
            audio_.PlayStop(settings_.sounds);
        }
        lastRunning_ = running;
        if (!running && liveRate_ != 0.0) {
            liveRate_ = 0.0;
        }
        needsRepaint = true;
    }

    if (!banner_.empty() && now > bannerUntil_) {
        banner_.clear();
        needsRepaint = true;
    }

    UpdateUiTimer();
    if (needsRepaint) {
        InvalidateLiveCard();
    }
}

void App::OnMouseMove(int x, int y) {
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
    TrackMouseEvent(&tme);
    int hit = HitTest(POINT{ x, y });
    if (hit != hover_) {
        int previous = hover_;
        hover_ = hit;
        InvalidateSection(HitBounds(previous));
        InvalidateSection(HitBounds(hit));
    }
}

void App::OnClick(int x, int y) {
    int hit = HitTest(POINT{ x, y });
    switch (hit) {
    case HitTheme:
        settings_.theme = settings_.theme == Theme::Dark ? Theme::Light : Theme::Dark;
        ApplyTheme();
        audio_.PlayUi(settings_.sounds);
        Banner(settings_.theme == Theme::Dark ? L"Dark mode enabled" : L"Light mode enabled");
        break;
    case HitSound:
        if (settings_.sounds) {
            audio_.PlayDisabled(true);
            settings_.sounds = false;
            Banner(L"UI sounds off");
        } else {
            settings_.sounds = true;
            audio_.PlayEnabled(true);
            Banner(L"UI sounds on");
        }
        InvalidateSection(soundRect_);
        InvalidateLiveCard();
        break;
    case HitSoft:
        settings_.softStart = !settings_.softStart;
        engine_.SetConfig(BuildConfig());
        audio_.PlayUi(settings_.sounds);
        Banner(settings_.softStart ? L"Soft start enabled" : L"Soft start disabled");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    case HitStart:
        ToggleEngine();
        break;
    case HitAnchor:
        BeginAnchorCapture();
        break;
    case HitHotkey:
        hotkeyCapture_ = true;
        SetFocus(hwnd_);
        audio_.PlayUi(settings_.sounds);
        Banner(L"Press an F-key from F1 to F24");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    case HitLeft:
        settings_.button = MouseButton::Left;
        engine_.SetConfig(BuildConfig());
        audio_.PlayUi(settings_.sounds);
        Banner(L"Left mouse button selected");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    case HitRight:
        settings_.button = MouseButton::Right;
        engine_.SetConfig(BuildConfig());
        audio_.PlayUi(settings_.sounds);
        Banner(L"Right mouse button selected");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    case HitMiddle:
        settings_.button = MouseButton::Middle;
        engine_.SetConfig(BuildConfig());
        audio_.PlayUi(settings_.sounds);
        Banner(L"Middle mouse button selected");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    case HitCursor:
        settings_.target = TargetMode::Cursor;
        engine_.SetConfig(BuildConfig());
        audio_.PlayUi(settings_.sounds);
        Banner(L"Cursor mode ready");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    case HitAnchorMode:
        if (!settings_.hasAnchor) {
            BeginAnchorCapture();
            break;
        }
        settings_.target = TargetMode::Anchor;
        engine_.SetConfig(BuildConfig());
        audio_.PlayUi(settings_.sounds);
        Banner(L"Lock point mode ready");
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        break;
    }
}

void App::OnHotkeyCapture(UINT vk) {
    if (vk == VK_ESCAPE) {
        hotkeyCapture_ = false;
        Banner(L"Hotkey stayed the same");
        audio_.PlayUi(settings_.sounds);
        InvalidateSection(modeCard_);
        InvalidateLiveCard();
        return;
    }
    if (vk < VK_F1 || vk > VK_F24) {
        Banner(L"Use F1 through F24");
        audio_.PlayWarn(settings_.sounds);
        return;
    }
    settings_.hotkey = vk;
    hotkeyCapture_ = false;
    RegisterHotkey();
    if (hotkeyFallback_) {
        Banner(L"That hotkey is busy, using F6");
        audio_.PlayWarn(settings_.sounds);
    } else if (hotkeyOk_) {
        Banner(L"Hotkey set to " + HotkeyText(settings_.hotkey));
        audio_.PlayUi(settings_.sounds);
    } else {
        Banner(L"Could not grab that hotkey");
        audio_.PlayWarn(settings_.sounds);
    }
    InvalidateSection(modeCard_);
    InvalidateLiveCard();
}

int App::HitTest(POINT pt) const {
    if (Contains(themeRect_, pt)) return HitTheme;
    if (Contains(soundRect_, pt)) return HitSound;
    if (Contains(softRect_, pt)) return HitSoft;
    if (Contains(startRect_, pt)) return HitStart;
    if (Contains(anchorRect_, pt)) return HitAnchor;
    if (Contains(hotkeyRect_, pt)) return HitHotkey;
    if (Contains(buttonRects_[0], pt)) return HitLeft;
    if (Contains(buttonRects_[1], pt)) return HitRight;
    if (Contains(buttonRects_[2], pt)) return HitMiddle;
    if (Contains(targetRects_[0], pt)) return HitCursor;
    if (Contains(targetRects_[1], pt)) return HitAnchorMode;
    return HitNone;
}

void App::DrawTextBlock(Graphics& g, const std::wstring& text, const RectF& rc, const Font& font, COLORREF color, StringAlignment align, StringAlignment line, bool wrap, StringTrimming trimming) {
    SolidBrush brush(ToColor(color));
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(line);
    format.SetTrimming(trimming);
    INT flags = format.GetFormatFlags();
    flags = wrap ? (flags | StringFormatFlagsLineLimit)
                 : (flags | StringFormatFlagsNoWrap);
    format.SetFormatFlags(flags);
    g.DrawString(text.c_str(), -1, &font, rc, &format, &brush);
}

void App::DrawShadow(Graphics& g, const RECT& rc, int radius, COLORREF color) {
    for (int i = 0; i < 4; ++i) {
        RECT s = rc;
        OffsetRect(&s, 0, Scale(3 + i * 2));
        InflateRect(&s, i * 2, i * 2);
        GraphicsPath path(FillModeAlternate);
        Rounded(path, s, radius + i * 2);
        SolidBrush brush(ToColor(color, static_cast<BYTE>(28 - i * 6)));
        g.FillPath(&brush, &path);
    }
}

void App::DrawCard(Graphics& g, const RECT& rc, const Palette& p, bool accent) {
    DrawShadow(g, rc, Scale(18), accent ? Mix(p.accent, p.shadow, 0.65) : p.shadow);
    GraphicsPath path(FillModeAlternate);
    Rounded(path, rc, Scale(18));
    Color top = ToColor(accent ? Mix(p.panel2, p.accent, 0.16) : p.panel);
    Color bottom = ToColor(accent ? Mix(p.panel, p.accent2, 0.11) : p.panel2);
    LinearGradientBrush brush(Point(rc.left, rc.top), Point(rc.right, rc.bottom), top, bottom);
    g.FillPath(&brush, &path);
    Pen pen(ToColor(accent ? Mix(p.border, p.accent, 0.42) : p.border), 1.0f);
    g.DrawPath(&pen, &path);
}

void App::DrawPill(Graphics& g, const RECT& rc, const Palette& p, const std::wstring& label, bool selected, bool hovered) {
    COLORREF base = selected ? Mix(p.accent, p.accent2, 0.28) : Mix(p.surface, p.panel2, hovered ? 0.45 : 0.18);
    COLORREF border = selected ? Mix(p.accent2, p.text, 0.12) : Mix(p.border, p.text, hovered ? 0.18 : 0.05);
    GraphicsPath path(FillModeAlternate);
    Rounded(path, rc, (rc.bottom - rc.top) / 2);
    SolidBrush brush(ToColor(base));
    Pen pen(ToColor(border), 1.0f);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
    Font font(L"Comic Sans MS", static_cast<REAL>(Scale(13)), FontStyleBold, UnitPixel);
    DrawTextBlock(g, label, ToRectF(rc), font, selected ? RGB(255, 255, 255) : p.text, StringAlignmentCenter, StringAlignmentCenter);
}

void App::DrawField(Graphics& g, const FieldLayout& field, const Palette& p) {
    GraphicsPath path(FillModeAlternate);
    Rounded(path, field.box, Scale(16));
    LinearGradientBrush brush(Point(field.box.left, field.box.top), Point(field.box.right, field.box.bottom), ToColor(Mix(p.input, p.panel, 0.25)), ToColor(Mix(p.input, p.panel2, 0.38)));
    Pen pen(ToColor(Mix(p.border, p.accent, 0.12)), 1.0f);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
    Font labelFont(L"Comic Sans MS", static_cast<REAL>(Scale(11)), FontStyleRegular, UnitPixel);
    RECT labelRect = field.box;
    labelRect.left += Scale(14);
    labelRect.top += Scale(10);
    labelRect.right -= Scale(14);
    labelRect.bottom = labelRect.top + Scale(18);
    DrawTextBlock(g, field.label, ToRectF(labelRect), labelFont, p.muted);
}

void App::DrawStaticChrome(Graphics& g, const RECT& rc, const Palette& p) {
    LinearGradientBrush bg(Point(0, 0), Point(rc.right, rc.bottom), ToColor(p.bg0), ToColor(p.bg1));
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    SolidBrush orbA(ToColor(Mix(p.accent, RGB(255, 255, 255), 0.18), 34));
    SolidBrush orbB(ToColor(Mix(p.accent2, RGB(255, 255, 255), 0.24), 28));
    g.FillEllipse(&orbA, static_cast<REAL>(Scale(-40)), static_cast<REAL>(Scale(-90)), static_cast<REAL>(Scale(300)), static_cast<REAL>(Scale(260)));
    g.FillEllipse(&orbB, static_cast<REAL>(rc.right - Scale(260)), static_cast<REAL>(Scale(-70)), static_cast<REAL>(Scale(320)), static_cast<REAL>(Scale(220)));

    Font titleFont(L"Comic Sans MS", static_cast<REAL>(Scale(32)), FontStyleBold, UnitPixel);
    Font subFont(L"Comic Sans MS", static_cast<REAL>(Scale(16)), FontStyleBold, UnitPixel);
    Font sectionFont(L"Comic Sans MS", static_cast<REAL>(Scale(17)), FontStyleBold, UnitPixel);
    Font smallFont(L"Comic Sans MS", static_cast<REAL>(Scale(12)), FontStyleBold, UnitPixel);

    RECT titleRc{ Scale(24), Scale(26), Scale(420), Scale(76) };
    DrawTextBlock(g, L"Flow Autoclicker", ToRectF(titleRc), titleFont, p.text);
    RECT subRc{ Scale(26), Scale(76), Scale(560), Scale(106) };
    DrawTextBlock(g, L"best autoclicker", ToRectF(subRc), subFont, p.muted);

    DrawCard(g, timingCard_, p);
    DrawCard(g, modeCard_, p);
    DrawCard(g, liveCard_, p, true);

    RECT tHead{ timingCard_.left + Scale(20), timingCard_.top + Scale(22), timingCard_.right - Scale(20), timingCard_.top + Scale(46) };
    RECT tSub{ timingCard_.left + Scale(20), timingCard_.top + Scale(52), timingCard_.right - Scale(20), timingCard_.top + Scale(80) };
    DrawTextBlock(g, L"Speed", ToRectF(tHead), sectionFont, p.text);
    DrawTextBlock(g, L"set ur speed, jitter, and burst", ToRectF(tSub), smallFont, p.muted);
    DrawField(g, intervalField_, p);
    DrawField(g, jitterField_, p);
    DrawField(g, burstField_, p);
    DrawField(g, limitField_, p);

    RECT mHead{ modeCard_.left + Scale(20), modeCard_.top + Scale(22), modeCard_.right - Scale(20), modeCard_.top + Scale(46) };
    RECT mSub{ modeCard_.left + Scale(20), modeCard_.top + Scale(52), modeCard_.right - Scale(20), modeCard_.top + Scale(80) };
    DrawTextBlock(g, L"Controls", ToRectF(mHead), sectionFont, p.text);
    DrawTextBlock(g, L"choose hotkeys and control stuff", ToRectF(mSub), smallFont, p.muted);
    RECT bLabel{ modeCard_.left + Scale(20), modeCard_.top + Scale(88), modeCard_.right - Scale(20), modeCard_.top + Scale(108) };
    DrawTextBlock(g, L"Mouse button", ToRectF(bLabel), smallFont, p.muted);
    RECT targetLabel{ modeCard_.left + Scale(20), buttonRects_[0].bottom + Scale(24), modeCard_.right - Scale(20), buttonRects_[0].bottom + Scale(46) };
    DrawTextBlock(g, L"Click target", ToRectF(targetLabel), smallFont, p.muted);
}

void App::DrawTimingOverlay(Graphics& g, const Palette& p) {
    Font sectionFont(L"Comic Sans MS", static_cast<REAL>(Scale(17)), FontStyleBold, UnitPixel);
    Font smallFont(L"Comic Sans MS", static_cast<REAL>(Scale(12)), FontStyleBold, UnitPixel);

    double effectiveRate = (1000.0 / std::max(settings_.intervalMs, 0.1)) * static_cast<double>(std::max(settings_.burst, 1));
    RECT effLabel{ timingCard_.left + Scale(20), limitField_.box.bottom + Scale(26), timingCard_.right - Scale(20), limitField_.box.bottom + Scale(48) };
    RECT effValue{ timingCard_.left + Scale(20), limitField_.box.bottom + Scale(48), timingCard_.right - Scale(20), limitField_.box.bottom + Scale(90) };
    DrawTextBlock(g, L"Estimated speed", ToRectF(effLabel), smallFont, p.muted);
    DrawTextBlock(g, ToRate(effectiveRate), ToRectF(effValue), sectionFont, p.text);

    RECT tuneRc{ timingCard_.left + Scale(20), timingCard_.bottom - Scale(76), timingCard_.right - Scale(20), timingCard_.bottom - Scale(28) };
    DrawPill(g, tuneRc, p, settings_.jitter > 0 ? L"Jitter On" : L"Jitter Off", settings_.jitter > 0, false);
}

void App::DrawModeOverlay(Graphics& g, const Palette& p) {
    DrawPill(g, buttonRects_[0], p, L"Left", settings_.button == MouseButton::Left, hover_ == HitLeft);
    DrawPill(g, buttonRects_[1], p, L"Right", settings_.button == MouseButton::Right, hover_ == HitRight);
    DrawPill(g, buttonRects_[2], p, L"Middle", settings_.button == MouseButton::Middle, hover_ == HitMiddle);

    DrawPill(g, targetRects_[0], p, L"Follow cursor", settings_.target == TargetMode::Cursor, hover_ == HitCursor);
    DrawPill(g, targetRects_[1], p, L"Use lock point", settings_.target == TargetMode::Anchor, hover_ == HitAnchorMode);

    std::wstring anchorLabel = anchorCapture_
        ? L"Capturing... left-click anywhere to save"
        : (settings_.hasAnchor ? (L"Move Lock Point  " + PointText(settings_.anchor)) : L"Set Lock Point");
    DrawPill(g, anchorRect_, p, anchorLabel, true, hover_ == HitAnchor);
    DrawPill(g, softRect_, p, settings_.softStart ? L"Soft Start On" : L"Soft Start Off", settings_.softStart, hover_ == HitSoft);

    std::wstring hotkeyLabel = hotkeyCapture_ ? L"Press F1 to F24"
        : (hotkeyOk_ ? (L"Hotkey  " + HotkeyText(settings_.hotkey)) : L"Hotkey Unavailable");
    DrawPill(g, hotkeyRect_, p, hotkeyLabel, hotkeyOk_, hover_ == HitHotkey);
}

void App::DrawLiveOverlay(Graphics& g, const Palette& p) {
    Font sectionFont(L"Comic Sans MS", static_cast<REAL>(Scale(17)), FontStyleBold, UnitPixel);
    Font smallFont(L"Comic Sans MS", static_cast<REAL>(Scale(12)), FontStyleBold, UnitPixel);
    Font statFont(L"Comic Sans MS", static_cast<REAL>(Scale(42)), FontStyleBold, UnitPixel);
    Font heroFont(L"Comic Sans MS", static_cast<REAL>(Scale(14)), FontStyleBold, UnitPixel);

    RECT lHead{ liveCard_.left + Scale(24), liveCard_.top + Scale(22), liveCard_.right - Scale(24), liveCard_.top + Scale(56) };
    DrawTextBlock(g, engine_.Running() ? L"Clicking" : L"Idle", ToRectF(lHead), sectionFont, engine_.Running() ? p.good : p.text);
    RECT lSub{ liveCard_.left + Scale(24), liveCard_.top + Scale(58), liveCard_.right - Scale(24), liveCard_.top + Scale(84) };
    DrawTextBlock(g, L"glorified stopwatch lol", ToRectF(lSub), smallFont, p.muted);

    RECT countRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(104), liveCard_.right - Scale(24), liveCard_.top + Scale(176) };
    DrawTextBlock(g, ToWideU64(engine_.Count()), ToRectF(countRc), statFont, p.text);
    RECT countLabel{ liveCard_.left + Scale(26), countRc.bottom - Scale(6), liveCard_.right - Scale(24), countRc.bottom + Scale(20) };
    DrawTextBlock(g, L"Clicks sent", ToRectF(countLabel), smallFont, p.muted);

    RECT liveRateRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(212), liveCard_.right - Scale(24), liveCard_.top + Scale(244) };
    DrawTextBlock(g, ToRate(liveRate_), ToRectF(liveRateRc), sectionFont, engine_.Running() ? p.accent2 : p.text);
    RECT lagRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(244), liveCard_.right - Scale(24), liveCard_.top + Scale(266) };
    DrawTextBlock(g, L"Timing drift  " + ToWide(engine_.Running() ? engine_.AvgLagUs() : 0) + L" us", ToRectF(lagRc), smallFont, p.muted);

    RECT notesRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(286), liveCard_.right - Scale(24), liveCard_.top + Scale(324) };
    std::wstring targetText = settings_.target == TargetMode::Anchor
        ? (settings_.hasAnchor ? (L"lock point at " + PointText(settings_.anchor)) : L"lock point not set")
        : L"cursor";
    DrawTextBlock(g, L"Target: " + targetText, ToRectF(notesRc), heroFont, p.muted, StringAlignmentNear, StringAlignmentNear, false, StringTrimmingEllipsisWord);

    DrawPill(g, startRect_, p, engine_.Running() ? L"Stop Engine" : L"Start Engine", true, hover_ == HitStart);

    if (!banner_.empty()) {
        RECT bannerRc{ liveCard_.left + Scale(24), startRect_.top - Scale(60), liveCard_.right - Scale(24), startRect_.top - Scale(16) };
        DrawPill(g, bannerRc, p, banner_, true, false);
    }
}

void App::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    EnsureStaticCache(hdc, rc);
    EnsurePaintBuffer(hdc, rc);

    int paintW = ps.rcPaint.right - ps.rcPaint.left;
    int paintH = ps.rcPaint.bottom - ps.rcPaint.top;
    HDC targetDc = (paintBufferDc_ && staticCacheDc_) ? paintBufferDc_ : hdc;

    if (staticCacheDc_) {
        BitBlt(targetDc, ps.rcPaint.left, ps.rcPaint.top, paintW, paintH, staticCacheDc_, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }

    Graphics g(targetDc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetClip(Rect(ps.rcPaint.left, ps.rcPaint.top, paintW, paintH));
    Palette p = PaletteFor(settings_.theme);
    if (Intersects(ps.rcPaint, themeRect_)) {
        DrawPill(g, themeRect_, p, settings_.theme == Theme::Dark ? L"Dark Mode" : L"Light Mode", true, hover_ == HitTheme);
    }
    if (Intersects(ps.rcPaint, soundRect_)) {
        DrawPill(g, soundRect_, p, settings_.sounds ? L"Sounds On" : L"Sounds Off", settings_.sounds, hover_ == HitSound);
    }
    if (Intersects(ps.rcPaint, timingCard_)) {
        DrawTimingOverlay(g, p);
    }
    if (Intersects(ps.rcPaint, modeCard_)) {
        DrawModeOverlay(g, p);
    }
    if (Intersects(ps.rcPaint, liveCard_)) {
        DrawLiveOverlay(g, p);
    }
    if (targetDc != hdc) {
        BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, paintW, paintH, targetDc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken{};
    GdiplusStartup(&gdipToken, &gdipInput, nullptr);
    App app(instance);
    int code = app.Run(show);
    GdiplusShutdown(gdipToken);
    return code;
}
