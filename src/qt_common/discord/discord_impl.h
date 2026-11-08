// SPDX-FileCopyrightText: Copyright 2026 Yngchii-X3D Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "discord.h"
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    explicit DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update() override;

private:
    bool ConnectPipe();
    void DisconnectPipe();
    void SendPacket(uint32_t op, const std::string& json_str);

    Core::System& system;
    std::string game_title{};
    int64_t start_time{0};

#ifdef _WIN32
    HANDLE pipe_handle{reinterpret_cast<HANDLE>(-1)};
#endif
};

} // namespace DiscordRPC
