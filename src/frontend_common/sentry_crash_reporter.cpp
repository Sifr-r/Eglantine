// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/sentry_crash_reporter.h"

#ifdef EGLANTINE_ENABLE_SENTRY
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <sentry.h>

#include "common/fs/path_util.h"
#include "common/scm_rev.h"
#endif

namespace Frontend {

#ifdef EGLANTINE_ENABLE_SENTRY
namespace {

bool HasHostAndValidPort(std::string_view host) {
    const bool bracketed = host.starts_with('[');
    std::size_t port_separator = host.find(':');
    if (bracketed) {
        const auto closing_bracket = host.find(']');
        if (closing_bracket == std::string_view::npos || closing_bracket <= 1 ||
            port_separator == std::string_view::npos || port_separator > closing_bracket) {
            return false;
        }
        if (closing_bracket + 1 == host.size()) {
            return true;
        }
        port_separator = closing_bracket + 1;
        if (host[port_separator] != ':') {
            return false;
        }
    }
    if (port_separator == std::string_view::npos) {
        return !host.empty() && host.find_first_of("[]") == std::string_view::npos;
    }
    if (port_separator == 0) {
        return false;
    }
    const auto port_text = host.substr(port_separator + 1);
    std::uint16_t port{};
    const auto [end, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    return error == std::errc{} && end == port_text.data() + port_text.size() && port != 0;
}

bool HasDsnFormat(std::string_view dsn) {
    if (dsn.starts_with("https://")) {
        dsn.remove_prefix(8);
    } else if (dsn.starts_with("http://")) {
        dsn.remove_prefix(7);
    } else {
        return false;
    }

    if (dsn.find_first_of(" \t\n\r\v\f?#\\") != std::string_view::npos) {
        return false;
    }
    const auto path_start = dsn.find('/');
    if (path_start == std::string_view::npos) {
        return false;
    }
    const auto authority = dsn.substr(0, path_start);
    const auto key_end = authority.find('@');
    if (key_end == std::string_view::npos || key_end == 0 || authority.front() == ':' ||
        key_end != authority.rfind('@') || !HasHostAndValidPort(authority.substr(key_end + 1))) {
        return false;
    }

    dsn = dsn.substr(0, dsn.find_last_not_of('/') + 1);
    const auto project_id_start = dsn.rfind('/');
    if (project_id_start == std::string_view::npos) {
        return false;
    }
    const auto project_id = dsn.substr(project_id_start + 1);
    return project_id.find_first_not_of("0123456789") == std::string_view::npos;
}

sentry_value_t DiscardTelemetry(sentry_value_t value, void*) {
    sentry_value_decref(value);
    return sentry_value_new_null();
}

} // namespace
#endif

SentryCrashReporter::~SentryCrashReporter() {
#ifdef EGLANTINE_ENABLE_SENTRY
    if (active) {
        sentry_close();
    }
#endif
}

void SentryCrashReporter::Start([[maybe_unused]] SentryFrontend frontend) {
#ifdef EGLANTINE_ENABLE_SENTRY
    if (active) {
        return;
    }

    const char* dsn = std::getenv("SENTRY_DSN");
    if (!dsn || !*dsn) {
        return;
    }
    if (!HasDsnFormat(dsn)) {
        std::fputs("Sentry crash reporting disabled because SENTRY_DSN has an invalid format.\n",
                   stderr);
        return;
    }

    const char* frontend_name = frontend == SentryFrontend::Qt ? "qt" : "sdl";
    const auto cache_path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "sentry" /
                            frontend_name;
    std::error_code path_error;
    const auto database_path = std::filesystem::absolute(cache_path, path_error);
    if (path_error) {
        std::fputs("Sentry crash reporting disabled because its cache path could not be resolved.\n",
                   stderr);
        return;
    }

    const auto release = std::string{"eglantine@"} + Common::g_scm_rev;
    auto* options = sentry_options_new();
    if (!options) {
        std::fputs("Sentry crash reporting disabled because the SDK options could not be created.\n",
                   stderr);
        return;
    }

    sentry_options_set_dsn(options, dsn);
    sentry_options_set_release(options, release.c_str());
#ifdef _WIN32
    sentry_options_set_database_pathw(options, database_path.c_str());
#else
    sentry_options_set_database_path(options, database_path.c_str());
#endif
    sentry_options_set_debug(options, false);
    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_traces_sample_rate(options, 0.0);
    sentry_options_set_send_client_reports(options, false);
    sentry_options_set_before_send_log(options, DiscardTelemetry, nullptr);
    sentry_options_set_before_send_metric(options, DiscardTelemetry, nullptr);

    active = sentry_init(options) == 0;
    if (!active) {
        std::fputs("Sentry crash reporting disabled because the native SDK failed to initialize.\n",
                   stderr);
        return;
    }
    sentry_set_tag("frontend", frontend_name);
#endif
}

bool SentryCrashReporter::IsActive() const {
    return active;
}

} // namespace Frontend
