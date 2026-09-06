#include "stdafx.h"
#include "MapleTVMediaHook.h"
#include "Logger.h"

namespace {
constexpr uintptr_t kMapleTVConnectAddress = 0x00635142;
const BYTE kExpectedMapleTVConnectEntry[] = {
    0x56, 0x57, 0x33, 0xFF, 0x57, 0x57, 0x57, 0x57, 0x68
};

using GetProcAddressFn = decltype(&GetProcAddress);
using MapleTVConnectFn = int(__thiscall*)(void* self);

GetProcAddressFn g_getProcAddress = GetProcAddress;
MapleTVConnectFn g_mapleTVConnect =
    reinterpret_cast<MapleTVConnectFn>(kMapleTVConnectAddress);
bool g_getProcAddressHookInstalled = false;
bool g_downloadHookInstalled = false;

bool IsNamedExport(LPCSTR procName, const char* expectedName) {
    return reinterpret_cast<uintptr_t>(procName) > 0xFFFFu &&
        strcmp(procName, expectedName) == 0;
}

bool IsWzFlashRenderer(HMODULE module) {
    char modulePath[MAX_PATH]{};
    if (!module || !GetModuleFileNameA(module, modulePath, ARRAYSIZE(modulePath))) {
        return false;
    }

    const char* moduleName = modulePath;
    for (const char* cursor = modulePath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            moduleName = cursor + 1;
        }
    }
    return _stricmp(moduleName, "WzFlashRenderer.dll") == 0;
}

bool IsSupportedClientBuild() {
    __try {
        return memcmp(
            reinterpret_cast<const void*>(kMapleTVConnectAddress),
            kExpectedMapleTVConnectEntry,
            sizeof(kExpectedMapleTVConnectEntry)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall MapleTVConnectHook(void* self, void*) {
    UNREFERENCED_PARAMETER(self);
    return 0;
}

int __cdecl RenderFlashDisabled(int x, int y, int currentTime) {
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
    UNREFERENCED_PARAMETER(currentTime);
    return 1;
}

FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR procName) {
    FARPROC proc = g_getProcAddress(module, procName);
    if (proc && IsNamedExport(procName, "RenderFlash") &&
        IsWzFlashRenderer(module)) {
        Logger::WriteFormat(
            "[MapleTVMedia] WzFlashRenderer RenderFlash disabled\r\n");
        return reinterpret_cast<FARPROC>(RenderFlashDisabled);
    }
    return proc;
}
} // namespace

bool HookMapleTVMedia(bool disable) {
#if defined(_M_IX86)
    if (!disable) {
        Logger::WriteFormat("[MapleTVMedia] enabled by configuration\r\n");
        return true;
    }

    if (!IsSupportedClientBuild()) {
        Logger::WriteFormat(
            "[MapleTVMedia] unsupported client build; media was not disabled\r\n");
        return false;
    }

    if (!g_downloadHookInstalled) {
        g_downloadHookInstalled = Memory::SetHook(
            true,
            reinterpret_cast<void**>(&g_mapleTVConnect),
            reinterpret_cast<void*>(MapleTVConnectHook));
    }
    if (!g_getProcAddressHookInstalled) {
        g_getProcAddressHookInstalled = Memory::SetHook(
            true,
            reinterpret_cast<void**>(&g_getProcAddress),
            reinterpret_cast<void*>(GetProcAddressHook));
    }

    Logger::WriteFormat(
        "[MapleTVMedia] download=%s render=%s\r\n",
        g_downloadHookInstalled ? "DISABLED" : "HOOK_FAILED",
        g_getProcAddressHookInstalled ? "DISABLED" : "HOOK_FAILED");
    return g_downloadHookInstalled && g_getProcAddressHookInstalled;
#else
    UNREFERENCED_PARAMETER(disable);
    return false;
#endif
}
