//
// DebugLog.h
// AES67 macOS Driver - Build #7
// Debug logging utilities
//

#pragma once

#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace AES67 {
namespace Debug {

// Log file location
inline const char* GetLogPath() {
    return "/tmp/aes67driver_debug.log";
}

// Off in processes that must not write the shared driver log: the root clock
// service polls the configuration every second (most of the former log volume)
// and must not write into world-writable /tmp with root rights.
inline std::atomic<bool>& Enabled() {
    static std::atomic<bool> enabled{true};
    return enabled;
}
inline void SetEnabled(bool enabled) { Enabled().store(enabled, std::memory_order_relaxed); }

// Keep one previous generation. Only the file's owner rotates, so the CoreAudio
// helper never ends up unable to write a file another process recreated.
inline void RotateIfNeeded() {
    constexpr off_t kMaxLogBytes = 5 * 1024 * 1024;
    struct stat info {};
    if (lstat(GetLogPath(), &info) != 0 || !S_ISREG(info.st_mode)) return;
    if (info.st_size < kMaxLogBytes || info.st_uid != geteuid()) return;
    rename(GetLogPath(), "/tmp/aes67driver_debug.log.1");
}

// O_NOFOLLOW: /tmp is world-writable, never write through a planted symlink.
// Only service accounts (the CoreAudio helper runs as _coreaudiod) create the
// file: a test or command-line tool run by a login user (uid >= 500) would
// otherwise own it and lock the driver out of its own log until reboot.
inline int OpenLog(int extraFlags) {
    const int create = geteuid() < 500 ? O_CREAT : 0;
    return open(GetLogPath(), O_WRONLY | O_NOFOLLOW | create | extraFlags, 0644);
}

// Write timestamped log message
inline void Log(const char* message) {
    if (!Enabled().load(std::memory_order_relaxed)) return;
    RotateIfNeeded();

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    char timestamp[64];
    time_t nowtime = tv.tv_sec;
    struct tm local {};
    localtime_r(&nowtime, &local);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);

    char line[640];
    const int length = snprintf(line, sizeof(line), "[%s.%06d] %s\n", timestamp, (int)tv.tv_usec, message);
    // AES67_LOG_STDERR=1 mirrors the log for tests and command-line tools,
    // which normally may not write the shared file.
    static const bool mirror = getenv("AES67_LOG_STDERR") != nullptr;
    if (mirror) fputs(line, stderr);
    const int fd = OpenLog(O_APPEND);
    if (fd < 0) return;
    if (length > 0) {
        (void)!write(fd, line, length < (int)sizeof(line) ? (size_t)length : sizeof(line) - 1);
    }
    close(fd);
}

// Log with formatted string
inline void LogF(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

// Errors: log file plus stderr. Inside the CoreAudio host stderr is not
// visible, so an error that only goes to stderr is lost there.
inline void ErrorF(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    fprintf(stderr, "%s\n", buffer);
    Log(buffer);
}

// Clear log file
inline void ClearLog() {
    if (!Enabled().load(std::memory_order_relaxed)) return;
    const int fd = OpenLog(O_TRUNC);
    if (fd < 0) return;
    const char header[] = "=== AES67 Driver Debug Log ===\n";
    (void)!write(fd, header, sizeof(header) - 1);
    close(fd);
}

} // namespace Debug
} // namespace AES67

// Convenience macros
#define AES67_LOG(msg) AES67::Debug::Log(msg)
#define AES67_LOGF(fmt, ...) AES67::Debug::LogF(fmt, __VA_ARGS__)
#define AES67_ERRF(...) AES67::Debug::ErrorF(__VA_ARGS__)
