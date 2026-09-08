#pragma once

bool HookNameSpaceStreaming(bool enable, DWORD readAheadKiB, DWORD smallFileCacheMiB);
void LogNameSpaceStreamingStats(const char* phase);
