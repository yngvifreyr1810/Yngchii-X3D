// SPDX-FileCopyrightText: Copyright 2026 Yngchii-X3D Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "discord_impl.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/x3d_detect.h"
#include "core/core.h"
#include "core/loader/loader.h"

#include <chrono>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace DiscordRPC {

static constexpr char DISCORD_CLIENT_ID[] = "1569019855057715350";
static constexpr char DEFAULT_LOGO_URL[] = "https://files.catbox.moe/aacnh6.png";

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    start_time = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    ConnectPipe();
}

DiscordImpl::~DiscordImpl() {
    DisconnectPipe();
}

bool DiscordImpl::ConnectPipe() {
#ifdef _WIN32
    if (pipe_handle != INVALID_HANDLE_VALUE) {
        return true;
    }
    for (int i = 0; i < 10; ++i) {
        std::string pipe_name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        pipe_handle = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_handle != INVALID_HANDLE_VALUE) {
            LOG_INFO(Frontend, "[DiscordRPC] Connected to {}", pipe_name);
            break;
        }
    }
    if (pipe_handle == INVALID_HANDLE_VALUE) {
        LOG_DEBUG(Frontend, "[DiscordRPC] No running Discord client found on pipes 0-9");
        return false;
    }

    // Send Handshake
    nlohmann::json handshake = {
        {"v", 1},
        {"client_id", DISCORD_CLIENT_ID}
    };
    SendPacket(0, handshake.dump());

    // Read Handshake Response
    uint32_t op = 0;
    uint32_t len = 0;
    DWORD read = 0;
    if (ReadFile(pipe_handle, &op, sizeof(op), &read, nullptr) &&
        ReadFile(pipe_handle, &len, sizeof(len), &read, nullptr) && len > 0) {
        std::vector<char> buf(len);
        ReadFile(pipe_handle, buf.data(), len, &read, nullptr);
        LOG_INFO(Frontend, "[DiscordRPC] Handshake acknowledged by Discord");
    }
    return true;
#else
    return false;
#endif
}

void DiscordImpl::DisconnectPipe() {
#ifdef _WIN32
    if (pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle);
        pipe_handle = INVALID_HANDLE_VALUE;
        LOG_INFO(Frontend, "[DiscordRPC] Disconnected pipe and cleared presence");
    }
#endif
}

void DiscordImpl::SendPacket([[maybe_unused]] uint32_t op, [[maybe_unused]] const std::string& json_str) {
#ifdef _WIN32
    if (pipe_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    uint32_t len = static_cast<uint32_t>(json_str.size());
    DWORD written = 0;
    WriteFile(pipe_handle, &op, sizeof(op), &written, nullptr);
    WriteFile(pipe_handle, &len, sizeof(len), &written, nullptr);
    WriteFile(pipe_handle, json_str.data(), len, &written, nullptr);
#endif
}

void DiscordImpl::Pause() {
    DisconnectPipe();
}

void DiscordImpl::Update() {
#ifdef _WIN32
    if (pipe_handle == INVALID_HANDLE_VALUE) {
        if (!ConnectPipe()) {
            return;
        }
    }

    std::string details;
    std::string state;
    std::string large_text;

    const bool x3d_active = Common::DetectRyzenX3DCpu().is_supported_x3d;

    if (system.IsPoweredOn()) {
        system.GetAppLoader().ReadTitle(game_title);
        details = fmt::format("Playing {}", game_title.empty() ? "Nintendo Switch" : game_title);
        state = x3d_active ? "⚡ Yngvi's X3D Optimize: ON | Vulkan" : "Stock Profile | Vulkan";
        large_text = fmt::format("{} - Yngchii-X3D v5", game_title.empty() ? "Switch" : game_title);
    } else {
        details = "Idle in Main Menu";
        state = x3d_active ? "⚡ Yngvi's X3D Optimize: Ready" : "Stock Profile | Ready";
        large_text = "Yngchii-X3D v5 (AMD Ryzen X3D & Auto-VRAM Edition)";
    }

    nlohmann::json act = {
        {"details", details},
        {"state", state},
        {"timestamps", {
            {"start", start_time}
        }},
        {"assets", {
            {"large_image", DEFAULT_LOGO_URL},
            {"large_text", large_text},
            {"small_image", DEFAULT_LOGO_URL},
            {"small_text", "Yngchii-X3D v5"}
        }}
    };

    nlohmann::json root = {
        {"cmd", "SET_ACTIVITY"},
        {"args", {
            {"pid", static_cast<int>(GetCurrentProcessId())},
            {"activity", act}
        }},
        {"nonce", std::to_string(start_time)}
    };

    SendPacket(1, root.dump());

    // Drain echo from Discord to keep pipe buffer clean
    uint32_t op = 0;
    uint32_t len = 0;
    DWORD read = 0;
    if (ReadFile(pipe_handle, &op, sizeof(op), &read, nullptr) &&
        ReadFile(pipe_handle, &len, sizeof(len), &read, nullptr) && len > 0) {
        std::vector<char> buf(len);
        ReadFile(pipe_handle, buf.data(), len, &read, nullptr);
    }
#endif
}

} // namespace DiscordRPC
