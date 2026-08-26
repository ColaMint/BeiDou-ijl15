#pragma once

#include "Memory.h"
#include <cstdint>
#include <type_traits>

#define MEMBER_AT(T, OFFSET, NAME) \
    __declspec(property(get = get_##NAME, put = set_##NAME)) T NAME; \
    __forceinline const T& get_##NAME() const { return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(this) + OFFSET); } \
    __forceinline T& get_##NAME() { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + OFFSET); } \
    __forceinline void set_##NAME(const T& value) { *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + OFFSET) = value; }

#define MEMBER_HOOK(T, ADDRESS, NAME, ...) \
    inline static auto NAME = reinterpret_cast<T(__thiscall*)(void*, __VA_ARGS__)>(ADDRESS); \
    T NAME##_hook(__VA_ARGS__);

#define ATTACH_HOOK(TARGET, DETOUR) \
    Memory::SetHook(true, reinterpret_cast<void**>(&(TARGET)), CastHook(&(DETOUR)))

template <typename T>
constexpr void* CastHook(T fn) {
    union { T fn; void* ptr; } value{};
    value.fn = fn;
    return value.ptr;
}

template <typename T>
inline uintptr_t HookAddress(T value) {
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        return static_cast<uintptr_t>(value);
    } else {
        return reinterpret_cast<uintptr_t>(value);
    }
}

inline void PatchMemory(void* address, const void* value, size_t size) {
    Memory::WriteByteArray(static_cast<DWORD>(reinterpret_cast<uintptr_t>(address)),
                           const_cast<unsigned char*>(static_cast<const unsigned char*>(value)),
                           static_cast<int>(size));
}

template <typename T>
inline void Patch1(T address, unsigned char value) {
    Memory::WriteByte(static_cast<DWORD>(HookAddress(address)), value);
}

template <typename T>
inline void Patch4(T address, unsigned int value) {
    Memory::WriteInt(static_cast<DWORD>(HookAddress(address)), value);
}

template <typename T, typename U>
inline void PatchNop(T begin, U end) {
    Memory::PatchNop(static_cast<DWORD>(HookAddress(begin)),
                     static_cast<int>(HookAddress(end) - HookAddress(begin)));
}

template <typename T, typename U>
inline void PatchJmp(T address, U destination) {
    Memory::PatchJump(static_cast<DWORD>(HookAddress(address)),
                      static_cast<DWORD>(HookAddress(destination)));
}

template <typename T, typename U>
inline void PatchCall(T address, U destination, size_t size = 5) {
    const DWORD source = static_cast<DWORD>(HookAddress(address));
    Memory::WriteByte(source, 0xE8);
    Memory::WriteInt(source + 1, static_cast<unsigned int>(HookAddress(destination) - source - 5));
    if (size > 5) Memory::PatchNop(source + 5, static_cast<int>(size - 5));
}
