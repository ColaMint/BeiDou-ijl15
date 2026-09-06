#include "stdafx.h"
#include "ProcessDiagnostics.h"
#include "Logger.h"

#pragma comment(lib, "psapi.lib")

namespace {
const char* MemoryStateName(DWORD state) {
    switch (state) {
    case MEM_COMMIT: return "COMMIT";
    case MEM_RESERVE: return "RESERVE";
    case MEM_FREE: return "FREE";
    default: return "UNKNOWN";
    }
}

const char* MemoryTypeName(DWORD type) {
    switch (type) {
    case MEM_IMAGE: return "IMAGE";
    case MEM_MAPPED: return "MAPPED";
    case MEM_PRIVATE: return "PRIVATE";
    default: return "NONE";
    }
}

double ToMiB(ULONGLONG bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
} // namespace

namespace ProcessDiagnostics {
void WriteMemorySnapshot(HANDLE file, const char* label) {
    Logger::WriteFormat(file, "Memory snapshot (%s):\r\n", label ? label : "unspecified");

    bool largeAddressAware = false;
    const auto executable = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
    __try {
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(executable);
        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            executable + dosHeader->e_lfanew);
        largeAddressAware = dosHeader->e_magic == IMAGE_DOS_SIGNATURE &&
            ntHeaders->Signature == IMAGE_NT_SIGNATURE &&
            (ntHeaders->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        largeAddressAware = false;
    }
    BOOL isWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &isWow64);

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        Logger::WriteFormat(
            file,
            "  Process: WorkingSet=%.2f MiB PeakWorkingSet=%.2f MiB "
            "PrivateUsage=%.2f MiB PagefileUsage=%.2f MiB PeakPagefileUsage=%.2f MiB\r\n",
            ToMiB(counters.WorkingSetSize), ToMiB(counters.PeakWorkingSetSize),
            ToMiB(counters.PrivateUsage), ToMiB(counters.PagefileUsage),
            ToMiB(counters.PeakPagefileUsage));
    } else {
        Logger::WriteFormat(file, "  Process counters unavailable: error=%lu\r\n", GetLastError());
    }

    ULONGLONG committed = 0;
    ULONGLONG reserved = 0;
    ULONGLONG freeBytes = 0;
    ULONGLONG largestFree = 0;
    ULONGLONG committedImage = 0;
    ULONGLONG committedMapped = 0;
    ULONGLONG committedPrivate = 0;
    DWORD regionCount = 0;
    DWORD freeRegionCount = 0;

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    uintptr_t maximum = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
#if defined(_M_IX86)
    if (largeAddressAware && isWow64) {
        maximum = 0xFFFEFFFFu;
    } else if (!largeAddressAware && maximum > 0x7FFEFFFFu) {
        maximum = 0x7FFEFFFFu;
    }
#endif
    Logger::WriteFormat(
        file, "  Executable: LAA=%s WOW64=%s pointerBits=%u addressMaximum=0x%08lX\r\n",
        largeAddressAware ? "yes" : "no", isWow64 ? "yes" : "no",
        static_cast<unsigned int>(sizeof(void*) * 8),
        static_cast<DWORD>(maximum));
    uintptr_t cursor = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    while (cursor <= maximum) {
        MEMORY_BASIC_INFORMATION memoryInfo{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memoryInfo, sizeof(memoryInfo))) {
            break;
        }

        const ULONGLONG regionSize = static_cast<ULONGLONG>(memoryInfo.RegionSize);
        ++regionCount;
        if (memoryInfo.State == MEM_COMMIT) {
            committed += regionSize;
            if (memoryInfo.Type == MEM_IMAGE) {
                committedImage += regionSize;
            } else if (memoryInfo.Type == MEM_MAPPED) {
                committedMapped += regionSize;
            } else if (memoryInfo.Type == MEM_PRIVATE) {
                committedPrivate += regionSize;
            }
        } else if (memoryInfo.State == MEM_RESERVE) {
            reserved += regionSize;
        } else if (memoryInfo.State == MEM_FREE) {
            freeBytes += regionSize;
            ++freeRegionCount;
            if (regionSize > largestFree) {
                largestFree = regionSize;
            }
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
        const uintptr_t next = base + memoryInfo.RegionSize;
        if (next <= cursor || next > maximum) {
            break;
        }
        cursor = next;
    }

    Logger::WriteFormat(
        file,
        "  AddressSpace: min=0x%08lX max=0x%08lX committed=%.2f MiB "
        "reserved=%.2f MiB free=%.2f MiB largestFree=%.2f MiB regions=%lu freeRegions=%lu\r\n",
        static_cast<DWORD>(reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress)),
        static_cast<DWORD>(maximum), ToMiB(committed), ToMiB(reserved), ToMiB(freeBytes),
        ToMiB(largestFree), regionCount, freeRegionCount);
    Logger::WriteFormat(
        file, "  CommitTypes: private=%.2f MiB mapped=%.2f MiB image=%.2f MiB\r\n",
        ToMiB(committedPrivate), ToMiB(committedMapped), ToMiB(committedImage));

    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        const ULONGLONG pageSize = performance.PageSize;
        Logger::WriteFormat(
            file,
            "  SystemCommit: current=%.2f MiB limit=%.2f MiB peak=%.2f MiB "
            "physicalAvailable=%.2f MiB\r\n",
            ToMiB(performance.CommitTotal * pageSize),
            ToMiB(performance.CommitLimit * pageSize),
            ToMiB(performance.CommitPeak * pageSize),
            ToMiB(performance.PhysicalAvailable * pageSize));
    } else {
        Logger::WriteFormat(file, "  System performance unavailable: error=%lu\r\n", GetLastError());
    }

    DWORD handleCount = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handleCount);
    Logger::WriteFormat(
        file, "  Objects: handles=%lu GDI=%lu USER=%lu heaps=%lu\r\n",
        handleCount,
        GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
        GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
        GetProcessHeaps(0, nullptr));
}

void WriteAddressDetails(HANDLE file, const char* label, const void* address) {
    const char* description = label ? label : "Address";
    if (!address) {
        Logger::WriteFormat(file, "%s region: address is null\r\n", description);
        return;
    }
    MEMORY_BASIC_INFORMATION memoryInfo{};
    if (!VirtualQuery(address, &memoryInfo, sizeof(memoryInfo))) {
        Logger::WriteFormat(file, "%s region: unavailable for 0x%08lX (error=%lu)\r\n",
            description, static_cast<DWORD>(reinterpret_cast<uintptr_t>(address)),
            GetLastError());
        return;
    }

    char modulePath[MAX_PATH]{};
    const auto module = static_cast<HMODULE>(memoryInfo.AllocationBase);
    const DWORD modulePathLength = memoryInfo.Type == MEM_IMAGE && module
        ? GetModuleFileNameA(module, modulePath, ARRAYSIZE(modulePath)) : 0;
    const char* moduleName = memoryInfo.Type == MEM_IMAGE
        ? "<image path unavailable>" : "<not an image>";
    if (modulePathLength) {
        const char* slash = strrchr(modulePath, '\\');
        moduleName = slash ? slash + 1 : modulePath;
    }

    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uintptr_t allocationBase = reinterpret_cast<uintptr_t>(memoryInfo.AllocationBase);
    Logger::WriteFormat(
        file,
        "%s region: module=%s moduleOffset=0x%08lX allocationBase=0x%08lX "
        "regionBase=0x%08lX regionSize=%.2f MiB state=%s type=%s "
        "protect=0x%08lX allocationProtect=0x%08lX\r\n",
        description, moduleName, static_cast<DWORD>(value - allocationBase),
        static_cast<DWORD>(allocationBase),
        static_cast<DWORD>(reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress)),
        ToMiB(memoryInfo.RegionSize), MemoryStateName(memoryInfo.State),
        MemoryTypeName(memoryInfo.Type), memoryInfo.Protect, memoryInfo.AllocationProtect);
}
} // namespace ProcessDiagnostics
