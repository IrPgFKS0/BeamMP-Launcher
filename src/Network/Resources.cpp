/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/


#include "Network/network.hpp"
#include <chrono>
#include <iomanip>
#include <ios>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/evp.h>

#if defined(_WIN32)
#include <ws2tcpip.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "Logger.h"
#include "Options.h"
#include "Startup.h"
#include "CombinedHost.h" // --combined: g_CombinedMode (host mounts local mods instead of downloading)
#include <Utils.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

#include "hashpp.h"

namespace fs = std::filesystem;

void CheckForDir() {
    if (!fs::exists(CachingDirectory)) {
        try {
            fs::create_directories(CachingDirectory);
        } catch (const std::exception& e) {
            error(std::string("Failed to create caching directory: ") + e.what() + ". This is a fatal error. Please make sure to configure a directory which you have permission to create, read and write from/to.");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::exit(1);
        }
    }
}
void WaitForConfirm() {
    while (!Terminate && !ModLoaded) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ModLoaded = false;
}

void Abord() {
    Terminate = true;
    TCPTerminate = true;
    info("Terminated!");
}

std::string Auth(SOCKET Sock) {
    TCPSend("VC" + GetVer(), Sock);

    auto Res = TCPRcv(Sock);

    if (Res.empty() || Res[0] == 'E' || Res[0] == 'K') {
        Abord();
        CoreSend("L");
        return "";
    }

    TCPSend(PublicKey, Sock);
    if (Terminate) {
        CoreSend("L");
        return "";
    }

    Res = TCPRcv(Sock);
    if (Res.empty() || Res[0] != 'P') {
        Abord();
        CoreSend("L");
        return "";
    }

    Res = Res.substr(1);
    if (Res.find_first_not_of("0123456789") == std::string::npos) {
        ClientID = std::stoi(Res);
    } else {
        Abord();
        CoreSend("L");
        UUl("Authentication failed!");
        return "";
    }
    TCPSend("SR", Sock);
    if (Terminate) {
        CoreSend("L");
        return "";
    }

    Res = TCPRcv(Sock);

    if (Res.empty() || Res == "-") {
        info("Didn't Receive any mods...");
        CoreSend("L");
        TCPSend("Done", Sock);
        info("Done!");
        return "";
    }

    if (Res[0] == 'E' || Res[0] == 'K') { // check emptiness first (above) so Res[0] is always a real byte
        Abord();
        CoreSend("L");
        return "";
    }
    return Res;
}

void UpdateUl(bool D, const std::string& msg) {
    if (D)
        UlStatus = "UlDownloading Resource " + msg;
    else
        UlStatus = "UlLoading Resource " + msg;
}

float DownloadSpeed = 0;

void AsyncUpdate(uint64_t& Rcv, uint64_t Size, const std::string& Name) {
    do {
        double pr = (Size > 0) ? (double(Rcv) / double(Size) * 100) : 100.0; // avoid nan/inf on a zero-byte mod
        std::string Per = std::to_string(trunc(pr * 10) / 10);
        std::string SpeedString = "";
        if (DownloadSpeed > 0.01) {
            std::stringstream ss;
            ss << " at " << std::setprecision(1) << std::fixed << DownloadSpeed << " Mbit/s";
            SpeedString = ss.str();
        }
        UpdateUl(true, Name + " (" + Per.substr(0, Per.find('.') + 2) + "%)" + SpeedString);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (!Terminate && Rcv < Size);
}

// MICROSOFT, I DONT CARE, WRITE BETTER CODE
#undef min

std::vector<char> TCPRcvRaw(SOCKET Sock, uint64_t& GRcv, uint64_t Size) {
    if (Sock == -1) {
        Terminate = true;
        UUl("Invalid Socket");
        return {};
    }
    std::vector<char> File(Size);
    uint64_t Rcv = 0;

    auto start = std::chrono::high_resolution_clock::now();

    int i = 0;
    do {
        // receive at most some MB at a time
        // NOTE: compute the min in 64-bit *before* casting to int. Casting
        // (Size - Rcv) to int first overflows for files >2GB (e.g. a 3.5GB map),
        // producing a negative length that makes RecvWaitAll return 0 and the
        // download abort with "Recv returned: 0".
        int Len = int(std::min<uint64_t>(Size - Rcv, 1 * 1024 * 1024));
        int Temp = RecvWaitAll(Sock, &File[Rcv], Len);
        if (Temp == -1 || Temp == 0) {
            debug("Recv returned: " + std::to_string(Temp));
            if (Temp == -1) {
                error("Socket error during download: " + std::to_string(WSAGetLastError()));
            }
            UUl("Socket Closed Code 1");
            KillSocket(Sock);
            Terminate = true;
            return {};
        }
        Rcv += Temp;
        GRcv += Temp;

        auto end = std::chrono::high_resolution_clock::now();
        auto difference = end - start;
        double bits_per_s = double(Rcv * 8) / double(std::chrono::duration_cast<std::chrono::milliseconds>(difference).count());
        double megabits_per_s = bits_per_s / 1000;
        DownloadSpeed = megabits_per_s;
        // every 8th iteration print the speed
        if (i % 8 == 0) {
            debug("Download speed: " + std::to_string(uint32_t(megabits_per_s)) + "Mbit/s");
        }
        ++i;
    } while (Rcv < Size && !Terminate);
    return File;
}
// Streaming variant of TCPRcvRaw: writes received bytes straight to `Out`
// instead of buffering the whole file in RAM. Required for very large mods
// (e.g. a 3.5GB map) and, more importantly, to avoid out-of-memory crashes
// when many big mods are synced while the game is also loading them. Peak
// memory here is a single 1MB chunk buffer regardless of file size.
bool TCPRcvToFile(SOCKET Sock, uint64_t& GRcv, uint64_t Size, std::ofstream& Out) {
    if (Sock == -1) {
        Terminate = true;
        UUl("Invalid Socket");
        return false;
    }
    std::vector<char> Buf(1 * 1024 * 1024);
    uint64_t Rcv = 0;
    auto start = std::chrono::high_resolution_clock::now();
    int i = 0;
    while (Rcv < Size && !Terminate) {
        int Len = int(std::min<uint64_t>(Size - Rcv, Buf.size()));
        int Temp = RecvWaitAll(Sock, Buf.data(), Len);
        if (Temp == -1 || Temp == 0) {
            debug("Recv returned: " + std::to_string(Temp));
            if (Temp == -1) {
                error("Socket error during download: " + std::to_string(WSAGetLastError()));
            }
            UUl("Socket Closed Code 1");
            KillSocket(Sock);
            Terminate = true;
            return false;
        }
        Out.write(Buf.data(), Temp);
        if (!Out) {
            error("Failed to write downloaded data to disk (out of disk space?)");
            Terminate = true;
            return false;
        }
        Rcv += Temp;
        GRcv += Temp;

        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        if (ms > 0) {
            DownloadSpeed = (double(Rcv * 8) / double(ms)) / 1000;
        }
        if (i % 8 == 0) {
            debug("Download speed: " + std::to_string(uint32_t(DownloadSpeed)) + "Mbit/s");
        }
        ++i;
    }
    return true;
}
void MultiKill(SOCKET Sock, SOCKET Sock1) {
    KillSocket(Sock1);
    KillSocket(Sock);
    Terminate = true;
}
SOCKET InitDSock() {
    SOCKET DSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKADDR_IN ServerAddr;
    if (DSock < 1) {
        KillSocket(DSock);
        Terminate = true;
        return 0;
    }
    ServerAddr.sin_family = AF_INET;
    ServerAddr.sin_port = htons(LastPort);
    inet_pton(AF_INET, LastIP.c_str(), &ServerAddr.sin_addr);
    if (connect(DSock, (SOCKADDR*)&ServerAddr, sizeof(ServerAddr)) != 0) {
        KillSocket(DSock);
        Terminate = true;
        return 0;
    }
    char Code[2] = { 'D', char(ClientID) };
    if (send(DSock, Code, 2, 0) != 2) {
        KillSocket(DSock);
        Terminate = true;
        return 0;
    }
    return DSock;
}

// NOTE: SingleNormalDownload was removed (dead code). The single-socket mod
// download now streams straight to disk via TCPRcvToFile (see NewSyncResources).
// MultiDownload below is still used by the legacy (non-LAN) sync path.

std::vector<char> MultiDownload(SOCKET MSock, SOCKET DSock, uint64_t Size, const std::string& Name) {
    DownloadSpeed = 0;

    uint64_t GRcv = 0;

    uint64_t MSize = Size / 2;
    uint64_t DSize = Size - MSize;

    std::thread Au([&] { AsyncUpdate(GRcv, Size, Name); });

    const std::vector<char> MData = TCPRcvRaw(MSock, GRcv, MSize);

    if (MData.empty()) {
        MultiKill(MSock, DSock);
        Terminate = true;
        Au.join();
        return {};
    }

    const std::vector<char> DData = TCPRcvRaw(DSock, GRcv, DSize);

    if (DData.empty()) {
        MultiKill(MSock, DSock);
        Terminate = true;
        Au.join();
        return {};
    }

    // ensure that GRcv is good before joining the async update thread
    GRcv = MData.size() + DData.size();
    if (GRcv != Size) {
        error("Something went wrong during download; didn't get enough data. Expected " + std::to_string(Size) + " bytes, got " + std::to_string(GRcv) + " bytes instead");
        Terminate = true;
        Au.join();
        return {};
    }

    Au.join();

    std::vector<char> Result {};
    Result.insert(Result.begin(), MData.begin(), MData.end());
    Result.insert(Result.end(), DData.begin(), DData.end());
    return Result;
}

void InvalidResource(const std::string& File) {
    UUl("Invalid mod \"" + File + "\"");
    warn("The server tried to sync \"" + File + "\" that is not a .zip file!");
    Terminate = true;
}

struct ModInfo {
    static std::pair<bool, std::vector<ModInfo>> ParseModInfosFromPacket(const std::string& packet) {
        bool success = false;
        std::vector<ModInfo> modInfos;
        try {
            auto json = nlohmann::json::parse(packet);
            if (json.empty()) {
                return std::make_pair(true, modInfos);
            }

            for (const auto& entry : json) {
                ModInfo modInfo {
                    .FileName = entry["file_name"],
                    .FileSize = entry["file_size"],
                    .Hash = entry["hash"],
                    .HashAlgorithm = entry["hash_algorithm"],
                };

                if (entry.contains("protected")) {
                    modInfo.Protected = entry["protected"];
                }

                modInfos.push_back(modInfo);
                success = true;
            }
        } catch (const std::exception& e) {
            debug(std::string("Failed to receive mod list: ") + e.what());
            debug("Failed to receive new mod list format! This server may be outdated, but everything should still work as expected.");
        }
        return std::make_pair(success, modInfos);
    }
    std::string FileName;
    size_t FileSize;
    std::string Hash;
    std::string HashAlgorithm;
    bool Protected = false;
};

nlohmann::json modUsage = {};

void UpdateModUsage(const std::string& fileName) {
    try {
        fs::path usageFile = CachingDirectory / "mods.json";

        if (!fs::exists(usageFile)) {
            if (std::ofstream file(usageFile); !file.is_open()) {
                error("Failed to create mods.json");
                return;
            } else {
                file.close();
            }
        }

        std::fstream file(usageFile, std::ios::in | std::ios::out);
        if (!file.is_open()) {
            error("Failed to open or create mods.json");
            return;
        }

        if (modUsage.empty()) {
            auto Size = fs::file_size(CachingDirectory / "mods.json");
            std::string modsJson(Size, 0);
            file.read(&modsJson[0], Size);

            if (!modsJson.empty()) {
                auto parsedModJson = nlohmann::json::parse(modsJson, nullptr, false);

                if (parsedModJson.is_object())
                    modUsage = parsedModJson;
            }
        }

        modUsage[fileName] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        file.clear();
        file.seekp(0, std::ios::beg);
        file << modUsage.dump();
        file.close();
    } catch (std::exception& e) {
        error("Failed to update mods.json: " + std::string(e.what()));
    }
}


// LAN-only build: delete mods that a previous session's sync flagged (those the
// server had dropped). Called from PreGame at startup, BEFORE BeamNG launches, so the
// game never sees mid-session deletions -- which is what broke the active map's mount
// and forced a relaunch. Best-effort; clears the pending list when done.
void ProcessPendingModRemovals() {
    try {
        auto pendingPath = CachingDirectory / "pending_mod_removals.txt";
        // Read the queued names first so we can report the count BEFORE the game launches --
        // visible even when there's nothing to do, so the pre-launch prune phase is obvious in
        // the launcher log instead of looking like removal only happens on connect.
        std::vector<std::string> pending;
        if (fs::exists(pendingPath)) {
            std::ifstream pf(pendingPath);
            std::string fn;
            while (std::getline(pf, fn)) {
                while (!fn.empty() && (fn.back() == '\r' || fn.back() == '\n' || fn.back() == ' ' || fn.back() == '\t'))
                    fn.pop_back();
                if (!fn.empty())
                    pending.push_back(fn);
            }
        }
        info("Processing " + std::to_string(pending.size()) + " pending mod removal(s) before launch");
        if (pending.empty())
            return;
        auto MpDir = GetGamePath() / beammp_wide("mods/multiplayer");
        for (const auto& fn : pending) {
            std::string lower = fn;
            for (char& c : lower)
                c = char(tolower(c));
            if (lower == "beammp.zip")
                continue; // never remove the mod itself
            std::error_code rec;
            auto target = MpDir / fn;
            if (fs::exists(target)) {
                fs::remove(target, rec);
                if (!rec)
                    info("Removed mod no longer on server: " + fn);
                else
                    debug("Could not remove pending mod '" + fn + "': " + rec.message());
            }
        }
        std::error_code dec;
        fs::remove(pendingPath, dec);
    } catch (const std::exception& e) {
        debug(std::string("Pending mod removal skipped: ") + e.what());
    }
}

void NewSyncResources(SOCKET Sock, const std::string& Mods, const std::vector<ModInfo> ModInfos) {
    if (ModInfos.empty()) {
        CoreSend("L");
        TCPSend("Done", Sock);
        info("Done!");
        return;
    }

    if (!SecurityWarning())
        return;

    info("Checking Resources...");

    CheckForDir();

    // LAN-only build: mods the server no longer lists must leave the client's
    // persisted mods/multiplayer -- but NOT mid-session. BeamNG already has them
    // mounted, and deleting them now fires onFilesChanged churn that breaks the
    // active map's mount (forcing a relaunch). So just RECORD them here;
    // ProcessPendingModRemovals() deletes them at the next launcher startup, before
    // BeamNG is started. They linger (inert -- not in the server's set, so never
    // spawned) for one extra session. Updated mods still go through the normal flow:
    // a changed hash is a cache miss -> re-download -> overwrite.
    try {
        auto MpDir = GetGamePath() / beammp_wide("mods/multiplayer");
        std::vector<std::string> toRemove;
        if (fs::exists(MpDir)) {
            for (const auto& entry : fs::directory_iterator(MpDir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".zip")
                    continue;
                std::string fn = entry.path().filename().string();
                std::string lower = fn;
                for (char& c : lower)
                    c = char(tolower(c));
                if (lower == "beammp.zip")
                    continue;
                bool onServer = false;
                for (const auto& mod : ModInfos) {
                    std::string expect = mod.FileName;
                    for (char& c : expect)
                        c = char(tolower(c));
                    if (expect == lower) {
                        onServer = true;
                        break;
                    }
                }
                if (!onServer)
                    toRemove.push_back(fn);
            }
        }
        // Overwrite the pending list with this sync's set (authoritative for the
        // server we just synced with). Cleared when there's nothing to remove.
        auto pendingPath = CachingDirectory / "pending_mod_removals.txt";
        std::error_code pec;
        if (toRemove.empty()) {
            fs::remove(pendingPath, pec);
        } else {
            std::ofstream pf(pendingPath, std::ios::trunc);
            for (const auto& fn : toRemove) {
                pf << fn << "\n";
                info("Mod no longer on server, will remove on next launch: " + fn);
            }
        }
    } catch (const std::exception& e) {
        debug(std::string("Mod prune scheduling skipped: ") + e.what());
    }

    // LAN-only build: keep the launcher cache (CachingDirectory) mirrored to the
    // server's current set, so it holds exactly one (current) version of each
    // served mod. The cache is what loads/updates mods/multiplayer, so stale
    // old-hash copies and mods the server dropped would otherwise pile up
    // (38GB+ observed). Remove any "<stem>-<hash8>.zip" cache file that isn't the
    // current version of a served mod. Plainly-named files (e.g. manually-placed
    // protected mods) don't match the pattern and are left untouched.
    try {
        std::vector<std::string> expectedCache;
        expectedCache.reserve(ModInfos.size());
        for (const auto& mod : ModInfos) {
            if (mod.Hash.length() < 8)
                continue;
            auto p = std::filesystem::path(mod.FileName);
            expectedCache.push_back(p.stem().string() + "-" + mod.Hash.substr(0, 8) + p.extension().string());
        }
        auto isCacheName = [](const std::string& n) {
            // matches "<stem>-<8 hex>.zip"
            if (n.size() < 14 || n.substr(n.size() - 4) != ".zip" || n[n.size() - 13] != '-')
                return false;
            for (size_t i = n.size() - 12; i < n.size() - 4; ++i) {
                char c = n[i];
                bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hex)
                    return false;
            }
            return true;
        };
        for (const auto& entry : fs::directory_iterator(CachingDirectory)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".zip")
                continue;
            const std::string name = entry.path().filename().string();
            bool keep = !isCacheName(name); // leave non-cache-named files alone
            for (const auto& e : expectedCache) {
                if (e == name) {
                    keep = true;
                    break;
                }
            }
            if (keep)
                continue;
            std::error_code rec;
            fs::remove(entry.path(), rec);
            if (!rec)
                info("Removed stale cached mod: " + name);
        }
    } catch (const std::exception& e) {
        debug(std::string("Cache prune skipped: ") + e.what());
    }

    std::string t;
    for (const auto& mod : ModInfos) {
        t += mod.FileName + ";";
    }

    if (t.empty())
        CoreSend("L");
    else
        CoreSend("L" + t);
    t.clear();

    info("Syncing...");

    std::vector<std::pair<std::string, std::filesystem::path>> CachedMods = {};
    if (deleteDuplicateMods) {
        for (const auto& entry : fs::directory_iterator(CachingDirectory)) {
            const std::string filename = entry.path().filename().string();
            if (entry.is_regular_file() && entry.path().extension() == ".zip" && filename.length() > 10) {
                CachedMods.push_back(std::make_pair(filename.substr(0, filename.length() - 13) + ".zip", entry.path()));
            }
        }
    }

    int ModNo = 0;
    int TotalMods = ModInfos.size();
    for (auto ModInfoIter = ModInfos.begin(), AlsoModInfoIter = ModInfos.begin(); ModInfoIter != ModInfos.end() && !Terminate; ++ModInfoIter, ++AlsoModInfoIter) {
        ++ModNo;
        if (deleteDuplicateMods) {
            for (auto& CachedMod : CachedMods) {
                const bool cachedModExists = CachedMod.first == ModInfoIter->FileName;
                const bool cachedModIsNotNewestVersion = CachedMod.second.stem().string() + ".zip" != std::filesystem::path(ModInfoIter->FileName).stem().string() + "-" + ModInfoIter->Hash.substr(0, 8) + ".zip";
                if (cachedModExists && cachedModIsNotNewestVersion) {
                    debug("Found duplicate mod '" + CachedMod.second.stem().string() + ".zip" + "' in cache, removing it");
                    std::filesystem::remove(CachedMod.second);
                    break;
                }
            }
        }
        if (ModInfoIter->Hash.length() < 8 || ModInfoIter->HashAlgorithm != "sha256") {
            error("Unsupported hash algorithm or invalid hash for '" + ModInfoIter->FileName + "'");
            Terminate = true;
            return;
        }
        auto FileName = std::filesystem::path(ModInfoIter->FileName).stem().string() + "-" + ModInfoIter->Hash.substr(0, 8) + std::filesystem::path(ModInfoIter->FileName).extension().string();
        auto PathToSaveTo = (CachingDirectory / FileName);

        if (g_CombinedMode) {
            // Combined host: the ENTIRE mod set is already on this machine -- the in-process server
            // serves it from ./Resources/Client. Never "download" it from ourselves over the link
            // (the virtual client has no socket for the file transfer, which is exactly what failed:
            // "Failed to send raw data to client"). Mount the local file into mods/multiplayer
            // directly. If it's already there and current (it usually is, from prior sessions), do
            // nothing. This skips the cache entirely for the host.
            try {
                auto MpDir = GetGamePath() / beammp_wide("mods/multiplayer");
                if (!fs::exists(MpDir)) {
                    fs::create_directories(MpDir);
                }
                auto Dest = MpDir / std::filesystem::path(ModInfoIter->FileName);
                std::error_code ec;
                bool current = fs::exists(Dest)
                    && fs::file_size(Dest, ec) == ModInfoIter->FileSize && !ec
                    && (!options.full_mod_hash || Utils::GetSha256HashReallyFastFile(Dest) == ModInfoIter->Hash);
                if (!current) {
                    // Locate the source under the server's client folder (Resources/Client/**).
                    std::filesystem::path Src;
                    std::filesystem::path ClientDir = std::filesystem::path("Resources") / "Client";
                    std::error_code sec;
                    if (fs::exists(ClientDir)) {
                        for (auto it = fs::recursive_directory_iterator(ClientDir, sec);
                             it != fs::recursive_directory_iterator(); ++it) {
                            if (it->is_regular_file() && it->path().filename() == std::filesystem::path(ModInfoIter->FileName)) {
                                Src = it->path();
                                break;
                            }
                        }
                    }
                    if (Src.empty()) {
                        // Broken-mod robustness: a single mod missing from Resources/Client must NOT
                        // abort the host's whole join. Skip it (the host joins without that one mod)
                        // and continue -- the same graceful degradation stock BeamMP gives a failed
                        // download, rather than killing the session over one bad/absent file.
                        warn("Combined host: mod '" + ModInfoIter->FileName + "' not found under Resources/Client; SKIPPING it (host joins without this mod). Check the server's mod set vs Resources/Client.");
                        continue;
                    }
                    std::string Tmp = Dest.string() + ".tmp";
                    fs::copy_file(Src, Tmp, fs::copy_options::overwrite_existing);
                    fs::rename(Tmp, Dest);
                    // info, not debug: launcher-only mode shows every mod it installs, and the host
                    // was the one surface where a mod silently changed under you.
                    info("Combined host: mounted updated server mod into mods/multiplayer: '" + ModInfoIter->FileName + "'");
                }
                UpdateUl(false, std::to_string(ModNo) + "/" + std::to_string(TotalMods) + ": " + ModInfoIter->FileName);
                UpdateModUsage(FileName);
                WaitForConfirm();
                continue;
            } catch (const std::exception& e) {
                // One mod failing to copy/mount (locked file, transient FS error, oversized/broken zip)
                // should skip that mod, not tear down the whole host session.
                warn(std::string("Combined host: local mod mount failed for '") + ModInfoIter->FileName + "': " + e.what() + " -- skipping this mod, continuing the join.");
                continue;
            }
        }
        // Fast validate (LAN default): the cache file is hash-named (<stem>-<hash8>.zip), so its
        // filename already encodes the content hash -- existence + exact size is a near-certain
        // match and avoids re-hashing the entire mod set on every connect (the "Checking
        // Resources..." delay). --full-mod-hash forces the original full SHA256 re-verification.
        std::error_code cacheSzEc;
        if (fs::exists(PathToSaveTo)
            && (options.full_mod_hash
                    ? (Utils::GetSha256HashReallyFastFile(PathToSaveTo) == ModInfoIter->Hash)
                    : (fs::file_size(PathToSaveTo, cacheSzEc) == ModInfoIter->FileSize && !cacheSzEc))) {
            debug("Mod '" + FileName + "' found in cache");
            UpdateUl(false, std::to_string(ModNo) + "/" + std::to_string(TotalMods) + ": " + ModInfoIter->FileName);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            try {
                if (!fs::exists(GetGamePath() / beammp_wide("mods/multiplayer"))) {
                    fs::create_directories(GetGamePath() / beammp_wide("mods/multiplayer"));
                }
                auto modname = ModInfoIter->FileName;
#if defined(__linux__)
                // Linux version of the game doesnt support uppercase letters in mod names
                for (char& c : modname) {
                    c = ::tolower(c);
                }
#endif
                debug("Mod name: " + modname);
                auto name = std::filesystem::path(GetGamePath()) / "mods/multiplayer" / modname;
                // LAN-only build: persist mods between sessions. Only skip the
                // copy when the file already in mods/multiplayer matches the
                // authoritative mod by CONTENT (sha256), not just size: a stale
                // copy of identical size would otherwise never be replaced,
                // leaving the game to mount outdated vehicle Lua (e.g. a broken
                // weapons controller that fails to compile and won't fire).
                // Fast validate (LAN default): trust existence + exact size and skip the copy;
                // --full-mod-hash adds the full SHA256 (original behavior, also catches the rare
                // stale same-size copy). Size stays a cheap pre-filter either way.
                std::error_code sizeEc;
                if (fs::exists(name)
                    && fs::file_size(name, sizeEc) == ModInfoIter->FileSize && !sizeEc
                    && (!options.full_mod_hash || Utils::GetSha256HashReallyFastFile(name) == ModInfoIter->Hash)) {
                    debug("Mod '" + modname + "' already present in mods/multiplayer, skipping copy");
                } else {
                    std::string tmp_name = name.string();
                    tmp_name += ".tmp";
                    fs::copy_file(PathToSaveTo, tmp_name, fs::copy_options::overwrite_existing);
                    fs::rename(tmp_name, name);
                }
                UpdateModUsage(FileName);
            } catch (std::exception& e) {
                error("Failed copy to the mods folder! " + std::string(e.what()));
                Terminate = true;
                continue;
            }
            WaitForConfirm();
            continue;
        } else if (auto OldCachedPath = CachingDirectory / std::filesystem::path(ModInfoIter->FileName).filename();
                   fs::exists(OldCachedPath) && Utils::GetSha256HashReallyFastFile(OldCachedPath) == ModInfoIter->Hash) {
            debug("Mod '" + FileName + "' found in old cache, copying it to the new cache");
            UpdateUl(false, std::to_string(ModNo) + "/" + std::to_string(TotalMods) + ": " + ModInfoIter->FileName);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            try {
                fs::copy_file(OldCachedPath, PathToSaveTo, fs::copy_options::overwrite_existing);

                if (!fs::exists(GetGamePath() / beammp_wide("mods/multiplayer"))) {
                    fs::create_directories(GetGamePath() / beammp_wide("mods/multiplayer"));
                }

                auto modname = ModInfoIter->FileName;

#if defined(__linux__)
                // Linux version of the game doesnt support uppercase letters in mod names
                for (char& c : modname) {
                    c = ::tolower(c);
                }
#endif

                debug("Mod name: " + modname);
                auto name = std::filesystem::path(GetGamePath()) / "mods/multiplayer" / modname;
                // LAN-only build: persist mods between sessions. Only skip the
                // copy when the file already in mods/multiplayer matches the
                // authoritative mod by CONTENT (sha256), not just size: a stale
                // copy of identical size would otherwise never be replaced,
                // leaving the game to mount outdated vehicle Lua (e.g. a broken
                // weapons controller that fails to compile and won't fire).
                // Fast validate (LAN default): trust existence + exact size and skip the copy;
                // --full-mod-hash adds the full SHA256 (original behavior, also catches the rare
                // stale same-size copy). Size stays a cheap pre-filter either way.
                std::error_code sizeEc;
                if (fs::exists(name)
                    && fs::file_size(name, sizeEc) == ModInfoIter->FileSize && !sizeEc
                    && (!options.full_mod_hash || Utils::GetSha256HashReallyFastFile(name) == ModInfoIter->Hash)) {
                    debug("Mod '" + modname + "' already present in mods/multiplayer, skipping copy");
                } else {
                    std::string tmp_name = name.string();
                    tmp_name += ".tmp";
                    fs::copy_file(PathToSaveTo, tmp_name, fs::copy_options::overwrite_existing);
                    fs::rename(tmp_name, name);
                }
                UpdateModUsage(FileName);
            } catch (std::exception& e) {
                error("Failed copy to the mods folder! " + std::string(e.what()));
                Terminate = true;
                continue;
            }
            WaitForConfirm();
            continue;
        }

        if (ModInfoIter->Protected) {
            std::string message = "Mod '" + ModInfoIter->FileName + "' is protected and therefore must be placed in the Resources/Caching folder manually here: " + absolute(CachingDirectory).string();

            error(message);
            UUl(message);
            Terminate = true;
            return;
        }

        CheckForDir();
        std::string FName = ModInfoIter->FileName;
        int DlAttempts = 0;
        static constexpr int MaxDlAttempts = 3;
        do {
            debug(beammp_wide("Loading file '") + Utils::ToWString(FName) + beammp_wide("' to '") + beammp_fs_string(PathToSaveTo) + beammp_wide("'"));
            TCPSend("f" + ModInfoIter->FileName, Sock);

            std::string Data = TCPRcv(Sock);
            if (Data == "CO" || Terminate) {
                Terminate = true;
                UUl("Server cannot find " + FName);
                break;
            }

            if (Data != "AG") {
                UUl("Received corrupted download confirmation, aborting download.");
                debug("Corrupted download confirmation: " + Data);
                Terminate = true;
                break;
            }

            std::string Name = std::to_string(ModNo) + "/" + std::to_string(TotalMods) + ": " + FName;

            // Stream straight to disk (don't hold the whole mod in RAM).
            DownloadSpeed = 0;
            uint64_t GRcv = 0;
            std::thread Au([&] { AsyncUpdate(GRcv, ModInfoIter->FileSize, Name); });
            bool DlOk = false;
            {
                std::ofstream OutFile(PathToSaveTo, std::ios::binary | std::ios::trunc);
                if (!OutFile) {
                    error(beammp_wide("Failed to open '") + beammp_fs_string(PathToSaveTo) + beammp_wide("' for writing"));
                    Terminate = true;
                } else {
                    DlOk = TCPRcvToFile(Sock, GRcv, ModInfoIter->FileSize, OutFile);
                }
            }
            Au.join();

            if (!DlOk || Terminate)
                break;
            UpdateUl(false, std::to_string(ModNo) + "/" + std::to_string(TotalMods) + ": " + FName);

            // verify size and hash. Use the non-throwing file_size overload: a write that failed or was
            // interrupted can leave the file absent, and the throwing overload would propagate out of the
            // whole sync routine for a joining player instead of degrading to a clean abort.
            bool VerifyOk = true;
            {
                std::error_code ec;
                auto WrittenSize = std::filesystem::file_size(PathToSaveTo, ec);
                if (ec || WrittenSize != ModInfoIter->FileSize) {
                    VerifyOk = false;
                    debug("Downloaded '" + FName + "' has wrong size, will retry");
                }
            }
            if (VerifyOk && Utils::GetSha256HashReallyFastFile(PathToSaveTo) != ModInfoIter->Hash) {
                VerifyOk = false;
                debug("Downloaded '" + FName + "' has wrong hash, will retry");
            }

            // SUCCESS -> stop. Without this break the do-while (which only exits on Terminate) re-requests
            // the SAME mod FOREVER on a clean download -- the endless "Download of 'turrets.zip'..." loop
            // that drops the client and forces a reconnect. On a real mismatch, retry a few times (covers a
            // transient corrupt transfer) before giving up, so one glitch doesn't abort the whole join.
            if (VerifyOk) {
                break;
            }
            if (++DlAttempts >= MaxDlAttempts) {
                error("Failed to download '" + FName + "' correctly after " + std::to_string(MaxDlAttempts) + " attempts (size/hash mismatch)");
                Terminate = true;
                break;
            }
            warn("Mod '" + FName + "' failed verification, retrying (" + std::to_string(DlAttempts) + "/" + std::to_string(MaxDlAttempts) + ")");
        } while (!Terminate);
        if (!Terminate) {
            if (!fs::exists(GetGamePath() / beammp_wide("mods/multiplayer"))) {
                fs::create_directories(GetGamePath() / beammp_wide("mods/multiplayer"));
            }

// Linux version of the game doesnt support uppercase letters in mod names
#if defined(__linux__)
            for (char& c : FName) {
                c = ::tolower(c);
            }
#endif

            // Copy to a temp name and atomically rename into place, so BeamNG's
            // mod watcher never sees a half-written zip (which produced
            // "Invalid ZIP file" errors on the first download of large mods).
            auto destName = std::filesystem::path(GetGamePath()) / "mods/multiplayer" / FName;
            std::string tmp_name = destName.string() + ".tmp";
            fs::copy_file(PathToSaveTo, tmp_name, fs::copy_options::overwrite_existing);
            fs::rename(tmp_name, destName);
            UpdateModUsage(FName);
        }
        WaitForConfirm();
    }

    if (!Terminate) {
        TCPSend("Done", Sock);
        info("Done!");
    } else {
        UlStatus = "Ulstart";
        info("Connection Terminated!");
    }
}

void SyncResources(SOCKET Sock) {
    std::string Ret = Auth(Sock);

    debug("Mod info: " + Ret);

    if (Ret.starts_with("R")) {
        debug("This server is likely outdated, not trying to parse new mod info format");
    } else {
        auto [success, modInfo] = ModInfo::ParseModInfosFromPacket(Ret);

        if (success) {
            NewSyncResources(Sock, Ret, modInfo);
            return;
        }
    }

    if (Ret.empty())
        return;

    if (!SecurityWarning())
        return;

    info("Checking Resources...");
    CheckForDir();

    std::vector<std::string> list = Utils::Split(Ret, ";");
    std::vector<std::string> FNames(list.begin(), list.begin() + (list.size() / 2));
    std::vector<std::string> FSizes(list.begin() + (list.size() / 2), list.end());
    list.clear();
    Ret.clear();

    int Amount = 0, Pos = 0;
    std::filesystem::path PathToSaveTo;
    std::string t;
    for (const std::string& name : FNames) {
        if (!name.empty()) {
            t += name.substr(name.find_last_of('/') + 1) + ";";
        }
    }
    if (t.empty())
        CoreSend("L");
    else
        CoreSend("L" + t);
    t.clear();
    for (auto FN = FNames.begin(), FS = FSizes.begin(); FN != FNames.end() && !Terminate; ++FN, ++FS) {
        auto pos = FN->find_last_of('/');
        auto ZIP = FN->find(".zip");
        if (ZIP == std::string::npos || FN->length() - ZIP != 4) {
            InvalidResource(*FN);
            return;
        }
        if (pos == std::string::npos)
            continue;
        Amount++;
    }
    if (!FNames.empty())
        info("Syncing...");
    SOCKET DSock = InitDSock();
    for (auto FN = FNames.begin(), FS = FSizes.begin(); FN != FNames.end() && !Terminate; ++FN, ++FS) {
        auto pos = FN->find_last_of('/');
        if (pos != std::string::npos) {
            PathToSaveTo = CachingDirectory / std::filesystem::path(*FN).filename();
        } else {
            continue;
        }
        Pos++;
        if (FS->find_first_not_of("0123456789") != std::string::npos) // validate BEFORE stoull (a non-numeric size field would throw std::invalid_argument)
            continue;
        auto FileSize = std::stoull(*FS);
        if (fs::exists(PathToSaveTo)) {
            if (fs::file_size(PathToSaveTo) == FileSize) {
                UpdateUl(false, std::to_string(Pos) + "/" + std::to_string(Amount) + ": " + PathToSaveTo.filename().string());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                try {
                    if (!fs::exists(GetGamePath() / beammp_wide("mods/multiplayer"))) {
                        fs::create_directories(GetGamePath() / beammp_wide("mods/multiplayer"));
                    }
                    auto modname = PathToSaveTo.filename().string();
#if defined(__linux__)
                    // Linux version of the game doesnt support uppercase letters in mod names
                    for (char& c : modname) {
                        c = ::tolower(c);
                    }
#endif
                    auto name = GetGamePath() / beammp_wide("mods/multiplayer") / Utils::ToWString(modname);
                    auto tmp_name = name;
                    tmp_name += L".tmp";
                    fs::copy_file(PathToSaveTo, tmp_name, fs::copy_options::overwrite_existing);
                    fs::rename(tmp_name, name);
                    UpdateModUsage(modname);
                } catch (std::exception& e) {
                    error("Failed copy to the mods folder! " + std::string(e.what()));
                    Terminate = true;
                    continue;
                }
                WaitForConfirm();
                continue;
            } else
                fs::remove(PathToSaveTo.wstring());
        }
        CheckForDir();
        std::string FName = PathToSaveTo.filename().string();
        do {
            debug("Loading file '" + FName + "' to '" + PathToSaveTo.string() + "'");
            TCPSend("f" + *FN, Sock);

            std::string Data = TCPRcv(Sock);
            if (Data == "CO" || Terminate) {
                Terminate = true;
                UUl("Server cannot find " + FName);
                break;
            }

            std::string Name = std::to_string(Pos) + "/" + std::to_string(Amount) + ": " + FName;

            std::vector<char> DownloadedFile = MultiDownload(Sock, DSock, FileSize, Name);

            if (Terminate)
                break;
            UpdateUl(false, std::to_string(Pos) + "/" + std::to_string(Amount) + ": " + FName);

            // 1. write downloaded file to disk
            {
                std::ofstream OutFile(PathToSaveTo, std::ios::binary | std::ios::trunc);
                OutFile.write(DownloadedFile.data(), DownloadedFile.size());
            }
            // 2. verify size
            if (std::filesystem::file_size(PathToSaveTo) != DownloadedFile.size()) {
                error(beammp_wide("Failed to write the entire file '") + beammp_fs_string(PathToSaveTo) + beammp_wide("' correctly (file size mismatch)"));
                Terminate = true;
            }
        } while (fs::file_size(PathToSaveTo) != std::stoull(*FS) && !Terminate);
        if (!Terminate) {
            if (!fs::exists(GetGamePath() / beammp_wide("mods/multiplayer"))) {
                fs::create_directories(GetGamePath() / beammp_wide("mods/multiplayer"));
            }

// Linux version of the game doesnt support uppercase letters in mod names
#if defined(__linux__)
            for (char& c : FName) {
                c = ::tolower(c);
            }
#endif

            // Atomic write (matches the other download sites, per BeamMP-Launcher#250): copy to a
            // temp name then rename into place, so BeamNG's mod watcher never mounts a half-written
            // zip ("Invalid ZIP file" on the first download of a large mod).
            auto destName = GetGamePath() / beammp_wide("mods/multiplayer") / Utils::ToWString(FName);
            auto tmp_name = destName;
            tmp_name += L".tmp";
            fs::copy_file(PathToSaveTo, tmp_name, fs::copy_options::overwrite_existing);
            fs::rename(tmp_name, destName);
            UpdateModUsage(FN->substr(pos));
        }
        WaitForConfirm();
    }

    KillSocket(DSock);
    if (!Terminate) {
        TCPSend("Done", Sock);
        info("Done!");
    } else {
        UlStatus = "Ulstart";
        info("Connection Terminated!");
    }
}
