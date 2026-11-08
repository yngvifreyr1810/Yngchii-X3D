// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
extern "C" {
__declspec(dllimport) int __stdcall WaitOnAddress(volatile void* Address, void* CompareAddress,
                                                  std::size_t AddressSize,
                                                  unsigned long dwMilliseconds);
__declspec(dllimport) void __stdcall WakeByAddressSingle(void* Address);
__declspec(dllimport) void __stdcall WakeByAddressAll(void* Address);
}
#endif

namespace Common::Futex {

inline void Wait32(std::atomic<std::uint32_t>& atom, std::uint32_t expected) noexcept {
#ifdef _WIN32
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
    WaitOnAddress(&atom, &expected, sizeof(std::uint32_t), 0xFFFFFFFFUL);
#else
    atom.wait(expected, std::memory_order_relaxed);
#endif
}

inline void WakeOne32(std::atomic<std::uint32_t>& atom) noexcept {
#ifdef _WIN32
    WakeByAddressSingle(&atom);
#else
    atom.notify_one();
#endif
}

inline void WakeAll32(std::atomic<std::uint32_t>& atom) noexcept {
#ifdef _WIN32
    WakeByAddressAll(&atom);
#else
    atom.notify_all();
#endif
}

#if defined(_WIN32) && defined(__x86_64__)
inline std::uintptr_t FastThreadToken() noexcept {
    std::uintptr_t teb;
    asm("movq %%gs:0x30, %0" : "=r"(teb));
    return teb;
}
#endif

} // namespace Common::Futex
