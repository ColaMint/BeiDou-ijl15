#include "stdafx.h"
#include "NameSpaceStreamingHook.h"
#include "Logger.h"

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace {
using MapViewOfFileFn = decltype(&MapViewOfFile);

MapViewOfFileFn g_mapViewOfFile = MapViewOfFile;
bool g_hookInstalled = false;
volatile LONG g_fallbackCount = 0;

bool IsNameSpaceCaller(const void* caller, DWORD* callerOffset) {
    const HMODULE module = GetModuleHandleA("NameSpace.dll");
    if (!module || !caller) {
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
    const uintptr_t address = reinterpret_cast<uintptr_t>(caller);
    if (address < base || address - base >= imageSize) {
        return false;
    }

    if (callerOffset) {
        *callerOffset = static_cast<DWORD>(address - base);
    }
    return true;
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

bool HookNameSpaceStreaming(bool enable) {
#if defined(_M_IX86)
    if (enable) {
        if (!g_hookInstalled) {
            g_hookInstalled = Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_mapViewOfFile),
                reinterpret_cast<void*>(MapViewOfFileHook));
        }
        Logger::WriteFormat(
            "[NameSpaceStreaming] hook=%s mode=whole-file-map-fallback\r\n",
            g_hookInstalled ? "OK" : "FAILED");
        return g_hookInstalled;
    }

    if (!g_hookInstalled) {
        Logger::WriteFormat("[NameSpaceStreaming] disabled\r\n");
        return true;
    }
    const bool removed = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_mapViewOfFile),
        reinterpret_cast<void*>(MapViewOfFileHook));
    if (removed) {
        g_hookInstalled = false;
    }
    return removed;
#else
    UNREFERENCED_PARAMETER(enable);
    return false;
#endif
}
