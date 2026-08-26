#include "stdafx.h"
#include "WeatherSystem.h"
#include "weather.h"

bool g_weatherSystemEnabled = true;

extern void Weather_Tick();
extern void WeatherPuddle_Frame();
extern void WeatherAccum_Frame();
extern void WeatherSplash_Frame();
extern void WeatherMove_Frame();
extern void WeatherMove_Restore();
extern void WeatherSway_Frame();

namespace {
using CallUpdate = void(__thiscall*)(void*, int);
CallUpdate g_callUpdate = reinterpret_cast<CallUpdate>(0x009F84D0);

void __fastcall CallUpdateHook(void* self, void*, int tCurTime) {
    Weather_Tick();
    if (Weather::IsFieldActive()) {
        if (Weather::HasFallingSky()) {
            WeatherSplash_Frame();
            WeatherPuddle_Frame();
            WeatherAccum_Frame();
            WeatherMove_Frame();
        } else {
            WeatherMove_Restore();
        }
        // WeatherSway_Frame();  // Temporarily disabled; keep the sway implementation.
    }
    g_callUpdate(self, tCurTime);
}
} // namespace

void HookWeatherFrame(bool enable) {
    Memory::SetHook(enable, reinterpret_cast<void**>(&g_callUpdate), CallUpdateHook);
}
