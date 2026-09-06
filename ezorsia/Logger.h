#pragma once

namespace Logger {
HANDLE Open();
void Reset();
void WriteText(HANDLE file, const char* text);
void WriteFormat(HANDLE file, const char* format, ...);
void WriteFormat(const char* format, ...);
void FlushAndClose(HANDLE file);
} // namespace Logger
