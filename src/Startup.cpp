/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "zip_file.h"
#include <charconv>
#include <cstring>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#if defined(_WIN32)
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#endif // __APPLE__
#include "Http.h"
#include "Logger.h"
#include "Network/network.hpp"
#include "Options.h"
#include "Security/Init.h"
#include "Startup.h"
#include "Utils.h"
#include "hashpp.h"
#include <filesystem>
#include <fstream>
#include <thread>

extern int TraceBack;
int ProxyPort = 0;

namespace fs = std::filesystem;

// Combined-host build: the launcher now links the server library, which ALSO defines a global
// ::Version (BeamMP-Server/include/Common.h). Wrap the launcher's local Version (+ its helpers,
// all used only in this TU) in an anonymous namespace so it has internal linkage and can't
// collide with the server's at link time (was LNK2005 "Version::Version already defined").
namespace {
struct Version {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    Version(uint8_t major, uint8_t minor, uint8_t patch);
    Version(const std::array<uint8_t, 3>& v);
};

std::array<uint8_t, 3> VersionStrToInts(const std::string& str) {
    std::array<uint8_t, 3> Version;
    std::stringstream ss(str);
    for (uint8_t& i : Version) {
        std::string Part;
        std::getline(ss, Part, '.');
        std::from_chars(&*Part.begin(), &*Part.begin() + Part.size(), i);
    }
    return Version;
}

bool IsOutdated(const Version& Current, const Version& Newest) {
    if (Newest.major > Current.major) {
        return true;
    } else if (Newest.major == Current.major && Newest.minor > Current.minor) {
        return true;
    } else if (Newest.major == Current.major && Newest.minor == Current.minor && Newest.patch > Current.patch) {
        return true;
    } else {
        return false;
    }
}

Version::Version(uint8_t major, uint8_t minor, uint8_t patch)
    : major(major)
    , minor(minor)
    , patch(patch) { }

Version::Version(const std::array<uint8_t, 3>& v)
    : Version(v[0], v[1], v[2]) {
}
} // anonymous namespace (launcher-local Version, combined-host build)

beammp_fs_string GetEN() {
#if defined(_WIN32)
    return L"BeamMP-Launcher.exe";
#elif defined(__linux__)
    return "BeamMP-Launcher";
#endif
}

std::string GetVer() {
    return "2.8";
}
std::string GetPatch() {
    return ".0";
}

beammp_fs_string GetEP(const beammp_fs_char* P) {
    static beammp_fs_string Ret = [&]() -> beammp_fs_string {
        if (P == nullptr) {
            // Called before initialization (GetEP(argv[0])). Constructing a string from a null
            // pointer is UB (silent crash), so return empty instead -- the caller (addToLog) then
            // just opens a relative path. main() now primes this with argv[0] before any logging.
            return beammp_fs_string {};
        }
        beammp_fs_string path(P);
        return path.substr(0, path.find_last_of(beammp_wide("\\/")) + 1);
    }();
    return Ret;
}

fs::path GetBP(const beammp_fs_char* P) {
    fs::path fspath = {};
#if defined(_WIN32)
    beammp_fs_char path[256];
    GetModuleFileNameW(nullptr, path, sizeof(path));
    fspath = path;
#elif defined(__linux__)
    fspath = fs::canonical("/proc/self/exe");
#elif defined(__APPLE__)
    pid_t pid = getpid();
    char path[PROC_PIDPATHINFO_MAXSIZE];
    // While this is fine for a raw executable,
    // an application bundle is read-only and these files
    // should instead be placed in Application Support.
    proc_pidpath(pid, path, sizeof(path));
    fspath = std::string(path);
#else
    fspath = beammp_fs_string(P);
#endif
    fspath = fs::weakly_canonical(fspath.string() + "/..");
#if defined(_WIN32)
    return fspath.wstring();
#else
    return fspath.string();
#endif
}

#if defined(_WIN32)
void ReLaunch() {
    std::wstring Arg;
    for (int c = 2; c <= options.argc; c++) {
        Arg += Utils::ToWString(options.argv[c - 1]);
        Arg += L" ";
    }
    info("Relaunch!");
    system("cls");
    ShellExecuteW(nullptr, L"runas", (GetBP() / GetEN()).c_str(), Arg.c_str(), nullptr, SW_SHOWNORMAL);
    ShowWindow(GetConsoleWindow(), 0);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    exit(1);
}
void URelaunch() {
    std::wstring Arg;
    for (int c = 2; c <= options.argc; c++) {
        Arg += Utils::ToWString(options.argv[c - 1]);
        Arg += L" ";
    }
    ShellExecuteW(nullptr, L"open", (GetBP() / GetEN()).c_str(), Arg.c_str(), nullptr, SW_SHOWNORMAL);
    ShowWindow(GetConsoleWindow(), 0);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    exit(1);
}
#elif defined(__linux__)
void ReLaunch() {
    std::string Arg;
    for (int c = 2; c <= options.argc; c++) {
        Arg += options.argv[c - 1];
        Arg += " ";
    }
    info("Relaunch!");
    system("clear");
    int ret = execv((GetBP() / GetEN()).c_str(), const_cast<char**>(options.argv));
    if (ret < 0) {
        error(std::string("execv() failed with: ") + strerror(errno) + ". Failed to relaunch");
        exit(1);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    exit(1);
}
void URelaunch() {
    int ret = execv((GetBP() / GetEN()).c_str(), const_cast<char**>(options.argv));
    if (ret < 0) {
        error(std::string("execv() failed with: ") + strerror(errno) + ". Failed to relaunch");
        exit(1);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    exit(1);
}
#endif

void CheckName() {
    // LAN build: do NOT rename the running executable.
    //
    // Upstream renamed the exe to GetEN() ("BeamMP-Launcher.exe") and relaunched itself whenever it
    // was started under any other filename -- part of the auto-update flow (so a freshly-downloaded
    // launcher would take over the canonical name). This LAN build updates manually and is shipped
    // under explicit names (the combined host exe, versioned diagnostic builds, ...) that the user
    // pins in shortcuts. Auto-renaming clobbered those names, broke the shortcuts, and -- in
    // --combined/--server-only -- would relaunch into the wrong mode. The exe name has no bearing on
    // functionality (the game reaches the launcher over the local proxy, not by filename), so the
    // rename is simply skipped: whatever name you launch as is kept.
    debug("CheckName: skipped (LAN build keeps the launched filename: '" + options.executable_name + "').");
}

#if defined(_WIN32)
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <iostream>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

bool CheckThumbprint(std::filesystem::path filepath)
{
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg = NULL;

    if (!CryptQueryObject(
        CERT_QUERY_OBJECT_FILE,
        filepath.wstring().c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
        CERT_QUERY_FORMAT_FLAG_BINARY,
        0,
        NULL, NULL, NULL,
        &hStore,
        &hMsg,
        NULL))
    {
        return false;
    }

    DWORD dwSignerInfo = 0;
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &dwSignerInfo) || dwSignerInfo == 0)
        return false;

    PCMSG_SIGNER_INFO pSignerInfo = (PCMSG_SIGNER_INFO)LocalAlloc(LPTR, dwSignerInfo);
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, pSignerInfo, &dwSignerInfo))
    {
        LocalFree(pSignerInfo);
        return false;
    }

    CERT_INFO certInfo = {};
    certInfo.Issuer = pSignerInfo->Issuer;
    certInfo.SerialNumber = pSignerInfo->SerialNumber;

    PCCERT_CONTEXT pCertContext = CertFindCertificateInStore(
        hStore,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_SUBJECT_CERT,
        &certInfo,
        NULL);

    if (!pCertContext)
    {
        LocalFree(pSignerInfo);
        return false;
    }

    BYTE hash[64];
    DWORD hashSize = sizeof(hash);

    bool match = false;
    if (CertGetCertificateContextProperty(pCertContext, CERT_SHA256_HASH_PROP_ID, hash, &hashSize))
    {
        std::string pubKeyData(
            reinterpret_cast<const char*>(pCertContext->pCertInfo->SubjectPublicKeyInfo.PublicKey.pbData),
            pCertContext->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData
        );

        std::string pubKeyHash = Utils::GetSha256HashReallyFast(pubKeyData, L"PubKey");
        debug("pub key hash: " + pubKeyHash);

        std::string fileThumbprint;
        for (DWORD i = 0; i < hashSize; i++)
        {
            char buf[3];
            sprintf_s(buf, "%02x", hash[i]);
            fileThumbprint += buf;
        }

        debug("File thumbprint: " +fileThumbprint);
        debug(filepath);

        if (fileThumbprint == "937f055b713de69416926ed4651d65219a0a0e77d7a78c1932c007e14326da33" && pubKeyHash == "2afad4a5773b0ac449f48350ce0d09c372be0d5bcbaa6d01332ce000baffde99"){
            match = true;
        }
    }

    CertFreeCertificateContext(pCertContext);
    CertCloseStore(hStore, 0);
    LocalFree(pSignerInfo);

    return match;
}
#include <windows.h>
#include <wintrust.h>
#include <Softpub.h>
#include <filesystem>
#include <string>

#pragma comment(lib, "wintrust")

bool VerifySignature(const std::filesystem::path& filePath)
{
    std::wstring path = filePath.wstring();

    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath = path.c_str();
    fileInfo.hFile = NULL;
    fileInfo.pgKnownSubject = NULL;

    WINTRUST_DATA winTrustData = {};
    winTrustData.cbStruct = sizeof(WINTRUST_DATA);
    winTrustData.dwUIChoice = WTD_UI_NONE;
    winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
    winTrustData.pFile = &fileInfo;

    winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    LONG status = WinVerifyTrust(
        NULL,
        &policyGUID,
        &winTrustData
    );

    debug(filePath);
    debug("Signature check code: " + std::to_string(status));

    winTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &policyGUID, &winTrustData);

    return (status == CERT_E_UNTRUSTEDROOT);
}
#endif

void CheckForUpdates(const std::string& CV) {
    // LAN-only build: no backend, so there is no update check. The launcher is
    // distributed and updated manually for LAN use.
    (void)CV;
    info("LAN-only build: launcher auto-update disabled.");
    TraceBack++;
}

#ifdef _WIN32
void LinuxPatch() {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, R"(Software\Wine)", 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS || getenv("USER") == nullptr)
        return;
    RegCloseKey(hKey);
    info("Wine/Proton Detected! If you are on windows delete HKEY_CURRENT_USER\\Software\\Wine in regedit");
    info("Applying patches...");

    result = RegCreateKey(HKEY_CURRENT_USER, R"(Software\Valve\Steam\Apps\284160)", &hKey);

    if (result != ERROR_SUCCESS) {
        fatal(R"(failed to create HKEY_CURRENT_USER\Software\Valve\Steam\Apps\284160)");
        return;
    }

    result = RegSetValueEx(hKey, "Name", 0, REG_SZ, (BYTE*)"BeamNG.drive", 12);

    if (result != ERROR_SUCCESS) {
        fatal(R"(failed to create the value "Name" under HKEY_CURRENT_USER\Software\Valve\Steam\Apps\284160)");
        return;
    }
    RegCloseKey(hKey);

    info("Patched!");
}
#endif

#if defined(_WIN32)

void InitLauncher() {
    SetConsoleTitleA(("BeamMP LAN Launcher v" + std::string(GetVer()) + GetPatch()).c_str());
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    // Disable console QuickEdit Mode. With it on (the Windows default), clicking or selecting text in
    // the window drops the console into "mark" mode and BLOCKS the process until a keypress -- which
    // is exactly why the combined host sometimes froze until you hit Enter. ENABLE_EXTENDED_FLAGS is
    // required for the QuickEdit bit to take effect; other input flags are preserved.
    {
        HANDLE hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD ConsoleMode = 0;
        if (hConsoleIn != nullptr && hConsoleIn != INVALID_HANDLE_VALUE && GetConsoleMode(hConsoleIn, &ConsoleMode)) {
            ConsoleMode = (ConsoleMode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hConsoleIn, ConsoleMode);
        }
    }
    debug("Launcher Version : " + GetVer() + GetPatch());
    CheckName();
    LinuxPatch();
    CheckLocalKey();
    CheckForUpdates(std::string(GetVer()) + GetPatch());
}
#elif defined(__linux__)

void InitLauncher() {
    info("BeamMP LAN Launcher v" + GetVer() + GetPatch());
    CheckName();
    CheckLocalKey();
    CheckForUpdates(std::string(GetVer()) + GetPatch());
}
#endif

size_t DirCount(const fs::path& path) {
    return (size_t)std::distance(fs::directory_iterator { path }, fs::directory_iterator {});
}

void CheckMP(const beammp_fs_string& Path) {
    // LAN-only build: PERSIST synced mods between sessions.
    // Previously this deleted every file in mods/multiplayer (except beammp.zip)
    // on each launch, which forced the entire mod set to be re-copied (and, if
    // the game had also wiped them, re-downloaded) every session. We now keep
    // them so returning to the server is fast. The sync step skips re-copying
    // mods already present, and the game's session cleanup no longer deletes
    // them either. Note: if you REMOVE mods from the server, stale client copies
    // will linger here -- clear mods/multiplayer manually in that case.
    (void)Path;
}

void EnableMP() {
    beammp_fs_string File(GetGamePath() / beammp_wide("mods/db.json"));
    if (!fs::exists(File))
        return;
    auto Size = fs::file_size(File);
    if (Size < 2)
        return;
    std::ifstream db(File);
    if (db.is_open()) {
        std::string Data(Size, 0);
        db.read(&Data[0], Size);
        db.close();
        nlohmann::json d = nlohmann::json::parse(Data, nullptr, false);
        if (Data.at(0) != '{' || d.is_discarded()) {
            // error("Failed to parse " + File); //TODO illegal formatting
            return;
        }
        if (d.contains("mods") && d["mods"].contains("multiplayerbeammp")) {
            d["mods"]["multiplayerbeammp"]["active"] = true;
            std::ofstream ofs(File);
            if (ofs.is_open()) {
                ofs << d.dump();
                ofs.close();
            } else {
                error(beammp_wide("Failed to write ") + File);
            }
        }
    }
}

// LAN-only build: if a new crash report appeared since our last launch, BeamNG crashed last
// session. The common case is the base-game "exceeded the allocated budget" crash (exit
// 0xC0000005) on map-load with too-large a mod set -- a BeamNG ENGINE limit, NOT system RAM and
// NOT this mod. Warn loudly in the launcher console so the host trims a mod instead of guessing.
// Uses a marker FILE (compares file mtimes, avoiding C++17 file_time<->system_clock conversion).
static void WarnIfLastSessionCrashed() {
    try {
        auto crashDir = GetGamePath() / beammp_wide("temp/crashReports");
        auto marker = CachingDirectory / "last_launch.marker";
        bool haveMarker = fs::exists(marker);
        auto markerTime = haveMarker ? fs::last_write_time(marker) : fs::file_time_type::min();
        bool crashed = false;
        if (haveMarker && fs::is_directory(crashDir)) { // is_directory => exists + a dir (avoids a file false-positive)
            std::error_code ec;
            for (auto it = fs::directory_iterator(crashDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                std::error_code tec;
                auto t = fs::last_write_time(it->path(), tec);
                if (!tec && t > markerTime) { crashed = true; break; }
            }
        }
        // (re)touch the marker so the NEXT launch only counts crashes newer than this launch
        { std::ofstream m(marker.string(), std::ios::trunc); m << "launch"; }
        if (crashed) {
            warn("====================================================================");
            warn("BeamNG CRASHED during your last session (a new crash report appeared).");
            warn("If it crashed while LOADING THE MAP, your mod set is over BeamNG's resource");
            warn("budget -- exit 0xC0000005, a base-game ENGINE limit, NOT RAM and NOT this mod.");
            warn("Fix: remove the largest / last-added mod from the server, then rejoin.");
            warn("====================================================================");
        }
    } catch (const std::exception& e) {
        debug(std::string("crash-history check skipped: ") + e.what());
    }
}

void PreGame(const beammp_fs_string& GamePath) {
    std::string GameVer = CheckVer(GamePath);
    info("Game Version : " + GameVer);

    CheckMP(GetGamePath() / beammp_wide("mods/multiplayer"));
    info(beammp_wide("Game user path: ") + beammp_fs_string(GetGamePath()));

    // LAN-only build: remove any mods a previous session flagged as no-longer-on-server
    // now -- before BeamNG launches -- instead of mid-session, which fired file-change
    // churn that broke the active map's mount and forced a relaunch.
    ProcessPendingModRemovals();

    // Heads-up if BeamNG crashed last session (usually the map-load mod/texture budget crash).
    WarnIfLastSessionCrashed();

    // LAN-only build: never download the mod from the backend. Instead install
    // the BeamMP.zip that ships next to the launcher executable. Pass
    // --no-download to skip installation entirely if you manage the mod yourself.
    if (options.no_download) {
        info("--no-download set: skipping BeamMP mod installation.");
        return;
    }

    try {
        if (!fs::exists(GetGamePath() / beammp_wide("mods/multiplayer"))) {
            fs::create_directories(GetGamePath() / beammp_wide("mods/multiplayer"));
        }
    } catch (std::exception& e) {
        fatal(e.what());
    }

#if defined(_WIN32)
    beammp_fs_string DestZip(GetGamePath() / LR"(mods\multiplayer\BeamMP.zip)");
#elif defined(__linux__)
    // Linux version of the game cant handle mods with uppercase names
    beammp_fs_string DestZip(GetGamePath() / R"(mods/multiplayer/beammp.zip)");
#endif
    beammp_fs_string SrcZip(beammp_wide("BeamMP.zip"));

    std::error_code ec;
    if (fs::exists(SrcZip)) {
        // LAN-only build: only (re)install the mod when it actually changed.
        // Overwriting the zip on every launch makes BeamNG see the mod as
        // deleted+recreated and re-mount it mid-startup, which breaks loading of
        // the vehicle-side extensions (positionVE etc.) and the multiplayer UI.
        // Compare size, then hash, and skip the copy if they already match.
        bool NeedsCopy = !fs::exists(DestZip);
        if (!NeedsCopy) {
            std::error_code sec;
            auto SrcSize = fs::file_size(SrcZip, sec);
            auto DestSize = fs::file_size(DestZip, sec);
            if (sec || SrcSize != DestSize) {
                NeedsCopy = true;
            } else {
                NeedsCopy = Utils::GetSha256HashReallyFastFile(SrcZip)
                    != Utils::GetSha256HashReallyFastFile(DestZip);
            }
        }
        // Always say WHICH mod build ended up in the game, installed or not. In combined mode the
        // host usually deploys the zip to both places at once, so the "installed" line never fired
        // and there was no confirmation on screen that the new mod took -- report the identity
        // (sha8 + size) in both branches so a version change is visible either way.
        std::error_code idec;
        auto SrcSha = Utils::GetSha256HashReallyFastFile(SrcZip);
        auto SrcSizeBytes = fs::file_size(SrcZip, idec);
        std::string Ident = "sha " + (SrcSha.size() >= 8 ? SrcSha.substr(0, 8) : SrcSha) + ", "
            + (idec ? std::string("? MB") : std::to_string(SrcSizeBytes / (1024 * 1024)) + " MB");
        if (!NeedsCopy) {
            info("BeamMP mod: already current in mods/multiplayer (" + Ident + ") -- no reinstall needed.");
        } else {
            fs::copy_file(SrcZip, DestZip, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error("Failed to install bundled BeamMP.zip: " + ec.message());
            } else {
                info("BeamMP mod: INSTALLED a new build into mods/multiplayer (" + Ident + ").");
            }
        }
    } else if (!fs::exists(DestZip)) {
        warn("No BeamMP.zip found next to the launcher and none present in mods/multiplayer. "
             "Place the LAN mod zip next to the launcher (or in mods/multiplayer) before joining.");
    }

    try {
        EnableMP();
    } catch (std::exception& e) {
        fatal(e.what());
    }

    beammp_fs_string Target(GetGamePath() / beammp_wide("mods/unpacked/beammp"));

    if (fs::is_directory(Target) && !fs::is_directory(Target + beammp_wide("/.git"))) {
        fs::remove_all(Target);
    }
}
