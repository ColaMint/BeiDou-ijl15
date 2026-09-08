#include "stdafx.h"
#include "LargePetLayerHook.h"
#include "Logger.h"
#include "WzLib/IWzGr2DLayer.h"

namespace {
constexpr uintptr_t kPetSetLayerZAddress = 0x00704480;
constexpr uintptr_t kPetPrepareActionLayerAddress = 0x00703D15;
constexpr size_t kPetActionLayerOffset = 0x124;

// The lowest stock CMob sub-layer on the same page/zMass key is K-7. Stock
// pets use K+3 (passive) or K+8 (active), so subtracting 16 puts either form
// behind characters, drops, ordinary mobs, and every known mob sub-layer.
constexpr int kLargePetZOffset = -16;

const BYTE kExpectedSetLayerZEntry[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x57,
    0x8B, 0xF9, 0x83, 0xBF, 0x24, 0x01, 0x00, 0x00
};
const BYTE kExpectedPrepareActionLayerEntry[] = {
    0xB8, 0xFC, 0xD9, 0xAA, 0x00, 0xE8, 0x79, 0xCE,
    0x35, 0x00, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00
};

using PetMethod = void(__thiscall*)(void* self);

PetMethod g_petSetLayerZ = reinterpret_cast<PetMethod>(kPetSetLayerZAddress);
PetMethod g_petPrepareActionLayer =
    reinterpret_cast<PetMethod>(kPetPrepareActionLayerAddress);
int g_sizeThreshold = 100;
bool g_setLayerZHookInstalled = false;
bool g_prepareActionLayerHookInstalled = false;

bool IsSupportedClientBuild() {
    __try {
        return memcmp(reinterpret_cast<const void*>(kPetSetLayerZAddress),
                      kExpectedSetLayerZEntry,
                      sizeof(kExpectedSetLayerZEntry)) == 0 &&
            memcmp(reinterpret_cast<const void*>(kPetPrepareActionLayerAddress),
                   kExpectedPrepareActionLayerEntry,
                   sizeof(kExpectedPrepareActionLayerEntry)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

IWzGr2DLayer* GetActionLayer(void* pet) {
    if (!pet) {
        return nullptr;
    }
    return *reinterpret_cast<IWzGr2DLayer**>(
        reinterpret_cast<BYTE*>(pet) + kPetActionLayerOffset);
}

bool HasOverlay(IWzGr2DLayer* layer) {
    VARIANT overlay;
    VariantInit(&overlay);
    const HRESULT result = layer->get_overlay(&overlay);
    if (FAILED(result)) {
        return true;
    }

    const bool hasOverlay = overlay.vt != VT_EMPTY;
    VariantClear(&overlay);
    return hasOverlay;
}

bool IsLargeCanvas(IWzCanvas* canvas) {
    if (!canvas) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    return SUCCEEDED(canvas->get_width(&width)) &&
        SUCCEEDED(canvas->get_height(&height)) &&
        (width > static_cast<UINT>(g_sizeThreshold) ||
         height > static_cast<UINT>(g_sizeThreshold));
}

bool HasLargeActionFrame(IWzGr2DLayer* layer) {
    UINT count = 0;
    if (SUCCEEDED(layer->get_count(&count))) {
        // A pet action has only a small frame list. Refuse an implausible count
        // instead of walking arbitrary COM indices if the object is corrupted.
        if (count > 1024) {
            return false;
        }
        for (UINT index = 0; index < count; ++index) {
            VARIANT frameIndex;
            VariantInit(&frameIndex);
            frameIndex.vt = VT_I4;
            frameIndex.lVal = static_cast<LONG>(index);

            IWzCanvas* canvas = nullptr;
            const HRESULT result = layer->get_canvas(frameIndex, &canvas);
            const bool large = SUCCEEDED(result) && IsLargeCanvas(canvas);
            if (canvas) {
                canvas->Release();
            }
            if (large) {
                return true;
            }
        }
    }

    // This also covers layers whose implementation does not expose canvas
    // enumeration but does expose its current frame's bounds.
    int width = 0;
    int height = 0;
    return SUCCEEDED(layer->get_width(&width)) &&
        SUCCEEDED(layer->get_height(&height)) &&
        (width > g_sizeThreshold || height > g_sizeThreshold);
}

void ApplyLargePetZ(void* pet) {
    IWzGr2DLayer* layer = GetActionLayer(pet);
    if (!layer || HasOverlay(layer) || !HasLargeActionFrame(layer)) {
        return;
    }

    int stockZ = 0;
    if (SUCCEEDED(layer->get_z(&stockZ))) {
        layer->put_z(stockZ + kLargePetZOffset);
    }
}

void __fastcall PetSetLayerZHook(void* self, void*) {
    g_petSetLayerZ(self);
    ApplyLargePetZ(self);
}

void __fastcall PetPrepareActionLayerHook(void* self, void*) {
    g_petPrepareActionLayer(self);

    // PrepareActionLayer replaces every action canvas but does not recalculate
    // z. Restore the stock value first so switching from a large action to a
    // normal action cannot leave the pet permanently demoted.
    g_petSetLayerZ(self);
    ApplyLargePetZ(self);
}
} // namespace

bool HookLargePetLayer(bool enable, int sizeThreshold) {
#if defined(_M_IX86)
    if (!enable) {
        Logger::WriteFormat("[LargePetLayer] disabled by configuration\r\n");
        return true;
    }
    if (!IsSupportedClientBuild()) {
        Logger::WriteFormat(
            "[LargePetLayer] unsupported client build; hooks not installed\r\n");
        return false;
    }

    g_sizeThreshold = sizeThreshold > 0 ? sizeThreshold : 100;
    if (!g_setLayerZHookInstalled) {
        g_setLayerZHookInstalled = Memory::SetHook(
            true,
            reinterpret_cast<void**>(&g_petSetLayerZ),
            reinterpret_cast<void*>(PetSetLayerZHook));
    }
    if (g_setLayerZHookInstalled && !g_prepareActionLayerHookInstalled) {
        g_prepareActionLayerHookInstalled = Memory::SetHook(
            true,
            reinterpret_cast<void**>(&g_petPrepareActionLayer),
            reinterpret_cast<void*>(PetPrepareActionLayerHook));
    }
    if (!g_prepareActionLayerHookInstalled && g_setLayerZHookInstalled) {
        Memory::SetHook(
            false,
            reinterpret_cast<void**>(&g_petSetLayerZ),
            reinterpret_cast<void*>(PetSetLayerZHook));
        g_setLayerZHookInstalled = false;
    }

    Logger::WriteFormat(
        "[LargePetLayer] hooks=%s threshold=%d zOffset=%d\r\n",
        g_setLayerZHookInstalled && g_prepareActionLayerHookInstalled
            ? "OK" : "FAILED",
        g_sizeThreshold,
        kLargePetZOffset);
    return g_setLayerZHookInstalled && g_prepareActionLayerHookInstalled;
#else
    UNREFERENCED_PARAMETER(enable);
    UNREFERENCED_PARAMETER(sizeThreshold);
    return false;
#endif
}
