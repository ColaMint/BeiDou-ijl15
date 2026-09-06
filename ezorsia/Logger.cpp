#include "stdafx.h"
#include "Logger.h"

#include <stdarg.h>

namespace {
constexpr char kLogFileName[] = "ijl15.log";

void GetLogPath(char* path, size_t pathCount) {
    const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathCount));
    if (length == 0 || length >= pathCount) {
        StringCchCopyA(path, pathCount, kLogFileName);
        return;
    }

    char* slash = strrchr(path, '\\');
    if (slash) {
        StringCchCopyA(slash + 1, pathCount - (slash + 1 - path), kLogFileName);
    } else {
        StringCchCopyA(path, pathCount, kLogFileName);
    }
}

void WriteVFormat(HANDLE file, const char* format, va_list args) {
    char buffer[2048]{};
    const HRESULT result = StringCchVPrintfA(buffer, ARRAYSIZE(buffer), format, args);
    if (SUCCEEDED(result) || result == STRSAFE_E_INSUFFICIENT_BUFFER) {
        Logger::WriteText(file, buffer);
    }
}
} // namespace

namespace Logger {
HANDLE Open() {
    char path[MAX_PATH]{};
    GetLogPath(path, ARRAYSIZE(path));
    return CreateFileA(
        path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
}

void Reset() {
    char path[MAX_PATH]{};
    GetLogPath(path, ARRAYSIZE(path));
    HANDLE file = CreateFileA(
        path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

void WriteText(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE || !text) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}

void WriteFormat(HANDLE file, const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteVFormat(file, format, args);
    va_end(args);
}

void WriteFormat(const char* format, ...) {
    HANDLE file = Open();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    va_list args;
    va_start(args, format);
    WriteVFormat(file, format, args);
    va_end(args);
    FlushAndClose(file);
}

void FlushAndClose(HANDLE file) {
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
}
} // namespace Logger
