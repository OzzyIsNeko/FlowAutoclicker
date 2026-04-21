#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <avrt.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <uxtheme.h>

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

namespace FlowCore {

inline int ClampInt(int v, int lo, int hi) {
    return std::min(std::max(v, lo), hi);
}

inline double ClampDouble(double v, double lo, double hi) {
    return std::min(std::max(v, lo), hi);
}

inline unsigned long long ClampU64(unsigned long long v, unsigned long long lo, unsigned long long hi) {
    return std::min(std::max(v, lo), hi);
}

inline std::wstring ToWide(int v) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%d", v);
    return buffer;
}

inline std::wstring ToWideDouble(double v, int decimals = 3) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.*f", decimals, v);
    std::wstring text = buffer;
    if (const auto dot = text.find(L'.'); dot != std::wstring::npos) {
        while (!text.empty() && text.back() == L'0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == L'.') {
            text.pop_back();
        }
    }
    return text;
}

inline std::wstring ToWideU64(unsigned long long v) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%llu", v);
    return buffer;
}

inline void FormatU64(wchar_t* buffer, size_t size, unsigned long long v) {
    swprintf_s(buffer, size, L"%llu", v);
}

inline void FormatPoint(wchar_t* buffer, size_t size, POINT pt) {
    swprintf_s(buffer, size, L"%d, %d", pt.x, pt.y);
}

inline void FormatRate(wchar_t* buffer, size_t size, double v) {
    if (v >= 100.0) {
        swprintf_s(buffer, size, L"%.0f cps", v);
    } else {
        swprintf_s(buffer, size, L"%.1f cps", v);
    }
}

inline void FormatHotkey(wchar_t* buffer, size_t size, UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(buffer, size, L"F%u", vk - VK_F1 + 1);
        return;
    }
    wcscpy_s(buffer, size, L"F6");
}

inline std::wstring PointText(POINT pt) {
    return ToWide(pt.x) + L", " + ToWide(pt.y);
}

inline std::wstring HotkeyText(UINT vk) {
    wchar_t buffer[16]{};
    FormatHotkey(buffer, std::size(buffer), vk);
    return buffer;
}

inline COLORREF Mix(COLORREF a, COLORREF b, double t) {
    const int blend = std::clamp(static_cast<int>(std::lround(t * 256.0)), 0, 256);
    const auto mixChannel = [blend](BYTE x, BYTE y) -> BYTE {
        return static_cast<BYTE>((((256 - blend) * x) + (blend * y) + 128) >> 8);
    };
    return RGB(mixChannel(GetRValue(a), GetRValue(b)),
               mixChannel(GetGValue(a), GetGValue(b)),
               mixChannel(GetBValue(a), GetBValue(b)));
}

struct VirtualScreenMetrics {
    int x{};
    int y{};
    int width{};
    int height{};
};

inline VirtualScreenMetrics CaptureVirtualScreenMetrics() {
    VirtualScreenMetrics screen;
    screen.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screen.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screen.width = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
    screen.height = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
    return screen;
}

inline LONG NormalizeAbsoluteX(const VirtualScreenMetrics& screen, int x) {
    return static_cast<LONG>(MulDiv(x - screen.x, 65535, std::max(screen.width - 1, 1)));
}

inline LONG NormalizeAbsoluteY(const VirtualScreenMetrics& screen, int y) {
    return static_cast<LONG>(MulDiv(y - screen.y, 65535, std::max(screen.height - 1, 1)));
}

inline double SoftStartSpeed(double elapsedMs) {
    if (elapsedMs >= 550.0) {
        return 1.0;
    }
    const double t = elapsedMs / 550.0;
    const double smooth = t * t * (3.0 - 2.0 * t);
    return 0.35 + 0.65 * smooth;
}

inline double AdvanceWeylPhase(double phase) {
    phase += 0.61803398875;
    if (phase >= 1.0) {
        phase -= 1.0;
    }
    return phase;
}

inline double WeylNoise(double phase) {
    double phase2 = phase + 0.38196601125;
    if (phase2 >= 1.0) {
        phase2 -= 1.0;
    }
    return phase + phase2 - 1.0;
}

inline int64_t IntervalTicks(double intervalMs, double speed, double ticksPerMs) {
    return std::max<int64_t>(1, static_cast<int64_t>(std::llround((intervalMs / speed) * ticksPerMs)));
}

inline int64_t JitterTicks(int64_t interval, int jitterPercent, double noise) {
    return static_cast<int64_t>(std::llround(static_cast<double>(interval) * (static_cast<double>(jitterPercent) / 100.0) * noise * 0.45));
}

} // namespace FlowCore

using FlowCore::ClampDouble;
using FlowCore::ClampInt;
using FlowCore::ClampU64;
using FlowCore::FormatHotkey;
using FlowCore::FormatPoint;
using FlowCore::FormatRate;
using FlowCore::FormatU64;
using FlowCore::HotkeyText;
using FlowCore::Mix;
using FlowCore::PointText;
using FlowCore::ToWide;
using FlowCore::ToWideDouble;
using FlowCore::ToWideU64;

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

static int64_t QpcNow() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

static void ConfigureProcessForTiming() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
}

static void ConfigureThreadForTiming() {
    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
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
        sessionDone_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (wake_ && stop_ && sessionDone_) {
            thread_ = std::thread([this] { Loop(); });
        }
    }

    ~ClickEngine() {
        exiting_.store(true, std::memory_order_relaxed);
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
        if (sessionDone_) {
            CloseHandle(sessionDone_);
        }
        if (timerResolutionActive_) {
            timeEndPeriod(1);
            timerResolutionActive_ = false;
        }
    }

    void SetConfig(const Config& cfg) {
        std::scoped_lock lock(mutex_);
        config_ = cfg;
        configDirty_.store(true, std::memory_order_release);
    }

    void Start() {
        if (!wake_ || !stop_) {
            return;
        }
        if (!timerResolutionActive_) {
            timerResolutionActive_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        }
        count_.store(0, std::memory_order_relaxed);
        avgLagUs_.store(0, std::memory_order_relaxed);
        ResetEvent(stop_);
        running_.store(true, std::memory_order_relaxed);
        SetEvent(wake_);
    }

    void Stop() {
        running_.store(false, std::memory_order_relaxed);
        if (stop_) {
            SetEvent(stop_);
        }
        if (sessionDone_) {
            WaitForSingleObject(sessionDone_, INFINITE);
        }
        if (timerResolutionActive_) {
            timeEndPeriod(1);
            timerResolutionActive_ = false;
        }
    }

    bool Running() const {
        return running_.load(std::memory_order_relaxed);
    }

    unsigned long long Count() const {
        return count_.load(std::memory_order_relaxed);
    }

    bool HighRes() const {
        return highRes_.load(std::memory_order_relaxed);
    }

    int AvgLagUs() const {
        return avgLagUs_.load(std::memory_order_relaxed);
    }

private:
    using VirtualScreen = FlowCore::VirtualScreenMetrics;
    static constexpr double kPreciseIntervalMs = 1.0;
    static constexpr int kMaxClicksPerSend = 32;

    HANDLE wake_{};
    HANDLE stop_{};
    HANDLE sessionDone_{};
    std::thread thread_;
    std::mutex mutex_;
    Config config_{};
    std::atomic<bool> configDirty_{ true };
    std::atomic<bool> running_{ false };
    std::atomic<bool> exiting_{ false };
    std::atomic<bool> highRes_{ false };
    std::atomic<unsigned long long> count_{ 0 };
    std::atomic<int> avgLagUs_{ 0 };
    bool timerResolutionActive_{ false };

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

    static bool WaitSpinUntil(const std::atomic<bool>& running, const std::atomic<bool>& exiting, int64_t target) {
        while (QpcNow() < target) {
            if (!running.load(std::memory_order_relaxed) || exiting.load(std::memory_order_relaxed)) {
                return false;
            }
            YieldProcessor();
        }
        return running.load(std::memory_order_relaxed) && !exiting.load(std::memory_order_relaxed);
    }

    static Config Sanitize(Config cfg) {
        cfg.intervalMs = ClampDouble(cfg.intervalMs, 0.1, 60000.0);
        cfg.jitter = ClampInt(cfg.jitter, 0, 50);
        cfg.burst = ClampInt(cfg.burst, 1, 8);
        if (cfg.intervalMs <= kPreciseIntervalMs) {
            cfg.jitter = 0;
            cfg.softStart = false;
        }
        return cfg;
    }

    static void Emit(const Config& cfg, const VirtualScreen& screen, int clickCount) {
        if (clickCount <= 0) {
            return;
        }
        auto sendInputs = [](INPUT* inputs, int count) {
            int sent = 0;
            while (sent < count) {
                UINT batch = SendInput(count - sent, inputs + sent, sizeof(INPUT));
                if (batch == 0) {
                    break;
                }
                sent += static_cast<int>(batch);
            }
        };
        auto addMove = [](std::array<INPUT, 66>& inputs, int& n, POINT pt, const VirtualScreen& screen) {
            INPUT i{};
            i.type = INPUT_MOUSE;
            i.mi.dx = FlowCore::NormalizeAbsoluteX(screen, pt.x);
            i.mi.dy = FlowCore::NormalizeAbsoluteY(screen, pt.y);
            i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            inputs[n++] = i;
        };
        auto sendClicks = [](std::array<INPUT, 66>& inputs, int& n, DWORD down, DWORD up, int clicks) {
            for (int i = 0; i < clicks; ++i) {
                INPUT d{};
                d.type = INPUT_MOUSE;
                d.mi.dwFlags = down;
                inputs[n++] = d;
                INPUT u{};
                u.type = INPUT_MOUSE;
                u.mi.dwFlags = up;
                inputs[n++] = u;
            }
        };
        DWORD down = DownFlag(cfg.button);
        DWORD up = UpFlag(cfg.button);
        if (cfg.target == TargetMode::Anchor && cfg.hasAnchor) {
            POINT current{};
            GetCursorPos(&current);
            bool movedToAnchor = false;
            int remaining = clickCount;
            while (remaining > 0) {
                std::array<INPUT, 66> inputs{};
                int n = 0;
                if (!movedToAnchor) {
                    addMove(inputs, n, cfg.anchor, screen);
                    movedToAnchor = true;
                }
                int clicksThisSend = std::min(remaining, kMaxClicksPerSend);
                sendClicks(inputs, n, down, up, clicksThisSend);
                remaining -= clicksThisSend;
                if (remaining == 0) {
                    addMove(inputs, n, current, screen);
                }
                if (n > 0) {
                    sendInputs(inputs.data(), n);
                }
            }
        } else {
            int remaining = clickCount;
            while (remaining > 0) {
                std::array<INPUT, 66> inputs{};
                int n = 0;
                int clicksThisSend = std::min(remaining, kMaxClicksPerSend);
                sendClicks(inputs, n, down, up, clicksThisSend);
                remaining -= clicksThisSend;
                if (n > 0) {
                    sendInputs(inputs.data(), n);
                }
            }
        }
    }

    static bool EmitPrecisionClick(const Config& cfg, const VirtualScreen& screen, const std::atomic<bool>& running, const std::atomic<bool>& exiting, int64_t dwellTarget) {
        INPUT down{};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = DownFlag(cfg.button);
        SendInput(1, &down, sizeof(INPUT));

        if (!WaitSpinUntil(running, exiting, dwellTarget)) {
            INPUT up{};
            up.type = INPUT_MOUSE;
            up.mi.dwFlags = UpFlag(cfg.button);
            SendInput(1, &up, sizeof(INPUT));
            return false;
        }

        INPUT up{};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = UpFlag(cfg.button);
        SendInput(1, &up, sizeof(INPUT));

        return running.load(std::memory_order_relaxed) && !exiting.load(std::memory_order_relaxed);
    }

    static bool WaitUntil(HANDLE timer, HANDLE stop, const std::atomic<bool>& running, const std::atomic<bool>& exiting, int64_t target, int64_t coarse, int64_t spin, int64_t freq, bool aggressiveSpin) {
        for (;;) {
            int64_t now = QpcNow();
            int64_t remain = target - now;
            if (remain <= 0) {
                return running.load(std::memory_order_relaxed) && !exiting.load(std::memory_order_relaxed);
            }
            if (aggressiveSpin) {
                while (QpcNow() < target) {
                    if (!running.load(std::memory_order_relaxed) || exiting.load(std::memory_order_relaxed)) {
                        return false;
                    }
                    YieldProcessor();
                }
                return running.load(std::memory_order_relaxed) && !exiting.load(std::memory_order_relaxed);
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
                if (!running.load(std::memory_order_relaxed) || exiting.load(std::memory_order_relaxed)) {
                    return false;
                }
                SwitchToThread();
            }
            while (QpcNow() < target) {
                YieldProcessor();
                if (!running.load(std::memory_order_relaxed) || exiting.load(std::memory_order_relaxed)) {
                    return false;
                }
            }
            return running.load(std::memory_order_relaxed) && !exiting.load(std::memory_order_relaxed);
        }
    }

    Config Snapshot() {
        std::scoped_lock lock(mutex_);
        return config_;
    }

    void Loop() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        HANDLE mmcssHandle = nullptr;
        DWORD mmcssTaskIndex = 0;
        if (timer) {
            highRes_.store(true, std::memory_order_relaxed);
        } else {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            highRes_.store(false, std::memory_order_relaxed);
        }
        ConfigureThreadForTiming();
        mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTaskIndex);
        if (!mmcssHandle) {
            mmcssTaskIndex = 0;
            mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
        }
        if (mmcssHandle) {
            AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        LARGE_INTEGER fq{};
        QueryPerformanceFrequency(&fq);
        const double ticksPerMs = static_cast<double>(fq.QuadPart) / 1000.0;
        const double msPerTick = 1000.0 / static_cast<double>(fq.QuadPart);
        const double usPerTick = 1000000.0 / static_cast<double>(fq.QuadPart);
        while (!exiting_.load(std::memory_order_relaxed)) {
            if (WaitForSingleObject(wake_, INFINITE) != WAIT_OBJECT_0) {
                break;
            }
            if (exiting_.load(std::memory_order_relaxed)) {
                break;
            }
            ResetEvent(wake_);
            if (!running_.load(std::memory_order_relaxed)) {
                continue;
            }
            ResetEvent(sessionDone_);
            count_.store(0, std::memory_order_relaxed);
            avgLagUs_.store(0, std::memory_order_relaxed);
            configDirty_.exchange(false, std::memory_order_acq_rel);
            Config cfg = Sanitize(Snapshot());
            VirtualScreen screen = FlowCore::CaptureVirtualScreenMetrics();
            int64_t sessionStart = QpcNow();
            int64_t next = sessionStart;
            double phase = 0.1732050807;
            while (running_.load(std::memory_order_relaxed) && !exiting_.load(std::memory_order_relaxed)) {
                if (configDirty_.exchange(false, std::memory_order_acq_rel)) {
                    cfg = Sanitize(Snapshot());
                }
                double speed = 1.0;
                if (cfg.softStart) {
                    double elapsedMs = static_cast<double>(QpcNow() - sessionStart) * msPerTick;
                    speed = FlowCore::SoftStartSpeed(elapsedMs);
                }
                int64_t interval = FlowCore::IntervalTicks(cfg.intervalMs, speed, ticksPerMs);
                bool precisionMode = cfg.intervalMs <= kPreciseIntervalMs &&
                    cfg.jitter == 0 &&
                    !cfg.softStart &&
                    cfg.burst == 1 &&
                    cfg.target == TargetMode::Cursor;
                phase = FlowCore::AdvanceWeylPhase(phase);
                double noise = FlowCore::WeylNoise(phase);
                int64_t jitter = FlowCore::JitterTicks(interval, cfg.jitter, noise);
                next += std::max<int64_t>(1, interval + jitter);
                if (!WaitUntil(timer, stop_, running_, exiting_, next, fq.QuadPart / 600, fq.QuadPart / 5000, fq.QuadPart, precisionMode)) {
                    break;
                }
                int64_t now = QpcNow();
                int lag = static_cast<int>(std::llround(std::abs(static_cast<double>(now - next)) * usPerTick));
                int averageLag = avgLagUs_.load(std::memory_order_relaxed);
                avgLagUs_.store((averageLag * 7 + lag) / 8, std::memory_order_relaxed);
                if (!running_.load(std::memory_order_relaxed) || exiting_.load(std::memory_order_relaxed)) {
                    break;
                }
                unsigned long long done = count_.load(std::memory_order_relaxed);
                unsigned long long remaining = cfg.limit == 0 ? ~0ull : (done < cfg.limit ? cfg.limit - done : 0ull);
                if (remaining == 0) {
                    running_.store(false, std::memory_order_relaxed);
                    break;
                }
                if (precisionMode) {
                    int64_t dwellTicks = std::max<int64_t>(1, interval / 4);
                    if (!EmitPrecisionClick(cfg, screen, running_, exiting_, now + dwellTicks)) {
                        break;
                    }
                    count_.fetch_add(1, std::memory_order_relaxed);
                    if (QpcNow() > next + interval) {
                        next = QpcNow();
                    }
                } else {
                    unsigned long long dueBursts = 1;
                    if (interval > 0 && now > next) {
                        dueBursts += static_cast<unsigned long long>((now - next) / interval);
                        next += static_cast<int64_t>(dueBursts - 1) * interval;
                    }
                    unsigned long long wantedClicks = static_cast<unsigned long long>(cfg.burst) * dueBursts;
                    int burst = static_cast<int>(std::min<unsigned long long>(wantedClicks, remaining));
                    Emit(cfg, screen, burst);
                    count_.fetch_add(static_cast<unsigned long long>(burst), std::memory_order_relaxed);
                }
                if (cfg.limit != 0 && count_.load(std::memory_order_relaxed) >= cfg.limit) {
                    running_.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            ResetEvent(stop_);
            running_.store(false, std::memory_order_relaxed);
            SetEvent(sessionDone_);
        }
        if (mmcssHandle) {
            AvRevertMmThreadCharacteristics(mmcssHandle);
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
static constexpr UINT kUiTimerAnchorMs = 50;
static constexpr UINT kUiTimerActiveMs = 100;
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
    Font* titleFont_{};
    Font* subtitleFont_{};
    Font* sectionFont_{};
    Font* smallFont_{};
    Font* pillFont_{};
    Font* fieldLabelFont_{};
    Font* statFont_{};
    Font* heroFont_{};
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
    bool mouseTracking_{ false };
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
    void DestroyFonts();
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
    RECT LiveStatsRect() const;
    RECT LiveBannerRect() const;
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
    void DrawTextBlock(Graphics& g, const wchar_t* text, const RectF& rc, const Font& font, COLORREF color, StringAlignment align = StringAlignmentNear, StringAlignment line = StringAlignmentNear, bool wrap = false, StringTrimming trimming = StringTrimmingEllipsisCharacter);
    void DrawTextBlock(Graphics& g, const std::wstring& text, const RectF& rc, const Font& font, COLORREF color, StringAlignment align = StringAlignmentNear, StringAlignment line = StringAlignmentNear, bool wrap = false, StringTrimming trimming = StringTrimmingEllipsisCharacter);
    void DrawShadow(Graphics& g, const RECT& rc, int radius, COLORREF color);
    void DrawCard(Graphics& g, const RECT& rc, const Palette& p, bool accent = false);
    void DrawPill(Graphics& g, const RECT& rc, const Palette& p, const wchar_t* label, bool selected, bool hovered);
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
        mouseTracking_ = false;
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
    ConfigureProcessForTiming();
    LoadSettings();
    CreateControls();
    RebuildFont();
    SyncControlsFromSettings();
    Layout();
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
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
    DestroyFonts();
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
    DestroyFonts();
    titleFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(32)), FontStyleBold, UnitPixel);
    subtitleFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(16)), FontStyleBold, UnitPixel);
    sectionFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(17)), FontStyleBold, UnitPixel);
    smallFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(12)), FontStyleBold, UnitPixel);
    pillFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(13)), FontStyleBold, UnitPixel);
    fieldLabelFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(11)), FontStyleRegular, UnitPixel);
    statFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(42)), FontStyleBold, UnitPixel);
    heroFont_ = new Font(L"Comic Sans MS", static_cast<REAL>(Scale(14)), FontStyleBold, UnitPixel);
    for (HWND edit : { intervalField_.edit, jitterField_.edit, burstField_.edit, limitField_.edit }) {
        if (edit) {
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
        }
    }
}

void App::DestroyFonts() {
    delete titleFont_;
    titleFont_ = nullptr;
    delete subtitleFont_;
    subtitleFont_ = nullptr;
    delete sectionFont_;
    sectionFont_ = nullptr;
    delete smallFont_;
    smallFont_ = nullptr;
    delete pillFont_;
    pillFont_ = nullptr;
    delete fieldLabelFont_;
    fieldLabelFont_ = nullptr;
    delete statFont_;
    statFont_ = nullptr;
    delete heroFont_;
    heroFont_ = nullptr;
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
    UpdateUiTimer();
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

RECT App::LiveStatsRect() const {
    RECT rc{
        liveCard_.left + Scale(24),
        liveCard_.top + Scale(22),
        liveCard_.right - Scale(24),
        liveCard_.top + Scale(324)
    };
    InflateRect(&rc, Scale(4), Scale(4));
    return rc;
}

RECT App::LiveBannerRect() const {
    RECT rc{
        liveCard_.left + Scale(24),
        startRect_.top - Scale(60),
        liveCard_.right - Scale(24),
        startRect_.top - Scale(16)
    };
    InflateRect(&rc, Scale(6), Scale(6));
    return rc;
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
    bool repaintLiveCard = false;
    bool repaintLiveStats = false;
    bool repaintBanner = false;
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
    int previousLag = lastLagSample_;
    int lagUs = engine_.AvgLagUs();
    if (lastSampleTick_ != 0 && now > lastSampleTick_) {
        liveRate_ = static_cast<double>(count - lastCountSample_) * 1000.0 / static_cast<double>(now - lastSampleTick_);
    }
    lastCountSample_ = count;
    lastSampleTick_ = now;

    if (running) {
        repaintLiveStats = count != previousCount || std::fabs(liveRate_ - previousRate) >= 0.05 || lagUs != previousLag;
    } else if (liveRate_ != 0.0) {
        liveRate_ = 0.0;
        repaintLiveStats = true;
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
        repaintLiveCard = true;
    }

    if (!banner_.empty() && now > bannerUntil_) {
        banner_.clear();
        repaintBanner = true;
    }

    UpdateUiTimer();
    if (repaintLiveCard) {
        InvalidateLiveCard();
        return;
    }
    if (repaintBanner) {
        InvalidateLiveCard();
        return;
    }
    if (repaintLiveStats) {
        InvalidateSection(LiveStatsRect());
    }
}

void App::OnMouseMove(int x, int y) {
    if (!mouseTracking_) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        mouseTracking_ = true;
    }
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

void App::DrawTextBlock(Graphics& g, const wchar_t* text, const RectF& rc, const Font& font, COLORREF color, StringAlignment align, StringAlignment line, bool wrap, StringTrimming trimming) {
    SolidBrush brush(ToColor(color));
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(line);
    format.SetTrimming(trimming);
    INT flags = format.GetFormatFlags();
    flags = wrap ? (flags | StringFormatFlagsLineLimit)
                 : (flags | StringFormatFlagsNoWrap);
    format.SetFormatFlags(flags);
    g.DrawString(text, -1, &font, rc, &format, &brush);
}

void App::DrawTextBlock(Graphics& g, const std::wstring& text, const RectF& rc, const Font& font, COLORREF color, StringAlignment align, StringAlignment line, bool wrap, StringTrimming trimming) {
    DrawTextBlock(g, text.c_str(), rc, font, color, align, line, wrap, trimming);
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

void App::DrawPill(Graphics& g, const RECT& rc, const Palette& p, const wchar_t* label, bool selected, bool hovered) {
    COLORREF base = selected ? Mix(p.accent, p.accent2, 0.28) : Mix(p.surface, p.panel2, hovered ? 0.45 : 0.18);
    COLORREF border = selected ? Mix(p.accent2, p.text, 0.12) : Mix(p.border, p.text, hovered ? 0.18 : 0.05);
    GraphicsPath path(FillModeAlternate);
    Rounded(path, rc, (rc.bottom - rc.top) / 2);
    SolidBrush brush(ToColor(base));
    Pen pen(ToColor(border), 1.0f);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
    DrawTextBlock(g, label, ToRectF(rc), *pillFont_, selected ? RGB(255, 255, 255) : p.text, StringAlignmentCenter, StringAlignmentCenter);
}

void App::DrawPill(Graphics& g, const RECT& rc, const Palette& p, const std::wstring& label, bool selected, bool hovered) {
    DrawPill(g, rc, p, label.c_str(), selected, hovered);
}

void App::DrawField(Graphics& g, const FieldLayout& field, const Palette& p) {
    GraphicsPath path(FillModeAlternate);
    Rounded(path, field.box, Scale(16));
    LinearGradientBrush brush(Point(field.box.left, field.box.top), Point(field.box.right, field.box.bottom), ToColor(Mix(p.input, p.panel, 0.25)), ToColor(Mix(p.input, p.panel2, 0.38)));
    Pen pen(ToColor(Mix(p.border, p.accent, 0.12)), 1.0f);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
    RECT labelRect = field.box;
    labelRect.left += Scale(14);
    labelRect.top += Scale(10);
    labelRect.right -= Scale(14);
    labelRect.bottom = labelRect.top + Scale(18);
    DrawTextBlock(g, field.label, ToRectF(labelRect), *fieldLabelFont_, p.muted);
}

void App::DrawStaticChrome(Graphics& g, const RECT& rc, const Palette& p) {
    LinearGradientBrush bg(Point(0, 0), Point(rc.right, rc.bottom), ToColor(p.bg0), ToColor(p.bg1));
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    SolidBrush orbA(ToColor(Mix(p.accent, RGB(255, 255, 255), 0.18), 34));
    SolidBrush orbB(ToColor(Mix(p.accent2, RGB(255, 255, 255), 0.24), 28));
    g.FillEllipse(&orbA, static_cast<REAL>(Scale(-40)), static_cast<REAL>(Scale(-90)), static_cast<REAL>(Scale(300)), static_cast<REAL>(Scale(260)));
    g.FillEllipse(&orbB, static_cast<REAL>(rc.right - Scale(260)), static_cast<REAL>(Scale(-70)), static_cast<REAL>(Scale(320)), static_cast<REAL>(Scale(220)));

    RECT titleRc{ Scale(24), Scale(26), Scale(420), Scale(76) };
    DrawTextBlock(g, L"Flow Autoclicker", ToRectF(titleRc), *titleFont_, p.text);
    RECT subRc{ Scale(26), Scale(76), Scale(560), Scale(106) };
    DrawTextBlock(g, L"best autoclicker", ToRectF(subRc), *subtitleFont_, p.muted);

    DrawCard(g, timingCard_, p);
    DrawCard(g, modeCard_, p);
    DrawCard(g, liveCard_, p, true);

    RECT tHead{ timingCard_.left + Scale(20), timingCard_.top + Scale(22), timingCard_.right - Scale(20), timingCard_.top + Scale(46) };
    RECT tSub{ timingCard_.left + Scale(20), timingCard_.top + Scale(52), timingCard_.right - Scale(20), timingCard_.top + Scale(80) };
    DrawTextBlock(g, L"Speed", ToRectF(tHead), *sectionFont_, p.text);
    DrawTextBlock(g, L"set ur speed, jitter, and burst", ToRectF(tSub), *smallFont_, p.muted);
    DrawField(g, intervalField_, p);
    DrawField(g, jitterField_, p);
    DrawField(g, burstField_, p);
    DrawField(g, limitField_, p);

    RECT mHead{ modeCard_.left + Scale(20), modeCard_.top + Scale(22), modeCard_.right - Scale(20), modeCard_.top + Scale(46) };
    RECT mSub{ modeCard_.left + Scale(20), modeCard_.top + Scale(52), modeCard_.right - Scale(20), modeCard_.top + Scale(80) };
    DrawTextBlock(g, L"Controls", ToRectF(mHead), *sectionFont_, p.text);
    DrawTextBlock(g, L"choose hotkeys and control stuff", ToRectF(mSub), *smallFont_, p.muted);
    RECT bLabel{ modeCard_.left + Scale(20), modeCard_.top + Scale(88), modeCard_.right - Scale(20), modeCard_.top + Scale(108) };
    DrawTextBlock(g, L"Mouse button", ToRectF(bLabel), *smallFont_, p.muted);
    RECT targetLabel{ modeCard_.left + Scale(20), buttonRects_[0].bottom + Scale(24), modeCard_.right - Scale(20), buttonRects_[0].bottom + Scale(46) };
    DrawTextBlock(g, L"Click target", ToRectF(targetLabel), *smallFont_, p.muted);
}

void App::DrawTimingOverlay(Graphics& g, const Palette& p) {
    double effectiveRate = (1000.0 / std::max(settings_.intervalMs, 0.1)) * static_cast<double>(std::max(settings_.burst, 1));
    wchar_t rateText[32]{};
    FormatRate(rateText, std::size(rateText), effectiveRate);
    RECT effLabel{ timingCard_.left + Scale(20), limitField_.box.bottom + Scale(26), timingCard_.right - Scale(20), limitField_.box.bottom + Scale(48) };
    RECT effValue{ timingCard_.left + Scale(20), limitField_.box.bottom + Scale(48), timingCard_.right - Scale(20), limitField_.box.bottom + Scale(90) };
    DrawTextBlock(g, L"Estimated speed", ToRectF(effLabel), *smallFont_, p.muted);
    DrawTextBlock(g, rateText, ToRectF(effValue), *sectionFont_, p.text);

    RECT tuneRc{ timingCard_.left + Scale(20), timingCard_.bottom - Scale(76), timingCard_.right - Scale(20), timingCard_.bottom - Scale(28) };
    DrawPill(g, tuneRc, p, settings_.jitter > 0 ? L"Jitter On" : L"Jitter Off", settings_.jitter > 0, false);
}

void App::DrawModeOverlay(Graphics& g, const Palette& p) {
    DrawPill(g, buttonRects_[0], p, L"Left", settings_.button == MouseButton::Left, hover_ == HitLeft);
    DrawPill(g, buttonRects_[1], p, L"Right", settings_.button == MouseButton::Right, hover_ == HitRight);
    DrawPill(g, buttonRects_[2], p, L"Middle", settings_.button == MouseButton::Middle, hover_ == HitMiddle);

    DrawPill(g, targetRects_[0], p, L"Follow cursor", settings_.target == TargetMode::Cursor, hover_ == HitCursor);
    DrawPill(g, targetRects_[1], p, L"Use lock point", settings_.target == TargetMode::Anchor, hover_ == HitAnchorMode);

    wchar_t anchorLabel[96]{};
    if (anchorCapture_) {
        wcscpy_s(anchorLabel, std::size(anchorLabel), L"Capturing... left-click anywhere to save");
    } else if (settings_.hasAnchor) {
        wchar_t pointText[48]{};
        FormatPoint(pointText, std::size(pointText), settings_.anchor);
        swprintf_s(anchorLabel, std::size(anchorLabel), L"Move Lock Point  %ls", pointText);
    } else {
        wcscpy_s(anchorLabel, std::size(anchorLabel), L"Set Lock Point");
    }
    DrawPill(g, anchorRect_, p, anchorLabel, true, hover_ == HitAnchor);
    DrawPill(g, softRect_, p, settings_.softStart ? L"Soft Start On" : L"Soft Start Off", settings_.softStart, hover_ == HitSoft);

    wchar_t hotkeyLabel[48]{};
    if (hotkeyCapture_) {
        wcscpy_s(hotkeyLabel, std::size(hotkeyLabel), L"Press F1 to F24");
    } else if (hotkeyOk_) {
        wchar_t hotkeyText[16]{};
        FormatHotkey(hotkeyText, std::size(hotkeyText), settings_.hotkey);
        swprintf_s(hotkeyLabel, std::size(hotkeyLabel), L"Hotkey  %ls", hotkeyText);
    } else {
        wcscpy_s(hotkeyLabel, std::size(hotkeyLabel), L"Hotkey Unavailable");
    }
    DrawPill(g, hotkeyRect_, p, hotkeyLabel, hotkeyOk_, hover_ == HitHotkey);
}

void App::DrawLiveOverlay(Graphics& g, const Palette& p) {
    bool running = engine_.Running();
    wchar_t countText[64]{};
    wchar_t rateText[32]{};
    wchar_t lagText[48]{};
    wchar_t targetText[96]{};
    FormatU64(countText, std::size(countText), engine_.Count());
    FormatRate(rateText, std::size(rateText), liveRate_);
    swprintf_s(lagText, std::size(lagText), L"Timing drift  %d us", running ? engine_.AvgLagUs() : 0);
    if (settings_.target == TargetMode::Anchor) {
        if (settings_.hasAnchor) {
            wchar_t pointText[48]{};
            FormatPoint(pointText, std::size(pointText), settings_.anchor);
            swprintf_s(targetText, std::size(targetText), L"Target: lock point at %ls", pointText);
        } else {
            wcscpy_s(targetText, std::size(targetText), L"Target: lock point not set");
        }
    } else {
        wcscpy_s(targetText, std::size(targetText), L"Target: cursor");
    }
    RECT lHead{ liveCard_.left + Scale(24), liveCard_.top + Scale(22), liveCard_.right - Scale(24), liveCard_.top + Scale(56) };
    DrawTextBlock(g, running ? L"Clicking" : L"Idle", ToRectF(lHead), *sectionFont_, running ? p.good : p.text);
    RECT lSub{ liveCard_.left + Scale(24), liveCard_.top + Scale(58), liveCard_.right - Scale(24), liveCard_.top + Scale(84) };
    DrawTextBlock(g, L"glorified stopwatch lol", ToRectF(lSub), *smallFont_, p.muted);

    RECT countRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(104), liveCard_.right - Scale(24), liveCard_.top + Scale(176) };
    DrawTextBlock(g, countText, ToRectF(countRc), *statFont_, p.text);
    RECT countLabel{ liveCard_.left + Scale(26), countRc.bottom - Scale(6), liveCard_.right - Scale(24), countRc.bottom + Scale(20) };
    DrawTextBlock(g, L"Clicks sent", ToRectF(countLabel), *smallFont_, p.muted);

    RECT liveRateRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(212), liveCard_.right - Scale(24), liveCard_.top + Scale(244) };
    DrawTextBlock(g, rateText, ToRectF(liveRateRc), *sectionFont_, running ? p.accent2 : p.text);
    RECT lagRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(244), liveCard_.right - Scale(24), liveCard_.top + Scale(266) };
    DrawTextBlock(g, lagText, ToRectF(lagRc), *smallFont_, p.muted);

    RECT notesRc{ liveCard_.left + Scale(24), liveCard_.top + Scale(286), liveCard_.right - Scale(24), liveCard_.top + Scale(324) };
    DrawTextBlock(g, targetText, ToRectF(notesRc), *heroFont_, p.muted, StringAlignmentNear, StringAlignmentNear, false, StringTrimmingEllipsisWord);

    DrawPill(g, startRect_, p, running ? L"Stop Engine" : L"Start Engine", true, hover_ == HitStart);

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

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show) {
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
