#include "stdafx.h"
#include "MissingSoundHook.h"
#include "Logger.h"

namespace {
constexpr DWORD kNullSoundErrorAddress = 0x0043FD55;
constexpr DWORD kNoSoundReturnAddress = 0x0043FEC6;
constexpr DWORD kGuardSequenceAddress = 0x0043FD46;
const BYTE kExpectedGuardSequence[] = {
    0x83, 0x7D, 0xE8, 0x00,             // cmp [ebp-18h], 0
    0xC6, 0x45, 0xFC, 0x0A,             // mov byte ptr [ebp-4], 0Ah
    0xBF, 0x03, 0x40, 0x00, 0x80,       // mov edi, E_POINTER
    0x75, 0x06,                         // jnz sound-object-ready
    0x57,                               // push edi
    0xE8, 0x89, 0x00, 0x62, 0x00        // call _com_issue_error
};

bool IsSupportedClientBuild() {
    __try {
        return memcmp(
            reinterpret_cast<const void*>(kGuardSequenceAddress),
            kExpectedGuardSequence, sizeof(kExpectedGuardSequence)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsGuardInstalled() {
    __try {
        const auto* patch = reinterpret_cast<const BYTE*>(kNullSoundErrorAddress);
        if (patch[0] != 0xE9 || patch[5] != 0x90) {
            return false;
        }
        const auto displacement = *reinterpret_cast<const LONG*>(patch + 1);
        return kNullSoundErrorAddress + 5 + displacement == kNoSoundReturnAddress;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool HookMissingSoundGuard(bool enable) {
#if defined(_M_IX86)
    if (!enable) {
        Logger::WriteFormat("[MissingSound] guard=DISABLED\r\n");
        return true;
    }
    if (!IsSupportedClientBuild()) {
        Logger::WriteFormat(
            "[MissingSound] unsupported client build; guard not installed\r\n");
        return false;
    }

    Memory::PatchJump(kNullSoundErrorAddress, kNoSoundReturnAddress);
    Memory::WriteByte(kNullSoundErrorAddress + 5, 0x90);
    const bool installed = IsGuardInstalled();
    Logger::WriteFormat(
        "[MissingSound] guard=%s\r\n", installed ? "OK" : "FAILED");
    return installed;
#else
    UNREFERENCED_PARAMETER(enable);
    return false;
#endif
}
