// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace YuzuCrashGuard {

/// Installs early pure Win32 crash handler and logs hardware details to %APPDATA%\eden\log\yngchii_startup.log
void InitializeEarlyCrashGuard();

} // namespace YuzuCrashGuard
