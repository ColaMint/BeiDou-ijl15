#pragma once
#include "ztl/ztl.h"

inline IWzGr2DPtr& get_gr() {
    return *reinterpret_cast<IWzGr2DPtr*>(0x00BF14EC);
}

inline IWzResManPtr& get_rm() {
    return *reinterpret_cast<IWzResManPtr*>(0x00BF14E8);
}

inline IWzNameSpacePtr& get_root() {
    return *reinterpret_cast<IWzNameSpacePtr*>(0x00BF14E0);
}

inline int get_int32(Ztl_variant_t v, int nDefault) {
    Ztl_variant_t vInt;
    if (V_VT(&v) == VT_EMPTY || V_VT(&v) == VT_ERROR || FAILED(ZComAPI::ZComVariantChangeType(&vInt, &v, 0, VT_I4))) {
        return nDefault;
    } else {
        return V_I4(&vInt);
    }
}

// implementation in resolution.cpp
inline int get_screen_width() { return Client::m_nGameWidth; }
inline int get_screen_height() { return Client::m_nGameHeight; }
inline int get_adjust_cy() { return Client::m_nGameHeight - 600; }

// implementation in inlink.cpp
inline IUnknownPtr* __cdecl get_unknown_hook(IUnknownPtr* result, Ztl_variant_t& v) {
    using GetUnknown = IUnknownPtr* (__cdecl*)(IUnknownPtr*, Ztl_variant_t&);
    return reinterpret_cast<GetUnknown>(0x00414ADA)(result, v);
}
inline IUnknownPtr get_unknown(Ztl_variant_t& v) {
    IUnknownPtr pUnk;
    get_unknown_hook(std::addressof(pUnk), v);
    return pUnk;
}
