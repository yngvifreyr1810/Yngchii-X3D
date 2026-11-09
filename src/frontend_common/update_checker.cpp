// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fmt/format.h>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"

namespace UpdateChecker {

namespace {

std::optional<u64> ParseBuildNumber(std::string_view tag) {
    constexpr std::string_view prefix = "build-";
    if (!tag.starts_with(prefix)) {
        return std::nullopt;
    }
    tag.remove_prefix(prefix.size());
    u64 number{};
    const auto [end, error] = std::from_chars(tag.data(), tag.data() + tag.size(), number);
    if (error != std::errc{} || end != tag.data() + tag.size() || number == 0) {
        return std::nullopt;
    }
    return number;
}

constexpr std::string_view ReleaseRepo = "yngvifreyr1810/Yngchii-X3D";
// Download URLs name the repository by its current name; GitHub redirects the old one.
[[maybe_unused]] constexpr std::array RepoNames{std::string_view{"Yngchii-X3D"}, std::string_view{"Fritchii"}, std::string_view{"Lichii"}};

std::optional<u32> ParseNumber(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0') ||
        !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    u32 number{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return number;
}

CheckResult CheckFritchii([[maybe_unused]] const FritchiiVersion& current) {
#ifndef _WIN32
    // Fritchii only publishes Windows packages.
    return {};
#else
    auto source = DefaultSource();
    if (const char* override_url = std::getenv("EDEN_UPDATE_URL");
        override_url != nullptr && *override_url != '\0') {
        const auto parsed = ParseSourceOverride(override_url);
        if (!parsed) {
            LOG_ERROR(Frontend,
                      "Ignoring EDEN_UPDATE_URL {}: it must be https, or http on a loopback host",
                      override_url);
            return {.failure = Failure::Network};
        }
        LOG_WARNING(Frontend, "Checking for updates at {} (EDEN_UPDATE_URL, for testing only)",
                    override_url);
        source = *parsed;
    }

    const auto response = Common::Net::Fetch(source.origin, source.api_path);
    if (!response.body) {
        LOG_WARNING(Frontend, "Update check failed with status {}", response.status);
        return {.failure = ClassifyFailure(response.status), .status = response.status};
    }
    const auto releases = Common::Net::Release::ListFromJson(
        std::string_view{*response.body}, source.origin, std::string{ReleaseRepo});
    auto update = SelectFritchiiUpdate(releases, current, source);
    if (!update) {
        return {};
    }
    auto asset = SelectFritchiiAsset(*update, source);
    LOG_INFO(Frontend, "Received update {} ({})", update->title, update->tag);
    return {.update = std::move(update), .asset = std::move(asset)};
#endif
}

} // namespace

Failure ClassifyFailure(int status) {
    if (status == 403 || status == 429) {
        return Failure::RateLimited;
    }
    return status >= 400 ? Failure::Http : Failure::Network;
}

std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view build_tag,
                                                 std::string_view platform,
                                                 Common::Net::LinuxPackage package) {
    auto newest = ParseBuildNumber(build_tag);
    if (!newest) {
        return std::nullopt;
    }

    const Common::Net::Release* selected = nullptr;
    for (const auto& release : releases) {
        const auto number = ParseBuildNumber(release.tag);
        if (number && *number > *newest && !release.GetPlatformAssets(platform, package).empty()) {
            newest = number;
            selected = &release;
        }
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

CheckResult Check() {
    if (const auto running = RunningVersion()) {
        return CheckFritchii(*running);
    }
    if (!ParseBuildNumber(Common::g_build_release_tag) ||
        std::string_view{Common::g_build_release_platform}.empty()) {
        return {};
    }

    const auto list = Common::Net::GetReleases();
    if (!list.ok) {
        const auto failure = ClassifyFailure(list.status);
        LOG_WARNING(Frontend, "Update check failed with status {}", list.status);
        return {.failure = failure, .status = list.status};
    }

    auto update =
        SelectUpdate(list.releases, Common::g_build_release_tag, Common::g_build_release_platform,
                     Common::Net::CurrentLinuxPackage());
    if (update) {
        LOG_INFO(Frontend, "Received update {}", update->title);
    }
    return {.update = std::move(update)};
}

std::optional<Common::Net::Release> GetUpdate() {
    return Check().update;
}

std::string FritchiiVersion::ToString() const {
    if (patch != 0) {
        return fmt::format("{}.{}.{}", major, minor, patch);
    }
    if (minor != 0) {
        return fmt::format("{}.{}", major, minor);
    }
    return fmt::format("{}", major);
}

std::optional<FritchiiVersion> ParseFritchiiVersion(std::string_view text) {
    if (text.starts_with('v') || text.starts_with('V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    const auto first_dot = text.find('.');
    const auto major = ParseNumber(text.substr(0, first_dot));
    if (!major || *major == 0) {
        return std::nullopt;
    }
    if (first_dot == std::string_view::npos) {
        return FritchiiVersion{.major = *major, .minor = 0, .patch = 0};
    }
    const auto rest = text.substr(first_dot + 1);
    const auto second_dot = rest.find('.');
    const auto minor = ParseNumber(rest.substr(0, second_dot));
    if (!minor) {
        return std::nullopt;
    }
    if (second_dot == std::string_view::npos) {
        return FritchiiVersion{.major = *major, .minor = *minor, .patch = 0};
    }
    const auto patch_sv = rest.substr(second_dot + 1);
    if (patch_sv.find('.') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto patch = ParseNumber(patch_sv);
    if (!patch) {
        return std::nullopt;
    }
    return FritchiiVersion{.major = *major, .minor = *minor, .patch = *patch};
}

std::optional<FritchiiVersion> ParseBuildVersion(std::string_view build_version) {
    for (const std::string_view prefix : {"Yngchii-X3D v", "Yngchii v", "Fritchii v", "Frizti v"}) {
        if (build_version.starts_with(prefix)) {
            return ParseFritchiiVersion(build_version.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

std::optional<FritchiiVersion> RunningVersion() {
    for (const std::string_view name : {"Yngchii-X3D", "Yngchii", "Fritchii"}) {
        if (std::string_view{Common::g_build_name} == name) {
            if (const auto v = ParseBuildVersion(fmt::format("{} {}", name, Common::g_build_version))) {
                return v;
            }
        }
    }
    if (const auto v = ParseBuildVersion(Common::g_build_version)) {
        return v;
    }
    if (const auto v = ParseFritchiiVersion(Common::g_build_version)) {
        return v;
    }
    return FritchiiVersion{.major = 5, .minor = 1, .patch = 0};
}

std::optional<FritchiiVersion> ParseFritchiiTag(std::string_view tag) {
    for (const std::string_view prefix : {"yngchii-v", "fritchii-v", "v", "frizti-v"}) {
        if (tag.starts_with(prefix)) {
            return ParseFritchiiVersion(tag.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

std::string FritchiiAssetName(const FritchiiVersion& version) {
    return fmt::format("Fritchii-v{}-Windows-x64.zip", version.ToString());
}

Source DefaultSource() {
    return {.origin = "https://api.github.com",
            .api_path = fmt::format("/repos/{}/releases?per_page=100", ReleaseRepo)};
}

std::optional<Source> ParseSourceOverride(std::string_view url) {
    bool https = false;
    if (url.starts_with("https://")) {
        https = true;
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        url.remove_prefix(7);
    } else {
        return std::nullopt;
    }
    if (std::ranges::any_of(url, [](unsigned char c) { return c <= ' ' || c >= 0x7f; })) {
        return std::nullopt;
    }
    const auto slash = url.find('/');
    const auto authority = url.substr(0, slash);
    if (authority.empty() || authority.find_first_of("@?#\\") != std::string_view::npos) {
        return std::nullopt;
    }
    if (!https) {
        std::string host{authority.starts_with('[') ? authority.substr(0, authority.find(']') + 1)
                                                    : authority.substr(0, authority.find(':'))};
        std::ranges::transform(host, host.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") {
            return std::nullopt;
        }
    }
    return Source{.origin = fmt::format("{}://{}", https ? "https" : "http", authority),
                  .api_path = slash == std::string_view::npos ? std::string{"/"}
                                                              : std::string{url.substr(slash)},
                  .test = true};
}

std::optional<Common::Net::Asset> SelectFritchiiAsset(const Common::Net::Release& release,
                                                      const Source& source) {
    const auto version = ParseFritchiiTag(release.tag);
    if (!version) {
        return std::nullopt;
    }

#ifdef __AVX512F__
    constexpr std::string_view PreferredKeyword = "Zen4-AVX512";
    constexpr std::string_view FallbackKeyword = "Universal-AVX2";
#else
    constexpr std::string_view PreferredKeyword = "Universal-AVX2";
    constexpr std::string_view FallbackKeyword = "Zen4-AVX512";
#endif

    const auto ver_str = version->ToString();
    const std::string default_yngchii = fmt::format("Yngchii-X3D-v{}-Windows-x64.zip", ver_str);
    const std::string default_fritchii = fmt::format("Fritchii-v{}-Windows-x64.zip", ver_str);

    std::string preferred_url;
    std::string fallback_url;
    std::string generic_url;

    for (const auto& url : release.assets) {
        if (!url.ends_with(".zip")) {
            continue;
        }

        bool trusted = false;
        if (source.test) {
            trusted = url.starts_with(source.origin + "/") && url.find("..") == std::string::npos &&
                      url.find_first_of("?#\\") == std::string::npos;
        } else {
            trusted = url.find("github.com/yngvifreyr1810/Yngchii-X3D/releases/download/") != std::string::npos ||
                      url.find("github.com/Frizti/") != std::string::npos;
        }
        if (!trusted) {
            continue;
        }

        if (url.find(PreferredKeyword) != std::string::npos) {
            preferred_url = url;
            break;
        } else if (url.find(FallbackKeyword) == std::string::npos && url.find("Yngchii-X3D") != std::string::npos) {
            generic_url = url;
        } else if (url.ends_with("/" + default_fritchii) || url.ends_with("/" + default_yngchii)) {
            fallback_url = url;
        }
    }

    const std::string& chosen_url = !preferred_url.empty() ? preferred_url
                                  : !generic_url.empty()   ? generic_url
                                  : fallback_url;

    if (chosen_url.empty()) {
        return std::nullopt;
    }

    const auto slash = chosen_url.rfind('/');
    const std::string filename = slash != std::string::npos ? chosen_url.substr(slash + 1) : default_yngchii;

    std::string origin = "https://github.com";
    if (source.test) {
        origin = source.origin;
    }

    Common::Net::Asset asset{
        .name = "Yngchii-X3D",
        .url = origin,
        .path = chosen_url.substr(origin.size()),
        .filename = filename,
    };

    if (const auto info = release.asset_info.find(chosen_url); info != release.asset_info.end()) {
        asset.sha256 = info->second.sha256;
        asset.size = info->second.size;
    }
    return asset;
}

std::optional<Common::Net::Release> SelectFritchiiUpdate(
    std::span<const Common::Net::Release> releases, const FritchiiVersion& current,
    const Source& source) {
    const Common::Net::Release* selected = nullptr;
    FritchiiVersion newest = current;
    for (const auto& release : releases) {
        if (release.prerelease) {
            continue;
        }
        const auto version = ParseFritchiiTag(release.tag);
        if (version && *version > newest && SelectFritchiiAsset(release, source)) {
            newest = *version;
            selected = &release;
        }
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

} // namespace UpdateChecker
