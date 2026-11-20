#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace paws {

enum class DebugViewMode : int {
    Normal = 0,
    DepthBuffer = 1,
    MotionVectors = 2,
    Count = 3
};

inline const char* DebugViewName(DebugViewMode mode) {
    switch (mode) {
        case DebugViewMode::Normal: return "Normal";
        case DebugViewMode::DepthBuffer: return "Depth Buffer";
        case DebugViewMode::MotionVectors: return "Motion Vectors";
        default: return "Unknown";
    }
}

class Logger {
public:
    static Logger& Instance() {
        static Logger inst;
        return inst;
    }

    void Init(const wchar_t* filePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
        _wfopen_s(&fp_, filePath, L"w");
        if (fp_) {
            std::fprintf(fp_, "=== paws_gtav.asi log started ===\n");
            std::fflush(fp_);
        }
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fp_) {
            std::fprintf(fp_, "=== paws_gtav.asi log closed ===\n");
            std::fflush(fp_);
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    void Log(const char* fmt, ...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fp_) return;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(fp_, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(fp_, fmt, args);
        va_end(args);
        std::fprintf(fp_, "\n");
        std::fflush(fp_);
    }

private:
    std::mutex mutex_;
    FILE* fp_ = nullptr;
};

#define PAWS_LOG(...) ::paws::Logger::Instance().Log(__VA_ARGS__)

} // namespace paws
