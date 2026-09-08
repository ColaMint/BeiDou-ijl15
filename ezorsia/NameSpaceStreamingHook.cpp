#include "stdafx.h"
#include "NameSpaceStreamingHook.h"
#include "Logger.h"

#include <intrin.h>
#include <cstring>
#include <deque>

#pragma intrinsic(_ReturnAddress)

namespace {
using MapViewOfFileFn = decltype(&MapViewOfFile);
using ReadFileFn = decltype(&ReadFile);
using CloseHandleFn = decltype(&CloseHandle);
using SetFilePointerFn = decltype(&SetFilePointer);
using CLoginInitFn = void(__thiscall*)(void*, void*);

constexpr DWORD kCLoginInitAddress = 0x005F42CD;

struct ReadAheadEntry {
    HANDLE file = INVALID_HANDLE_VALUE;
    char name[MAX_PATH]{};
    unsigned __int64 fileSize = 0;
    unsigned __int64 offset = 0;
    DWORD size = 0;
    bool reachesEof = false;
    bool positionKnown = false;
    bool virtualPosition = false;
    bool wholeFileCached = false;
    unsigned __int64 logicalPosition = 0;
    unsigned __int64 logicalReadCalls = 0;
    unsigned __int64 logicalReadBytes = 0;
    unsigned __int64 cacheHitCalls = 0;
    unsigned __int64 backendReadCalls = 0;
    unsigned __int64 backendReadBytes = 0;
    unsigned __int64 virtualPositionQueries = 0;
    unsigned __int64 virtualSeeks = 0;
    unsigned __int64 physicalSeeks = 0;
    std::vector<BYTE> data;
};

MapViewOfFileFn g_mapViewOfFile = MapViewOfFile;
ReadFileFn g_readFile = ReadFile;
CloseHandleFn g_closeHandle = CloseHandle;
SetFilePointerFn g_setFilePointer = SetFilePointer;
CLoginInitFn g_loginInit = reinterpret_cast<CLoginInitFn>(kCLoginInitAddress);
bool g_hookInstalled = false;
volatile LONG g_fallbackCount = 0;
SRWLOCK g_readAheadLock = SRWLOCK_INIT;
std::deque<ReadAheadEntry> g_readAheadEntries;
ReadAheadEntry* g_lastReadAheadEntry = nullptr;
DWORD g_readAheadBytes = 32 * 1024;
DWORD g_smallFileCacheMaxBytes = 16 * 1024 * 1024;
unsigned __int64 g_logicalReadCalls = 0;
unsigned __int64 g_logicalReadBytes = 0;
unsigned __int64 g_cacheHitCalls = 0;
unsigned __int64 g_backendReadCalls = 0;
unsigned __int64 g_backendReadBytes = 0;
unsigned __int64 g_virtualPositionQueries = 0;
unsigned __int64 g_virtualSeeks = 0;
unsigned __int64 g_physicalSeeks = 0;
DWORD g_startTick = 0;
std::atomic<uintptr_t> g_nameSpaceBase{0};
std::atomic<uintptr_t> g_nameSpaceEnd{0};

bool CacheNameSpaceModuleRange() {
    if (g_nameSpaceBase.load(std::memory_order_acquire) != 0) {
        return true;
    }
    const HMODULE module = GetModuleHandleA("NameSpace.dll");
    if (!module) {
        return false;
    }

    DWORD imageSize = 0;
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + dosHeader->e_lfanew);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
            ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }
        imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    g_nameSpaceEnd.store(base + imageSize, std::memory_order_relaxed);
    g_nameSpaceBase.store(base, std::memory_order_release);
    return true;
}

bool IsNameSpaceCaller(const void* caller, DWORD* callerOffset) {
    if (!caller || !CacheNameSpaceModuleRange()) {
        return false;
    }

    const uintptr_t base = g_nameSpaceBase.load(std::memory_order_acquire);
    const uintptr_t end = g_nameSpaceEnd.load(std::memory_order_relaxed);
    const uintptr_t address = reinterpret_cast<uintptr_t>(caller);
    if (address < base || address >= end) {
        return false;
    }

    if (callerOffset) {
        *callerOffset = static_cast<DWORD>(address - base);
    }
    return true;
}

bool GetFilePosition(HANDLE file, unsigned __int64* position) {
    LARGE_INTEGER distance{};
    LARGE_INTEGER current{};
    if (!SetFilePointerEx(file, distance, &current, FILE_CURRENT)) {
        return false;
    }
    *position = static_cast<unsigned __int64>(current.QuadPart);
    return true;
}

bool SetFilePosition(HANDLE file, unsigned __int64 position) {
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(position);
    return SetFilePointerEx(file, target, nullptr, FILE_BEGIN) != FALSE;
}

ReadAheadEntry* FindEntry(HANDLE file) {
    if (g_lastReadAheadEntry && g_lastReadAheadEntry->file == file) {
        return g_lastReadAheadEntry;
    }
    for (auto& entry : g_readAheadEntries) {
        if (entry.file == file) {
            g_lastReadAheadEntry = &entry;
            return &entry;
        }
    }
    for (auto& entry : g_readAheadEntries) {
        if (entry.file == INVALID_HANDLE_VALUE) {
            entry = ReadAheadEntry{};
            entry.file = file;
            g_lastReadAheadEntry = &entry;
            return &entry;
        }
    }
    g_readAheadEntries.emplace_back();
    g_readAheadEntries.back().file = file;
    g_lastReadAheadEntry = &g_readAheadEntries.back();
    return g_lastReadAheadEntry;
}

void CaptureFileName(ReadAheadEntry* entry) {
    if (entry->name[0]) {
        return;
    }
    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(entry->file, &fileSize) && fileSize.QuadPart > 0) {
        entry->fileSize = static_cast<unsigned __int64>(fileSize.QuadPart);
    }
    char path[MAX_PATH]{};
    const DWORD length = GetFinalPathNameByHandleA(
        entry->file, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED);
    if (length == 0 || length >= ARRAYSIZE(path)) {
        StringCchPrintfA(
            entry->name, ARRAYSIZE(entry->name), "handle-0x%08lX",
            static_cast<DWORD>(reinterpret_cast<uintptr_t>(entry->file)));
        return;
    }
    const char* slash = strrchr(path, '\\');
    StringCchCopyA(
        entry->name, ARRAYSIZE(entry->name), slash ? slash + 1 : path);
}

BOOL WINAPI ReadFileHook(
        HANDLE file, LPVOID buffer, DWORD bytesToRead, LPDWORD bytesRead,
        LPOVERLAPPED overlapped) {
    void* caller = _ReturnAddress();
    if (!buffer || !bytesRead || bytesToRead == 0 || overlapped ||
        !IsNameSpaceCaller(caller, nullptr)) {
        return g_readFile(file, buffer, bytesToRead, bytesRead, overlapped);
    }

    AcquireSRWLockExclusive(&g_readAheadLock);
    ReadAheadEntry* entry = nullptr;
    try {
        entry = FindEntry(file);
    }
    catch (...) {
        ReleaseSRWLockExclusive(&g_readAheadLock);
        return g_readFile(file, buffer, bytesToRead, bytesRead, overlapped);
    }
    if (!entry->positionKnown) {
        if (!GetFilePosition(file, &entry->logicalPosition)) {
            ReleaseSRWLockExclusive(&g_readAheadLock);
            return g_readFile(file, buffer, bytesToRead, bytesRead, overlapped);
        }
        entry->positionKnown = true;
    }
    entry->virtualPosition = true;
    CaptureFileName(entry);

    ++g_logicalReadCalls;
    g_logicalReadBytes += bytesToRead;
    ++entry->logicalReadCalls;
    entry->logicalReadBytes += bytesToRead;
    const unsigned __int64 offset = entry->logicalPosition;

    const unsigned __int64 entryEnd = entry->offset + entry->size;
    if (offset >= entry->offset && offset <= entryEnd) {
        const unsigned __int64 available64 = entryEnd - offset;
        if (available64 >= bytesToRead || entry->reachesEof) {
            const DWORD available = static_cast<DWORD>(
                (std::min<unsigned __int64>)(available64, bytesToRead));
            if (available != 0) {
                std::memcpy(
                    buffer, entry->data.data() + static_cast<size_t>(offset - entry->offset),
                    available);
            }
            *bytesRead = available;
            entry->logicalPosition = offset + available;
            ++g_cacheHitCalls;
            ++entry->cacheHitCalls;
            ReleaseSRWLockExclusive(&g_readAheadLock);
            return TRUE;
        }
    }

    if (g_readAheadBytes == 0 || bytesToRead > g_readAheadBytes) {
        if (!SetFilePosition(file, offset)) {
            const DWORD error = GetLastError();
            ReleaseSRWLockExclusive(&g_readAheadLock);
            SetLastError(error);
            return FALSE;
        }
        ++g_physicalSeeks;
        ++entry->physicalSeeks;
        ++g_backendReadCalls;
        ++entry->backendReadCalls;
        const BOOL result = g_readFile(file, buffer, bytesToRead, bytesRead, nullptr);
        if (result) {
            entry->logicalPosition += *bytesRead;
            g_backendReadBytes += *bytesRead;
            entry->backendReadBytes += *bytesRead;
        }
        const DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&g_readAheadLock);
        SetLastError(error);
        return result;
    }

    const bool cacheWholeFile =
        g_smallFileCacheMaxBytes != 0 && entry->fileSize != 0 &&
        entry->fileSize <= g_smallFileCacheMaxBytes &&
        entry->backendReadBytes >= entry->fileSize * 2;
    const unsigned __int64 readOffset = cacheWholeFile ? 0 : offset;
    const DWORD backendBytesToRead = cacheWholeFile
        ? static_cast<DWORD>(entry->fileSize) : g_readAheadBytes;
    entry->offset = readOffset;
    entry->size = 0;
    entry->reachesEof = false;
    entry->wholeFileCached = false;
    try {
        entry->data.resize(backendBytesToRead);
    }
    catch (...) {
        entry->data.clear();
        if (!SetFilePosition(file, offset)) {
            const DWORD error = GetLastError();
            ReleaseSRWLockExclusive(&g_readAheadLock);
            SetLastError(error);
            return FALSE;
        }
        ++g_physicalSeeks;
        ++entry->physicalSeeks;
        ++g_backendReadCalls;
        ++entry->backendReadCalls;
        const BOOL result = g_readFile(
            file, buffer, bytesToRead, bytesRead, overlapped);
        if (result) {
            entry->logicalPosition += *bytesRead;
            g_backendReadBytes += *bytesRead;
            entry->backendReadBytes += *bytesRead;
        }
        const DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&g_readAheadLock);
        SetLastError(error);
        return result;
    }
    DWORD backendBytesRead = 0;
    if (!SetFilePosition(file, readOffset)) {
        const DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&g_readAheadLock);
        SetLastError(error);
        return FALSE;
    }
    ++g_physicalSeeks;
    ++entry->physicalSeeks;
    ++g_backendReadCalls;
    ++entry->backendReadCalls;
    const BOOL result = g_readFile(
        file, entry->data.data(), backendBytesToRead, &backendBytesRead, nullptr);
    if (!result) {
        const DWORD error = GetLastError();
        entry->data.clear();
        ReleaseSRWLockExclusive(&g_readAheadLock);
        SetLastError(error);
        return FALSE;
    }

    entry->size = backendBytesRead;
    entry->reachesEof = backendBytesRead < backendBytesToRead ||
        (entry->fileSize != 0 && readOffset + backendBytesRead >= entry->fileSize);
    entry->wholeFileCached = cacheWholeFile &&
        backendBytesRead == backendBytesToRead;
    g_backendReadBytes += backendBytesRead;
    entry->backendReadBytes += backendBytesRead;
    const unsigned __int64 dataOffset = offset - readOffset;
    const DWORD available = dataOffset < backendBytesRead
        ? backendBytesRead - static_cast<DWORD>(dataOffset) : 0;
    const DWORD returned = (std::min)(bytesToRead, available);
    if (returned != 0) {
        std::memcpy(
            buffer, entry->data.data() + static_cast<size_t>(dataOffset), returned);
    }
    *bytesRead = returned;
    entry->logicalPosition = offset + returned;
    ReleaseSRWLockExclusive(&g_readAheadLock);
    return TRUE;
}

DWORD WINAPI SetFilePointerHook(
        HANDLE file, LONG distanceLow, PLONG distanceHigh, DWORD moveMethod) {
    void* caller = _ReturnAddress();
    if (!IsNameSpaceCaller(caller, nullptr)) {
        return g_setFilePointer(file, distanceLow, distanceHigh, moveMethod);
    }

    AcquireSRWLockExclusive(&g_readAheadLock);
    ReadAheadEntry* entry = nullptr;
    try {
        entry = FindEntry(file);
    }
    catch (...) {
        ReleaseSRWLockExclusive(&g_readAheadLock);
        return g_setFilePointer(file, distanceLow, distanceHigh, moveMethod);
    }
    if (!entry->virtualPosition) {
        SetLastError(ERROR_SUCCESS);
        const DWORD result = g_setFilePointer(
            file, distanceLow, distanceHigh, moveMethod);
        const DWORD error = GetLastError();
        if (result != INVALID_SET_FILE_POINTER || error == ERROR_SUCCESS) {
            const unsigned __int64 high = distanceHigh
                ? static_cast<unsigned __int64>(static_cast<DWORD>(*distanceHigh)) << 32
                : 0;
            entry->logicalPosition = high | result;
            entry->positionKnown = true;
        }
        ReleaseSRWLockExclusive(&g_readAheadLock);
        SetLastError(error);
        return result;
    }

    // NameSpace.dll performs this query before virtually every fallback read.
    if (moveMethod == FILE_CURRENT && distanceLow == 0 && !distanceHigh) {
        const DWORD result = static_cast<DWORD>(entry->logicalPosition);
        if (result == INVALID_SET_FILE_POINTER) {
            SetLastError(ERROR_SUCCESS);
        }
        ++g_virtualPositionQueries;
        ++entry->virtualPositionQueries;
        ReleaseSRWLockExclusive(&g_readAheadLock);
        return result;
    }

    LARGE_INTEGER distance{};
    distance.LowPart = static_cast<DWORD>(distanceLow);
    distance.HighPart = distanceHigh ? *distanceHigh : (distanceLow < 0 ? -1 : 0);

    LONGLONG base = 0;
    if (moveMethod == FILE_CURRENT) {
        base = static_cast<LONGLONG>(entry->logicalPosition);
    } else if (moveMethod == FILE_END) {
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file, &fileSize)) {
            const DWORD error = GetLastError();
            ReleaseSRWLockExclusive(&g_readAheadLock);
            SetLastError(error);
            return INVALID_SET_FILE_POINTER;
        }
        base = fileSize.QuadPart;
    } else if (moveMethod != FILE_BEGIN) {
        ReleaseSRWLockExclusive(&g_readAheadLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }

    unsigned __int64 target = 0;
    if (distance.QuadPart < 0) {
        const unsigned __int64 magnitude =
            static_cast<unsigned __int64>(-(distance.QuadPart + 1)) + 1;
        if (magnitude > static_cast<unsigned __int64>(base)) {
            ReleaseSRWLockExclusive(&g_readAheadLock);
            SetLastError(ERROR_NEGATIVE_SEEK);
            return INVALID_SET_FILE_POINTER;
        }
        target = static_cast<unsigned __int64>(base) - magnitude;
    } else {
        const unsigned __int64 positiveDistance =
            static_cast<unsigned __int64>(distance.QuadPart);
        if (positiveDistance >
            static_cast<unsigned __int64>(MAXLONGLONG - base)) {
            ReleaseSRWLockExclusive(&g_readAheadLock);
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_SET_FILE_POINTER;
        }
        target = static_cast<unsigned __int64>(base) + positiveDistance;
    }
    entry->logicalPosition = target;
    if (distanceHigh) {
        *distanceHigh = static_cast<LONG>(entry->logicalPosition >> 32);
    }
    const DWORD result = static_cast<DWORD>(entry->logicalPosition);
    ++g_virtualSeeks;
    ++entry->virtualSeeks;
    if (result == INVALID_SET_FILE_POINTER) {
        SetLastError(ERROR_SUCCESS);
    }
    ReleaseSRWLockExclusive(&g_readAheadLock);
    return result;
}

BOOL WINAPI CloseHandleHook(HANDLE object) {
    void* caller = _ReturnAddress();
    if (IsNameSpaceCaller(caller, nullptr)) {
        AcquireSRWLockExclusive(&g_readAheadLock);
        for (auto& entry : g_readAheadEntries) {
            if (entry.file == object) {
                if (g_lastReadAheadEntry == &entry) {
                    g_lastReadAheadEntry = nullptr;
                }
                entry = ReadAheadEntry{};
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_readAheadLock);
    }
    return g_closeHandle(object);
}

void __fastcall CLoginInitHook(void* self, void* edx, void* param) {
    UNREFERENCED_PARAMETER(edx);
    g_loginInit(self, param);
    LogNameSpaceStreamingStats("CLogin::Init");
}

LPVOID WINAPI MapViewOfFileHook(
    HANDLE fileMappingObject,
    DWORD desiredAccess,
    DWORD fileOffsetHigh,
    DWORD fileOffsetLow,
    SIZE_T numberOfBytesToMap) {
    void* caller = _ReturnAddress();
    DWORD callerOffset = 0;
    if (numberOfBytesToMap == 0 && IsNameSpaceCaller(caller, &callerOffset)) {
        const LONG count = InterlockedIncrement(&g_fallbackCount);
        Logger::WriteFormat(
            "[NameSpaceStreaming] fallback #%ld caller=NameSpace.dll+0x%08lX "
            "mapping=0x%08lX offset=0x%08lX%08lX\r\n",
            count, callerOffset,
            static_cast<DWORD>(reinterpret_cast<uintptr_t>(fileMappingObject)),
            fileOffsetHigh, fileOffsetLow);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    return g_mapViewOfFile(
        fileMappingObject, desiredAccess, fileOffsetHigh, fileOffsetLow,
        numberOfBytesToMap);
}
} // namespace

bool HookNameSpaceStreaming(
        bool enable, DWORD readAheadKiB, DWORD smallFileCacheMiB) {
#if defined(_M_IX86)
    if (enable) {
        const DWORD clampedKiB = (std::min<DWORD>)(4096, readAheadKiB);
        const DWORD clampedCacheMiB =
            (std::min<DWORD>)(128, smallFileCacheMiB);
        g_readAheadBytes = clampedKiB * 1024;
        g_smallFileCacheMaxBytes = clampedCacheMiB * 1024 * 1024;
        if (!g_hookInstalled) {
            CacheNameSpaceModuleRange();
            g_startTick = GetTickCount();
            const bool readHook = Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_readFile),
                reinterpret_cast<void*>(ReadFileHook));
            const bool pointerHook = readHook && Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_setFilePointer),
                reinterpret_cast<void*>(SetFilePointerHook));
            const bool closeHook = pointerHook && Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_closeHandle),
                reinterpret_cast<void*>(CloseHandleHook));
            const bool loginHook = closeHook && Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_loginInit),
                reinterpret_cast<void*>(CLoginInitHook));
            const bool mapHook = loginHook && Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_mapViewOfFile),
                reinterpret_cast<void*>(MapViewOfFileHook));
            g_hookInstalled = readHook && pointerHook && closeHook && loginHook && mapHook;
            if (!g_hookInstalled) {
                if (mapHook) {
                    Memory::SetHook(
                        false, reinterpret_cast<void**>(&g_mapViewOfFile),
                        reinterpret_cast<void*>(MapViewOfFileHook));
                }
                if (loginHook) {
                    Memory::SetHook(
                        false, reinterpret_cast<void**>(&g_loginInit),
                        reinterpret_cast<void*>(CLoginInitHook));
                }
                if (closeHook) {
                    Memory::SetHook(
                        false, reinterpret_cast<void**>(&g_closeHandle),
                        reinterpret_cast<void*>(CloseHandleHook));
                }
                if (pointerHook) {
                    Memory::SetHook(
                        false, reinterpret_cast<void**>(&g_setFilePointer),
                        reinterpret_cast<void*>(SetFilePointerHook));
                }
                if (readHook) {
                    Memory::SetHook(
                        false, reinterpret_cast<void**>(&g_readFile),
                        reinterpret_cast<void*>(ReadFileHook));
                }
            }
        }
        Logger::WriteFormat(
            "[NameSpaceStreaming] hook=%s mode=whole-file-map-fallback "
            "readAhead=%lu KiB smallFileCache=%lu MiB\r\n",
            g_hookInstalled ? "OK" : "FAILED", clampedKiB,
            clampedCacheMiB);
        return g_hookInstalled;
    }

    if (!g_hookInstalled) {
        Logger::WriteFormat("[NameSpaceStreaming] disabled\r\n");
        return true;
    }
    const bool mapRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_mapViewOfFile),
        reinterpret_cast<void*>(MapViewOfFileHook));
    const bool closeRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_closeHandle),
        reinterpret_cast<void*>(CloseHandleHook));
    const bool loginRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_loginInit),
        reinterpret_cast<void*>(CLoginInitHook));
    const bool readRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_readFile),
        reinterpret_cast<void*>(ReadFileHook));
    const bool pointerRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_setFilePointer),
        reinterpret_cast<void*>(SetFilePointerHook));
    const bool removed = mapRemoved && loginRemoved && closeRemoved &&
        pointerRemoved && readRemoved;
    if (removed) {
        g_hookInstalled = false;
        AcquireSRWLockExclusive(&g_readAheadLock);
        g_readAheadEntries.clear();
        g_lastReadAheadEntry = nullptr;
        ReleaseSRWLockExclusive(&g_readAheadLock);
    }
    return removed;
#else
    UNREFERENCED_PARAMETER(enable);
    UNREFERENCED_PARAMETER(readAheadKiB);
    UNREFERENCED_PARAMETER(smallFileCacheMiB);
    return false;
#endif
}

void LogNameSpaceStreamingStats(const char* phase) {
#if defined(_M_IX86)
    if (!g_hookInstalled) {
        return;
    }
    AcquireSRWLockShared(&g_readAheadLock);
    Logger::WriteFormat(
        "[NameSpaceStreaming] stats phase=%s elapsedMs=%lu fallback=%ld logicalCalls=%llu "
        "logicalBytes=%llu cacheHits=%llu backendCalls=%llu backendBytes=%llu "
        "virtualPositionQueries=%llu virtualSeeks=%llu physicalSeeks=%llu\r\n",
        phase ? phase : "unknown", GetTickCount() - g_startTick, g_fallbackCount,
        g_logicalReadCalls,
        g_logicalReadBytes, g_cacheHitCalls, g_backendReadCalls, g_backendReadBytes,
        g_virtualPositionQueries, g_virtualSeeks, g_physicalSeeks);
    for (const auto& entry : g_readAheadEntries) {
        if (entry.logicalReadCalls == 0) {
            continue;
        }
        Logger::WriteFormat(
            "[NameSpaceStreaming] file phase=%s name=%s logicalCalls=%llu "
            "logicalBytes=%llu cacheHits=%llu backendCalls=%llu backendBytes=%llu "
            "virtualPositionQueries=%llu virtualSeeks=%llu physicalSeeks=%llu "
            "wholeFileCached=%d\r\n",
            phase ? phase : "unknown", entry.name, entry.logicalReadCalls,
            entry.logicalReadBytes, entry.cacheHitCalls, entry.backendReadCalls,
            entry.backendReadBytes, entry.virtualPositionQueries,
            entry.virtualSeeks, entry.physicalSeeks,
            entry.wholeFileCached ? 1 : 0);
    }
    ReleaseSRWLockShared(&g_readAheadLock);
#else
    UNREFERENCED_PARAMETER(phase);
#endif
}
