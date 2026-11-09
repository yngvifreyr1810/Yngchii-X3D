// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef _WIN32
#include <cstddef>
#include <atomic>

extern "C" {
__declspec(dllimport) void __stdcall WakeByAddressSingle(void* Address);
__declspec(dllimport) void __stdcall WakeByAddressAll(void* Address);
__declspec(dllimport) int __stdcall WaitOnAddress(volatile void* Address, void* CompareAddress, size_t AddressSize, unsigned long dwMilliseconds);
}

namespace Common {

template <typename T>
inline void FutexWait(const std::atomic<T>& atom, T seen_value, unsigned long timeout_ms = 0xFFFFFFFFUL) {
    T compare = seen_value;
    WaitOnAddress(const_cast<void*>(static_cast<const volatile void*>(&atom)), &compare, sizeof(T), timeout_ms);
}

template <typename T>
inline void FutexWakeSingle(const std::atomic<T>& atom) {
    WakeByAddressSingle(const_cast<void*>(static_cast<const volatile void*>(&atom)));
}

template <typename T>
inline void FutexWakeAll(const std::atomic<T>& atom) {
    WakeByAddressAll(const_cast<void*>(static_cast<const volatile void*>(&atom)));
}

} // namespace Common

#endif
