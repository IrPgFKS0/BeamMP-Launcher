/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

// LAN-only build: there is no online login. We derive a local identity (a name)
// and use it directly. The name doubles as the "key" that is sent to the server,
// which (in this fork) simply treats the key as the player's name. No request is
// ever made to auth.beammp.com.

#include "Logger.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>

namespace fs = std::filesystem;
std::string PublicKey;
std::string PrivateKey;
extern bool LoginAuth;
extern std::string Username;
extern std::string UserRole;
extern int UserID;

/// Strips anything that isn't a sane name character and trims/caps the result.
static std::string SanitizeName(std::string Name) {
    Name.erase(std::remove_if(Name.begin(), Name.end(),
                   [](unsigned char c) {
                       return !(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ' ');
                   }),
        Name.end());
    while (!Name.empty() && Name.front() == ' ')
        Name.erase(Name.begin());
    while (!Name.empty() && Name.back() == ' ')
        Name.pop_back();
    if (Name.size() > 32)
        Name = Name.substr(0, 32);
    return Name;
}

/// Generates a fresh, reasonably-unique auto name like "Player_3f9a".
static std::string GenerateName() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 0xFFFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Player_%04x", dist(gen));
    return std::string(buf);
}

/// Returns the local player name, generating and persisting one on first run.
/// Override by setting the BEAMMP_NAME environment variable or by editing the
/// "name" file next to the launcher.
static std::string GetLocalName() {
    if (const char* Env = std::getenv("BEAMMP_NAME"); Env != nullptr && Env[0] != '\0') {
        std::string N = SanitizeName(Env);
        if (!N.empty())
            return N;
    }
    if (fs::exists("name")) {
        std::ifstream In("name");
        if (In.is_open()) {
            std::string N;
            std::getline(In, N);
            N = SanitizeName(N);
            if (!N.empty())
                return N;
        }
    }
    std::string N = GenerateName();
    std::ofstream Out("name");
    if (Out.is_open()) {
        Out << N;
    }
    return N;
}

/// Sets up the local identity used everywhere a login would normally be required.
static void ApplyLocalIdentity() {
    Username = GetLocalName();
    UserRole = "USER";
    UserID = 0;
    LoginAuth = true;
    // The LAN server treats the key it receives as the player's name, so we send
    // the name itself. PrivateKey is only used for HTTP proxy headers, which are
    // unused without a backend, but we keep it non-empty for safety.
    PublicKey = Username;
    PrivateKey = Username;
}

void UpdateKey(const char* /*newKey*/) {
    // Kept for API compatibility. The LAN build does not use a downloaded key.
}

std::string Login(const std::string& fields) {
    // "LO" is the logout request.
    if (fields == "LO") {
        ApplyLocalIdentity();
        nlohmann::json out = { { "success", true }, { "message", "LAN mode (local identity)" } };
        return out.dump();
    }
    // LAN: the in-game name field (Direct Connect) sends "setname:<name>". Adopt
    // and persist it as the local identity (the server uses this as the player's
    // name). Anything else (e.g. the stock username/password login form) is
    // ignored -- we just use the existing local identity.
    static const std::string kSetName = "setname:";
    if (fields.rfind(kSetName, 0) == 0) {
        std::string requested = SanitizeName(fields.substr(kSetName.size()));
        if (!requested.empty()) {
            { std::ofstream out("name"); if (out.is_open()) out << requested; }
            Username = requested;
            UserRole = "USER";
            UserID = 0;
            LoginAuth = true;
            PublicKey = Username;
            PrivateKey = Username;
            info("LAN mode: player name set to '" + Username + "'");
            nlohmann::json out = {
                { "success", true }, { "username", Username }, { "role", UserRole },
                { "id", UserID }, { "message", "Name set to " + Username }
            };
            return out.dump();
        }
    }
    ApplyLocalIdentity();
    info("LAN mode: using local identity '" + Username + "' (no online login)");
    nlohmann::json out = {
        { "success", true },
        { "username", Username },
        { "role", UserRole },
        { "id", UserID },
        { "message", "Logged in locally as " + Username }
    };
    return out.dump();
}

void CheckLocalKey() {
    ApplyLocalIdentity();
}
