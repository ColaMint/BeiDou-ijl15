#include "stdafx.h"
#include "ResManCacheHook.h"
#include "Logger.h"
#include "ProcessDiagnostics.h"
#include "NameSpaceStreamingHook.h"

namespace {
constexpr DWORD kInitializeResManAddress = 0x009F7159;
constexpr DWORD kResManGlobalAddress = 0x00BF14E8;
constexpr int kResManParam = 0x11;
constexpr size_t kSetResManParamVtableIndex = 5;

using InitializeResManFn = void(__thiscall*)(void*);
using SetResManParamFn = HRESULT(__stdcall*)(void*, int, int, int);

InitializeResManFn g_initializeResMan =
    reinterpret_cast<InitializeResManFn>(kInitializeResManAddress);
int g_retainTimeMs = 60000;
int g_nameSpaceCacheTimeMs = 60000;
bool g_hookInstalled = false;
bool g_diagnosticsEnabled = false;

void AppendResult(HRESULT result, void* resMan) {
    HANDLE file = Logger::Open();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    Logger::WriteFormat(
        file,
        "[ResManCache] object=0x%08lX param=0x%02X retain=%d ms namespace=%d ms HRESULT=0x%08lX\r\n",
        static_cast<DWORD>(reinterpret_cast<uintptr_t>(resMan)), kResManParam,
        g_retainTimeMs, g_nameSpaceCacheTimeMs, static_cast<DWORD>(result));
    if (g_diagnosticsEnabled) {
        ProcessDiagnostics::WriteMemorySnapshot(file, "after ResMan initialization");
    }
    Logger::FlushAndClose(file);
}

void __fastcall InitializeResManHook(void* self, void* edx) {
    UNREFERENCED_PARAMETER(edx);
    g_initializeResMan(self);
    LogNameSpaceStreamingStats("InitializeResMan");

    void* resMan = *reinterpret_cast<void**>(kResManGlobalAddress);
    if (!resMan) {
        AppendResult(E_POINTER, nullptr);
        return;
    }

    void** vtable = *reinterpret_cast<void***>(resMan);
    if (!vtable || !vtable[kSetResManParamVtableIndex]) {
        AppendResult(E_POINTER, resMan);
        return;
    }

    const auto setResManParam =
        reinterpret_cast<SetResManParamFn>(vtable[kSetResManParamVtableIndex]);
    const HRESULT result = setResManParam(
        resMan, kResManParam, g_retainTimeMs, g_nameSpaceCacheTimeMs);
    AppendResult(result, resMan);
}
} // namespace

bool HookResManCache(
        bool enable, int retainTimeMs, int nameSpaceCacheTimeMs, bool diagnostics) {
#if defined(_M_IX86)
    if (enable) {
        g_retainTimeMs = retainTimeMs < 0 ? 0 : retainTimeMs;
        g_nameSpaceCacheTimeMs = nameSpaceCacheTimeMs < 0 ? 0 : nameSpaceCacheTimeMs;
        g_diagnosticsEnabled = diagnostics;
        if (!g_hookInstalled) {
            g_hookInstalled = Memory::SetHook(
                true,
                reinterpret_cast<void**>(&g_initializeResMan),
                reinterpret_cast<void*>(InitializeResManHook));
        }
        Logger::WriteFormat(
            "[ResManCache] hook=%s retain=%d ms namespace=%d ms\r\n",
            g_hookInstalled ? "OK" : "FAILED",
            g_retainTimeMs, g_nameSpaceCacheTimeMs);
        return g_hookInstalled;
    }

    if (!g_hookInstalled) {
        Logger::WriteFormat("[ResManCache] disabled\r\n");
        return true;
    }
    const bool removed = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_initializeResMan),
        reinterpret_cast<void*>(InitializeResManHook));
    if (removed) {
        g_hookInstalled = false;
    }
    return removed;
#else
    UNREFERENCED_PARAMETER(enable);
    UNREFERENCED_PARAMETER(retainTimeMs);
    UNREFERENCED_PARAMETER(nameSpaceCacheTimeMs);
    UNREFERENCED_PARAMETER(diagnostics);
    return false;
#endif
}
