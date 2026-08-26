#include "stdafx.h"
#include "PacketDispatcher.h"
#include "HpMpAlert.h"
#include "weather.h"
#include "lamps.h"
#include "wvs/packet.h"

namespace {
constexpr unsigned short kHpMpAlertOpcode = 0x1000;
constexpr unsigned short kWeatherSyncOpcode = 0x373D;
constexpr unsigned short kLampPreviewOpcode = 0x373F;
constexpr unsigned short kSetFieldOpcode = 0x7D;
constexpr DWORD kProcessPacketAddress = 0x004965F1;
constexpr DWORD kWvsContextAddress = 0x00BE7918;
constexpr size_t kCharacterDataOffset = 0x20B8;

using ProcessPacket = void(__thiscall*)(void*, CInPacket*);
ProcessPacket g_processPacket = reinterpret_cast<ProcessPacket>(kProcessPacketAddress);

bool HasCharacterData() {
    auto* context = *reinterpret_cast<char* volatile*>(kWvsContextAddress);
    return context != nullptr && *reinterpret_cast<void* volatile*>(context + kCharacterDataOffset) != nullptr;
}

void __fastcall ProcessPacketHook(void* self, void*, CInPacket* packet) {
    if (packet == nullptr) {
        g_processPacket(self, packet);
        return;
    }

    const unsigned short opcode = packet->Peek2Public();
    switch (opcode) {
    case kHpMpAlertOpcode:
        HandleHpMpAlertPacket(packet);
        return;
    case kWeatherSyncOpcode:
        Weather_HandleWorldState(packet);
        return;
    case kLampPreviewOpcode:
        Lamp_HandlePreviewPacket(packet);
        return;
    default:
        break;
    }

    if (opcode >= 0x1D && opcode <= 0x7C && !HasCharacterData()) {
        return;
    }
    if (opcode == kSetFieldOpcode) {
        Weather_ExpectGameplayLoad();
        g_processPacket(self, packet);
        return;
    }
    g_processPacket(self, packet);
}
} // namespace

void HookPacketDispatcher(bool enable) {
    Memory::SetHook(enable, reinterpret_cast<void**>(&g_processPacket), ProcessPacketHook);
}
