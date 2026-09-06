#pragma once

namespace ProcessDiagnostics {
void WriteMemorySnapshot(HANDLE file, const char* label);
void WriteAddressDetails(HANDLE file, const char* label, const void* address);
} // namespace ProcessDiagnostics
