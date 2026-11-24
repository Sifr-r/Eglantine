// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <nlohmann/json.hpp>
#include "common/logging.h"
#include "web_service/verify_login.h"
#include "web_service/web_backend.h"
#include "web_service/web_result.h"

namespace WebService {

bool VerifyLogin(const std::string& host, const std::string& username, const std::string& token) {
    Client client(host, username, token);
    auto reply = client.GetJson("/profile", false).returned_data;
    if (reply.empty()) {
        return false;
    }
    try {
        nlohmann::json json = nlohmann::json::parse(reply);
        if (!json.is_object()) {
            return false;
        }
        const auto iter = json.find("username");

        if (iter == json.end()) {
            return username.empty();
        }

        return iter->is_string() && *iter == username;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR(WebService, "Failed to parse login verification response: {}", e.what());
        return false;
    }
}

} // namespace WebService
