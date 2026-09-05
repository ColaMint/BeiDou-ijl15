#include "stdafx.h"
#include "ExceptionLogger.h"

#include <dbghelp.h>
#include <stdarg.h>

namespace {
constexpr DWORD kTopLevelExceptionFilterAddress = 0x0079704D;
constexpr DWORD kErrorDialogAddress = 0x0068DCC2;
constexpr DWORD kMaximumStackFrames = 128;
constexpr DWORD kFinalExceptionMaximumAgeMs = 30000;
constexpr DWORD kMsvcCppExceptionCode = 0xE06D7363;
constexpr ULONG_PTR kMsvcCppExceptionMagic = 0x19930520;
constexpr DWORD kFinalDialogReturnAddresses[] = {
    0x009F1BFB,
    0x009F1E98,
    0x009F2013,
    0x009F2092,
    0x009F2153,
    0x009F2214,
    0x009F22DC,
    0x009F241F,
};

using TopLevelExceptionFilter = LONG(WINAPI*)(EXCEPTION_POINTERS*);
using ErrorDialog = void(__thiscall*)(void*, DWORD, DWORD, DWORD);
using StackWalk64Fn = BOOL(WINAPI*)(
    DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
    PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
    PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymCleanupFn = BOOL(WINAPI*)(HANDLE);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
using SymGetLineFromAddr64Fn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
using SymFunctionTableAccess64Fn = PVOID(WINAPI*)(HANDLE, DWORD64);
using SymGetModuleBase64Fn = DWORD64(WINAPI*)(HANDLE, DWORD64);

TopLevelExceptionFilter g_topLevelExceptionFilter =
    reinterpret_cast<TopLevelExceptionFilter>(kTopLevelExceptionFilterAddress);
ErrorDialog g_errorDialog = reinterpret_cast<ErrorDialog>(kErrorDialogAddress);
volatile LONG g_handlingException = 0;
volatile LONG g_exceptionSequence = 0;
LONG g_lastExceptionSequence = 0;
DWORD g_lastExceptionTick = 0;
PVOID g_vectoredHandler = nullptr;
PVOID g_lastVehExceptionRecord = nullptr;
DWORD g_lastVehThreadId = 0;
DWORD g_lastVehExceptionCode = 0;
PVOID g_lastVehExceptionAddress = nullptr;
HMODULE g_dbgHelp = nullptr;
StackWalk64Fn g_stackWalk64 = nullptr;
SymInitializeFn g_symInitialize = nullptr;
SymCleanupFn g_symCleanup = nullptr;
SymSetOptionsFn g_symSetOptions = nullptr;
SymFromAddrFn g_symFromAddr = nullptr;
SymGetLineFromAddr64Fn g_symGetLineFromAddr64 = nullptr;
SymFunctionTableAccess64Fn g_symFunctionTableAccess64 = nullptr;
SymGetModuleBase64Fn g_symGetModuleBase64 = nullptr;

void LoadDbgHelp() {
    char systemDirectory[MAX_PATH]{};
    char dbgHelpPath[MAX_PATH]{};
    const UINT length = GetSystemDirectoryA(systemDirectory, ARRAYSIZE(systemDirectory));
    if (length == 0 || length >= ARRAYSIZE(systemDirectory) ||
        FAILED(StringCchPrintfA(dbgHelpPath, ARRAYSIZE(dbgHelpPath), "%s\\dbghelp.dll", systemDirectory))) {
        return;
    }

    g_dbgHelp = LoadLibraryA(dbgHelpPath);
    if (!g_dbgHelp) {
        return;
    }

    g_stackWalk64 = reinterpret_cast<StackWalk64Fn>(GetProcAddress(g_dbgHelp, "StackWalk64"));
    g_symInitialize = reinterpret_cast<SymInitializeFn>(GetProcAddress(g_dbgHelp, "SymInitialize"));
    g_symCleanup = reinterpret_cast<SymCleanupFn>(GetProcAddress(g_dbgHelp, "SymCleanup"));
    g_symSetOptions = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(g_dbgHelp, "SymSetOptions"));
    g_symFromAddr = reinterpret_cast<SymFromAddrFn>(GetProcAddress(g_dbgHelp, "SymFromAddr"));
    g_symGetLineFromAddr64 = reinterpret_cast<SymGetLineFromAddr64Fn>(GetProcAddress(g_dbgHelp, "SymGetLineFromAddr64"));
    g_symFunctionTableAccess64 = reinterpret_cast<SymFunctionTableAccess64Fn>(
        GetProcAddress(g_dbgHelp, "SymFunctionTableAccess64"));
    g_symGetModuleBase64 = reinterpret_cast<SymGetModuleBase64Fn>(
        GetProcAddress(g_dbgHelp, "SymGetModuleBase64"));
}

void GetErrorLogPath(char* path, size_t pathCount) {
    DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathCount));
    if (length == 0 || length >= pathCount) {
        StringCchCopyA(path, pathCount, "error.log");
    } else {
        char* slash = strrchr(path, '\\');
        if (slash) {
            StringCchCopyA(slash + 1, pathCount - (slash + 1 - path), "error.log");
        } else {
            StringCchCopyA(path, pathCount, "error.log");
        }
    }
}

HANDLE OpenErrorLog() {
    char path[MAX_PATH]{};
    GetErrorLogPath(path, ARRAYSIZE(path));

    return CreateFileA(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
}

void ResetErrorLog() {
    char path[MAX_PATH]{};
    GetErrorLogPath(path, ARRAYSIZE(path));
    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

void WriteText(HANDLE file, const char* text) {
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}

void WriteFormat(HANDLE file, const char* format, ...) {
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    const HRESULT result = StringCchVPrintfA(buffer, ARRAYSIZE(buffer), format, args);
    va_end(args);
    if (SUCCEEDED(result) || result == STRSAFE_E_INSUFFICIENT_BUFFER) {
        WriteText(file, buffer);
    }
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case kMsvcCppExceptionCode: return "MSVC_CPP_EXCEPTION";
    default: return "UNKNOWN_EXCEPTION";
    }
}

void WriteCppExceptionDetails(HANDLE file, const EXCEPTION_RECORD* record) {
#if defined(_MSC_VER) && defined(_M_IX86)
    if (!record || record->ExceptionCode != kMsvcCppExceptionCode ||
        record->NumberParameters < 3 ||
        record->ExceptionInformation[0] != kMsvcCppExceptionMagic) {
        return;
    }

    __try {
        const auto* object = reinterpret_cast<const DWORD*>(record->ExceptionInformation[1]);
        const auto* throwInfo = reinterpret_cast<const DWORD*>(record->ExceptionInformation[2]);
        WriteFormat(file, "C++ object: 0x%08lX, DWORD[0]=0x%08lX, DWORD[1]=0x%08lX\r\n",
                    static_cast<DWORD>(record->ExceptionInformation[1]),
                    object ? object[0] : 0, object ? object[1] : 0);

        // x86 MSVC ThrowInfo + CatchableTypeArray use absolute image pointers.
        const auto* catchableTypes = reinterpret_cast<const DWORD*>(throwInfo[3]);
        const LONG typeCount = catchableTypes ? static_cast<LONG>(catchableTypes[0]) : 0;
        if (typeCount > 0 && typeCount < 64) {
            bool isComError = false;
            bool isZException = false;
            WriteText(file, "C++ types:");
            for (LONG index = 0; index < typeCount; ++index) {
                const auto* catchableType = reinterpret_cast<const DWORD*>(catchableTypes[index + 1]);
                const auto* typeDescriptor = catchableType ?
                    reinterpret_cast<const BYTE*>(catchableType[1]) : nullptr;
                const char* decoratedName = typeDescriptor ?
                    reinterpret_cast<const char*>(typeDescriptor + sizeof(void*) * 2) : nullptr;
                if (decoratedName && decoratedName[0]) {
                    WriteFormat(file, "%s%s", index == 0 ? " " : ", ", decoratedName);
                    isComError = isComError || strstr(decoratedName, "_com_error") != nullptr;
                    isZException = isZException || strstr(decoratedName, "ZException") != nullptr;
                }
            }
            WriteText(file, "\r\n");
            if (isComError && object) {
                WriteFormat(file, "_com_error HRESULT candidate: 0x%08lX\r\n", object[1]);
            } else if (isZException && object) {
                WriteFormat(file, "ZException HRESULT: 0x%08lX\r\n", object[0]);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteText(file, "C++ exception metadata could not be decoded safely.\r\n");
    }
#else
    UNREFERENCED_PARAMETER(file);
    UNREFERENCED_PARAMETER(record);
#endif
}

void GetModuleDescription(DWORD64 address, char* output, size_t outputCount) {
    MEMORY_BASIC_INFORMATION memoryInfo{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
                      &memoryInfo, sizeof(memoryInfo))) {
        StringCchCopyA(output, outputCount, "<unknown>");
        return;
    }

    char modulePath[MAX_PATH]{};
    const auto module = static_cast<HMODULE>(memoryInfo.AllocationBase);
    if (!GetModuleFileNameA(module, modulePath, ARRAYSIZE(modulePath))) {
        StringCchCopyA(output, outputCount, "<unknown>");
        return;
    }

    const char* name = strrchr(modulePath, '\\');
    name = name ? name + 1 : modulePath;
    StringCchPrintfA(output, outputCount, "%s+0x%08llX", name,
                     address - reinterpret_cast<uintptr_t>(module));
}

void WriteResolvedFrame(HANDLE file, DWORD index, DWORD64 address) {
    if (!address) {
        return;
    }

    char module[2 * MAX_PATH]{};
    GetModuleDescription(address, module, ARRAYSIZE(module));

    alignas(SYMBOL_INFO) BYTE symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    HANDLE process = GetCurrentProcess();
    const BOOL hasSymbol = g_symFromAddr(process, address, &displacement, symbol);
    const BOOL hasLine = g_symGetLineFromAddr64 &&
        g_symGetLineFromAddr64(process, address, &lineDisplacement, &line);

    WriteFormat(file, "  #%03lu 0x%08llX %s", index, address, module);
    if (hasSymbol) {
        WriteFormat(file, "!%s+0x%llX", symbol->Name, displacement);
    }
    if (hasLine && line.FileName) {
        WriteFormat(file, " (%s:%lu)", line.FileName, line.LineNumber);
    }
    WriteText(file, "\r\n");
}

void WriteStackTrace(HANDLE file, EXCEPTION_POINTERS* exceptionInfo) {
#if defined(_M_IX86)
    if (!exceptionInfo || !exceptionInfo->ContextRecord) {
        WriteText(file, "Call stack unavailable: missing exception context.\r\n");
        return;
    }
    if (!g_stackWalk64 || !g_symInitialize || !g_symCleanup || !g_symFromAddr ||
        !g_symFunctionTableAccess64 || !g_symGetModuleBase64) {
        WriteText(file, "Call stack unavailable: required DbgHelp exports were not found.\r\n");
        return;
    }

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    if (!g_symInitialize(process, nullptr, TRUE)) {
        WriteFormat(file, "Call stack unavailable: SymInitialize failed (%lu).\r\n", GetLastError());
        return;
    }

    if (g_symSetOptions) {
        g_symSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    }

    CONTEXT context = *exceptionInfo->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    WriteText(file, "Call stack:\r\n");
    DWORD64 previousAddress = 0;
    DWORD frameCount = 0;
    for (; frameCount < kMaximumStackFrames && frame.AddrPC.Offset; ++frameCount) {
        const DWORD64 address = frame.AddrPC.Offset;
        if (address == previousAddress) {
            break;
        }
        previousAddress = address;
        WriteResolvedFrame(file, frameCount, address);

        SetLastError(ERROR_SUCCESS);
        if (!g_stackWalk64(
                IMAGE_FILE_MACHINE_I386, process, thread, &frame, &context, nullptr,
                g_symFunctionTableAccess64, g_symGetModuleBase64, nullptr)) {
            WriteFormat(file, "StackWalk64 stopped after frame #%lu (error %lu).\r\n",
                        frameCount, GetLastError());
            break;
        }
    }

    if (frameCount <= 1) {
        PVOID capturedFrames[kMaximumStackFrames]{};
        const USHORT capturedCount = CaptureStackBackTrace(
            0, static_cast<DWORD>(ARRAYSIZE(capturedFrames)), capturedFrames, nullptr);
        WriteText(file,
            "Fallback current-thread stack (includes exception logger and dispatcher frames):\r\n");
        for (USHORT index = 0; index < capturedCount; ++index) {
            WriteResolvedFrame(file, index,
                reinterpret_cast<DWORD64>(capturedFrames[index]));
        }
    }

    g_symCleanup(process);
#else
    UNREFERENCED_PARAMETER(exceptionInfo);
    WriteText(file, "Call stack unavailable: this logger only supports the Win32 client.\r\n");
#endif
}

void WriteExceptionLog(EXCEPTION_POINTERS* exceptionInfo, const char* source, LONG sequence) {
    HANDLE file = OpenErrorLog();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    WriteFormat(file,
        "\r\n==== %04u-%02u-%02u %02u:%02u:%02u.%03u PID=%lu TID=%lu ====\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
    WriteFormat(file, "Source: %s\r\n", source);
    WriteFormat(file, "ExceptionID: %ld\r\n", sequence);

    if (!exceptionInfo || !exceptionInfo->ExceptionRecord) {
        WriteText(file, "Exception information is unavailable.\r\n");
        CloseHandle(file);
        return;
    }

    const EXCEPTION_RECORD* record = exceptionInfo->ExceptionRecord;
    WriteFormat(file, "Exception: 0x%08lX (%s)\r\n", record->ExceptionCode,
                ExceptionName(record->ExceptionCode));
    WriteFormat(file, "Fault address: 0x%08p\r\n", record->ExceptionAddress);
    WriteCppExceptionDetails(file, record);

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const char* operation = record->ExceptionInformation[0] == 0 ? "read" :
            (record->ExceptionInformation[0] == 1 ? "write" : "execute");
        WriteFormat(file, "Access violation: %s address 0x%08lX\r\n", operation,
                    static_cast<DWORD>(record->ExceptionInformation[1]));
    }

#if defined(_M_IX86)
    if (exceptionInfo->ContextRecord) {
        const CONTEXT* context = exceptionInfo->ContextRecord;
        WriteFormat(file,
            "Registers: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX "
            "ESI=%08lX EDI=%08lX\r\n"
            "           EIP=%08lX ESP=%08lX EBP=%08lX EFLAGS=%08lX\r\n",
            context->Eax, context->Ebx, context->Ecx, context->Edx,
            context->Esi, context->Edi, context->Eip, context->Esp,
            context->Ebp, context->EFlags);
    }
#endif

    WriteStackTrace(file, exceptionInfo);
    FlushFileBuffers(file);
    CloseHandle(file);
}

bool IsHardException(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case kMsvcCppExceptionCode:
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
        return true;
    default:
        return false;
    }
}

LONG CALLBACK VectoredExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (!exceptionInfo || !exceptionInfo->ExceptionRecord ||
        !IsHardException(exceptionInfo->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&g_handlingException, 1, 0) == 0) {
        const LONG sequence = InterlockedIncrement(&g_exceptionSequence);
        WriteExceptionLog(exceptionInfo, "vectored first-chance exception", sequence);
        g_lastVehExceptionRecord = exceptionInfo->ExceptionRecord;
        g_lastVehThreadId = GetCurrentThreadId();
        g_lastVehExceptionCode = exceptionInfo->ExceptionRecord->ExceptionCode;
        g_lastVehExceptionAddress = exceptionInfo->ExceptionRecord->ExceptionAddress;
        g_lastExceptionSequence = sequence;
        g_lastExceptionTick = GetTickCount();
        InterlockedExchange(&g_handlingException, 0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI ExceptionFilterHook(EXCEPTION_POINTERS* exceptionInfo) {
    const bool alreadyLoggedByVeh = exceptionInfo && exceptionInfo->ExceptionRecord &&
        g_lastVehExceptionRecord == exceptionInfo->ExceptionRecord &&
        g_lastVehThreadId == GetCurrentThreadId() &&
        g_lastVehExceptionCode == exceptionInfo->ExceptionRecord->ExceptionCode &&
        g_lastVehExceptionAddress == exceptionInfo->ExceptionRecord->ExceptionAddress;
    if (!alreadyLoggedByVeh && InterlockedCompareExchange(&g_handlingException, 1, 0) == 0) {
        const LONG sequence = InterlockedIncrement(&g_exceptionSequence);
        WriteExceptionLog(exceptionInfo, "top-level unhandled exception", sequence);
        g_lastExceptionSequence = sequence;
        g_lastVehThreadId = GetCurrentThreadId();
        g_lastExceptionTick = GetTickCount();
        InterlockedExchange(&g_handlingException, 0);
    }
    return g_topLevelExceptionFilter(exceptionInfo);
}

bool IsWinMainErrorPath() {
    PVOID frames[32]{};
    const USHORT count = CaptureStackBackTrace(0, ARRAYSIZE(frames), frames, nullptr);
    for (USHORT index = 0; index < count; ++index) {
        const DWORD address = static_cast<DWORD>(reinterpret_cast<uintptr_t>(frames[index]));
        for (DWORD finalReturnAddress : kFinalDialogReturnAddresses) {
            if (address == finalReturnAddress) {
                return true;
            }
        }
    }
    return false;
}

void WriteFinalErrorDialogMarker() {
    if (!IsWinMainErrorPath()) {
        return;
    }

    HANDLE file = OpenErrorLog();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const DWORD threadId = GetCurrentThreadId();
    const DWORD age = GetTickCount() - g_lastExceptionTick;
    if (g_lastExceptionSequence != 0 && g_lastVehThreadId == threadId &&
        age <= kFinalExceptionMaximumAgeMs) {
        WriteFormat(file,
            "\r\n*** FINAL ERROR DIALOG: ExceptionID=%ld TID=%lu age=%lu ms ***\r\n",
            g_lastExceptionSequence, threadId, age);
    } else {
        WriteFormat(file,
            "\r\n*** FINAL ERROR DIALOG: no recent exception on TID=%lu ***\r\n",
            threadId);
    }
    FlushFileBuffers(file);
    CloseHandle(file);
}

void __fastcall ErrorDialogHook(void* self, void*, DWORD text1, DWORD text2, DWORD text3) {
    WriteFinalErrorDialogMarker();
    g_errorDialog(self, text1, text2, text3);
}

void WriteInitializationStatus(bool topLevelHookInstalled, bool vectoredHandlerInstalled,
                               bool errorDialogHookInstalled) {
    HANDLE file = OpenErrorLog();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    WriteFormat(file,
        "\r\n[ExceptionLogger %04u-%02u-%02u %02u:%02u:%02u] "
        "Capture=SEH+MSVC_CPP+FINAL_DIALOG TopLevelHook=%s VectoredHandler=%s "
        "ErrorDialogHook=%s DbgHelp=%s\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        topLevelHookInstalled ? "OK" : "FAILED",
        vectoredHandlerInstalled ? "OK" : "FAILED",
        errorDialogHookInstalled ? "OK" : "FAILED",
        g_dbgHelp ? "OK" : "FAILED");
    FlushFileBuffers(file);
    CloseHandle(file);
}
} // namespace

bool HookExceptionLogger(bool enable) {
#if defined(_M_IX86)
    if (enable) {
        ResetErrorLog();
        if (!g_dbgHelp) {
            LoadDbgHelp();
        }
        if (!g_vectoredHandler) {
            g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredExceptionHandler);
        }
        const bool topLevelHookInstalled = Memory::SetHook(
            true,
            reinterpret_cast<void**>(&g_topLevelExceptionFilter),
            reinterpret_cast<void*>(ExceptionFilterHook));
        const bool errorDialogHookInstalled = Memory::SetHook(
            true,
            reinterpret_cast<void**>(&g_errorDialog),
            reinterpret_cast<void*>(ErrorDialogHook));
        WriteInitializationStatus(
            topLevelHookInstalled, g_vectoredHandler != nullptr, errorDialogHookInstalled);
        return topLevelHookInstalled || errorDialogHookInstalled || g_vectoredHandler != nullptr;
    }

    const bool topLevelHookRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_topLevelExceptionFilter),
        reinterpret_cast<void*>(ExceptionFilterHook));
    const bool errorDialogHookRemoved = Memory::SetHook(
        false,
        reinterpret_cast<void**>(&g_errorDialog),
        reinterpret_cast<void*>(ErrorDialogHook));
    if (g_vectoredHandler) {
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = nullptr;
    }
    return topLevelHookRemoved && errorDialogHookRemoved;
#else
    UNREFERENCED_PARAMETER(enable);
    return false;
#endif
}
