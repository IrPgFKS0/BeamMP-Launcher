/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "Network/network.hpp"
#include "CombinedHost.h"
#include "Utils.h"
#include <memory>
#include <zlib.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__linux__)
#include "linuxfixes.h"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "Logger.h"
#include "Options.h"
#include <charconv>
#include <mutex>
#include <string>
#include <thread>
#include "Options.h"
#include "Startup.h"
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

std::chrono::time_point<std::chrono::high_resolution_clock> PingStart, PingEnd;
bool GConnected = false;
bool CServer = true;
SOCKET CSocket = -1;
SOCKET GSocket = -1;
std::string magic;

int KillSocket(uint64_t Dead) {
    if (Dead == (SOCKET)-1) {
        debug("Kill socket got -1 returning...");
        return 0;
    }
    shutdown(Dead, SD_BOTH);
    int a = closesocket(Dead);
    if (a != 0) {
        warn("Failed to close socket!");
    }
    return a;
}

void GameSend(std::string_view Data) {
    static std::mutex Lock;
    std::scoped_lock Guard(Lock);
    // Don't send to the game proxy socket while it's closed/not yet connected.
    // After the game disconnects, CSocket holds a stale (closed) handle until the
    // next accept(); relay traffic arriving in that window used to fail with
    // 10038 (WSAENOTSOCK) and spam the log. We now reset CSocket to -1 on close
    // (see TCPGameServer) and bail out here.
    if (CSocket == (SOCKET)-1 || TCPTerminate) {
        return;
    }
    auto ToSend = Utils::PrependHeader<std::string_view>(Data);
    auto Result = send(CSocket, ToSend.data(), ToSend.size(), 0);
    if (Result < 0) {
        // The game closing/reconnecting races with relay sends; that's expected
        // and not actionable, so keep it at debug level instead of error.
        debug("(Game) send failed with error: " + std::to_string(WSAGetLastError()));
    }
}

void ServerSend(std::string Data, bool Rel) {
    if (Terminate || Data.empty())
        return;
    if (Data.size() > 500 && Data[0] == 'Z' && Data[1] == 'p') {
        // Was abort() -- a process-killing crash triggered by ordinary runtime data (an oversized
        // position packet). Check the packet CODE (Data[0..1]), not find("Zp") anywhere in the
        // payload, so a legit packet that merely contains "Zp" isn't dropped. Drop + keep alive.
        debug("Dropping oversized 'Zp' packet (" + std::to_string(Data.size()) + " bytes)");
        return;
    }
    char C = 0;
    bool Ack = false;
    int DLen = int(Data.length());
    if (DLen > 3)
        C = Data.at(0);
    if (C == 'O' || C == 'T')
        Ack = true;
    if (C == 'N' || C == 'W' || C == 'Y' || C == 'V' || C == 'E' || C == 'C' || C == 't') // 't' added upstream (#266); no sender exists yet in mod or launcher -- taken for merge parity
        Rel = true;
    // Opt-in unreliable (UDP) events (BeamMP#892/#253): lowercase 'e' events stay on UDP even when
    // large -- skip the >1KB compressed-size TCP upgrade for them. 'e' is not in the reliable list
    // above, so it falls through to the UDP branch below (CombinedServerSendUDP in combined mode).
    if (C != 'e' && compressBound(Data.size()) > 1024)
        Rel = true;
    if (Ack || Rel) {
        // Combined host: reliable -> the virtual client's in-memory TCP-in queue (uncompressed,
        // no framing); else the normal loopback/network socket path.
        if (g_CombinedMode)
            CombinedServerSendTCP(Data);
        else if (Ack || DLen > 1000)
            SendLarge(Data);
        else
            TCPSend(Data, TCPSock);
    } else {
        if (g_CombinedMode)
            CombinedServerSendUDP(Data);
        else
            UDPSend(Data);
    }

    if (DLen > 1000) {
        debug("(Launcher->Server) Bytes sent: " + std::to_string(Data.length()) + " : "
            + Data.substr(0, 10)
            + Data.substr(Data.length() - 10));
    } else if (C == 'Z') {
        // debug("(Game->Launcher) : " + Data);
    }
}

void NetReset() {
    TCPTerminate = false;
    GConnected = false;
    Terminate = false;
    UlStatus = "Ulstart";
    MStatus = " ";
    if (UDPSock != (SOCKET)(-1)) {
        debug("Terminating UDP Socket: " + std::to_string(TCPSock));
        KillSocket(UDPSock);
    }
    UDPSock = -1;
    if (TCPSock != (SOCKET)(-1)) {
        debug("Terminating TCP Socket: " + std::to_string(TCPSock));
        KillSocket(TCPSock);
    }
    TCPSock = -1;
    if (GSocket != (SOCKET)(-1)) {
        debug("Terminating GTCP Socket: " + std::to_string(GSocket));
        KillSocket(GSocket);
    }
    GSocket = -1;
    if (DVSock != (SOCKET)(-1)) { // direct vehicle socket (BeamMP-Launcher#245)
        debug("Terminating direct vehicle Socket: " + std::to_string(DVSock));
        KillSocket(DVSock);
    }
    DVSock = -1;
}

SOCKET SetupListener() {
    if (GSocket != -1)
        return GSocket;
    struct addrinfo* result = nullptr;
    struct addrinfo hints { };
    int iRes;
#ifdef _WIN32
    WSADATA wsaData;
    iRes = WSAStartup(514, &wsaData); // 2.2
    if (iRes != 0) {
        error("(Proxy) WSAStartup failed with error: " + std::to_string(iRes));
        return -1;
    }
#endif

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    iRes = getaddrinfo(nullptr, std::to_string(options.port + 1).c_str(), &hints, &result);
    if (iRes != 0) {
        error("(Proxy) info failed with error: " + std::to_string(iRes));
        WSACleanup();
    }
    GSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (GSocket == -1) {
        error("(Proxy) socket failed with error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(result);
        WSACleanup();
        return -1;
    }
    iRes = bind(GSocket, result->ai_addr, (int)result->ai_addrlen);
    if (iRes == SOCKET_ERROR) {
        error("(Proxy) bind failed with error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(result);
        KillSocket(GSocket);
        WSACleanup();
        return -1;
    }
    freeaddrinfo(result);
    iRes = listen(GSocket, SOMAXCONN);
    if (iRes == SOCKET_ERROR) {
        error("(Proxy) listen failed with error: " + std::to_string(WSAGetLastError()));
        KillSocket(GSocket);
        WSACleanup();
        return -1;
    }
    return GSocket;
}
void AutoPing() {
    while (!Terminate) {
        ServerSend("p", false);
        PingStart = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
int ClientID = -1;
void ParserAsync(std::string_view Data) {
    if (Data.empty())
        return;
    // Direct vehicle socket (BeamMP-Launcher#245): per-vehicle data (position/electrics/inputs/
    // powertrain/nodes/controllers) for a vehicle that has a registered direct UDP socket is sent
    // straight to that socket instead of the GE proxy, bypassing the GE Lua VM. Inert until a VE
    // registers (via 'Va' on the core channel) AND its port is learned (DVRcv) -- otherwise every
    // packet falls through to GameSend exactly as before.
    bool tryDirectVehicleSocket = false;
    char Code = Data.at(0), SubCode = 0;
    if (Data.length() > 1)
        SubCode = Data.at(1);
    switch (Code) {
    case 'p':
        PingEnd = std::chrono::high_resolution_clock::now();
        if (PingStart > PingEnd)
            ping = 0;
        else
            ping = int(std::chrono::duration_cast<std::chrono::milliseconds>(PingEnd - PingStart).count());
        return;
    case 'M':
        MStatus = Data;
        UlStatus = "Uldone";
        return;
    case 'U':
        magic = Data.substr(1);
        break;
    case 'R': // controller sync
    case 'W': // electrics
    case 'V': // inputs
    case 'Y': // powertrain
    case 'X': // nodes
    case 'Z': // position
        tryDirectVehicleSocket = true;
        break;
    default:
        break;
    }
    if (tryDirectVehicleSocket) {
        // packet is "<code><sub>:<serverVehicleID>:<data>"; pull the serverVehicleID.
        size_t first = Data.find(':');
        if (first == std::string_view::npos) {
            GameSend(Data);
            return;
        }
        first += 1;
        size_t len = Data.find(':', first);
        if (len != std::string_view::npos) {
            len -= first;
        }
        std::string serverVehicleID = std::string(Data.substr(first, len));
        int dvPort = -1;
        {
            std::scoped_lock lock(DVMapMutex); // copy the port out; DVSend/GameSend run unlocked
            auto portIter = vehiclePortMap.find(serverVehicleID);
            if (portIter != vehiclePortMap.end())
                dvPort = portIter->second;
        }
        if (dvPort >= 0) {
            DVSend(Data, dvPort); // vehicle is connected -> straight to its socket
        } else {
            GameSend(Data); // not connected -> normal GE-proxy path
        }
    } else {
        GameSend(Data);
    }
}
void ServerParser(std::string_view Data) {
    ParserAsync(Data);
}
void NetMain(const std::string& IP, int Port) {
    std::thread Ping(AutoPing);
    Ping.detach();
    UDPClientMain(IP, Port);
    CServer = true;
    Terminate = true;
    info("Connection Terminated!");
}
// Native machine name -- the mod can't read it (BeamNG sandboxes os.getenv), so the
// launcher fills in mp_state.txt's "device:" line during the gather. gethostname() is in
// winsock2.h (Windows, WSAStartup already done) / unistd.h (Linux), both already included.
static std::string LauncherHostName() {
    char buf[256] = { 0 };
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        return std::string(buf);
    }
    return "unknown";
}

// LAN debug helper: bundle the local BeamNG + launcher + (local) server logs, plus the
// mod-generated mp_state.txt, into one zip in the launcher folder. Triggered in-game by
// "/savelogs" (and the log-zip button), which sends "savelogs:<tag>" over the game proxy;
// TCPGameServer intercepts it and runs this on a DETACHED thread so the (potentially
// multi-MB) copy never stalls the game<->server relay. <tag> is a client-supplied timestamp
// used only for the output name, so it is sanitised to filename-safe chars. The logs are
// copied into a folder; on Windows we then shell out to the system tar to turn it into a
// single .zip (no zip lib is linked into the launcher). On failure the folder is kept.
void HandleSaveLogs(std::string tag) {
    try {
        namespace fs = std::filesystem;
        for (char& c : tag) {
            if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) c = '_';
        }
        if (tag.empty()) tag = "manual";

        fs::path userDir = GetGamePath(); // ...\BeamNG.drive\current  (beamng.log lives here)
        fs::path launcherDir = fs::path(GetEP());
        if (launcherDir.filename().empty()) launcherDir = launcherDir.parent_path(); // GetEP() ends with a separator
        // Combined host: start-server.bat launches via `start` with a RELATIVE argv[0], so GetEP()
        // yields no directory and launcherDir is empty. That makes the tar lpCurrentDirectory below an
        // empty string -> CreateProcessW fails with err 123 (ERROR_INVALID_NAME), leaving a loose folder.
        // Anchor to the absolute process cwd (the server folder) so all paths below are absolute.
        if (launcherDir.empty()) { std::error_code cpec; launcherDir = fs::current_path(cpec); }

        std::string folderName = "BeamMP_logs_" + tag;
        fs::path outDir = launcherDir / folderName;
        std::error_code mkec;
        fs::create_directories(outDir, mkec);
        int n = 0;
        auto add = [&](const fs::path& p, const std::string& arc) {
            std::error_code ec;
            if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                fs::copy_file(p, outDir / arc, fs::copy_options::overwrite_existing, ec);
                if (!ec) ++n;
            }
        };

        // BeamNG game logs (+ rotations) and BeamNG's own launcher log
        add(userDir / "beamng.log", "beamng.log");
        for (int i = 1; i <= 9; ++i) {
            std::string nm = "beamng." + std::to_string(i) + ".log";
            add(userDir / nm, nm);
        }
        add(userDir / "beamng-launcher.log", "beamng-launcher.log");
        for (int i = 1; i <= 3; ++i) {
            std::string nm = "beamng-launcher." + std::to_string(i) + ".log";
            add(userDir / nm, nm);
        }
        // mod-generated MP-state snapshot (written by the mod just before triggering). The
        // mod writes "device: unknown" (it can't read the host name); patch that to the real
        // hostname here, then write the result into the bundle.
        {
            std::error_code mec;
            fs::path mps = userDir / "BeamMP_logs" / "mp_state.txt";
            if (fs::exists(mps, mec) && fs::is_regular_file(mps, mec)) {
                std::ifstream in(mps, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close();
                const std::string needle = "device: unknown";
                size_t p = content.find(needle);
                if (p != std::string::npos) {
                    content.replace(p, needle.size(), "device: " + LauncherHostName());
                }
                std::ofstream out(outDir / "mp_state.txt", std::ios::binary | std::ios::trunc);
                out << content;
                out.close();
                ++n;
            }
        }
        // the BeamMP launcher's own log
        add(launcherDir / "Launcher.log", "Launcher.log");
        add(launcherDir / "Launcher.log.1", "Launcher.log.1");

        // Server log -- only "if on that system": probe the usual sibling/local spots.
        fs::path base = launcherDir.parent_path();
        for (const fs::path& sd : { base / "BeamMP Server", base / "BeamMP-Server", launcherDir }) {
            std::error_code ec;
            if (fs::exists(sd / "Server.log", ec)) {
                add(sd / "Server.log", "Server.log");
                add(sd / "Server.old.log", "Server.old.log");
                // newest server_state*.txt dump, if the host ran the server's savelogs command
                fs::path newest;
                fs::file_time_type best {};
                for (auto& de : fs::directory_iterator(sd, ec)) {
                    auto fn = de.path().filename().string();
                    if (fn.rfind("server_state", 0) == 0 && de.path().extension() == ".txt") {
                        auto t = de.last_write_time(ec);
                        if (newest.empty() || t > best) {
                            newest = de.path();
                            best = t;
                        }
                    }
                }
                if (!newest.empty()) add(newest, "server_state.txt");
                break;
            }
        }

#if defined(_WIN32)
        // Turn the folder into a single .zip via the system tar (Windows 10+ ships bsdtar,
        // which writes real zips with --format zip). Run from the launcher dir with
        // space-free relative names so no command-line quoting is needed; the spaces in the
        // launcher path live only in lpCurrentDirectory, which needs no quoting.
        {
            std::string zipName = folderName + ".zip";
            // Use the FULL path to the system tar (Windows 10+ bsdtar), not a bare "tar.exe": a
            // different tar earlier in PATH (Git/MSYS GNU tar) doesn't support --format zip, and the
            // combined host -- launched via start-server.bat through `start` -- may not surface
            // System32 in PATH the same way the standalone launcher does (this was leaving an
            // un-zipped folder). System32 has no spaces, so no extra quoting is needed.
            wchar_t sysDir[MAX_PATH] = { 0 };
            UINT slen = GetSystemDirectoryW(sysDir, MAX_PATH);
            std::wstring tarPath = (slen > 0 ? std::wstring(sysDir) : std::wstring(L"C:\\Windows\\System32")) + L"\\tar.exe";
            std::wstring cmd = L"\"" + tarPath + L"\" --format zip -c -f "
                + std::wstring(zipName.begin(), zipName.end()) + L" "
                + std::wstring(folderName.begin(), folderName.end());
            std::wstring cwd = launcherDir.wstring();
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');
            STARTUPINFOW si {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi {};
            // lpApplicationName = the exact tar path -> no PATH search at all.
            if (CreateProcessW(tarPath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 60000);
                DWORD tarExit = 1;
                GetExitCodeProcess(pi.hProcess, &tarExit);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                std::error_code ze;
                if (fs::exists(launcherDir / zipName, ze)) {
                    fs::remove_all(outDir, ze); // zipped OK -> drop the loose folder
                    info("Saved " + std::to_string(n) + " log file(s) to " + (launcherDir / zipName).string());
                    return;
                }
                warn("savelogs: tar (" + std::string(tarPath.begin(), tarPath.end()) + ") exited " + std::to_string(tarExit)
                    + " but produced no zip -- keeping the loose folder " + outDir.string());
            } else {
                warn("savelogs: could not launch tar (err " + std::to_string(GetLastError()) + ") -- keeping the loose folder " + outDir.string());
            }
        }
#endif
        info("Saved " + std::to_string(n) + " log file(s) to " + outDir.string());
    } catch (const std::exception& e) {
        error(std::string("savelogs failed: ") + e.what());
    }
}

void TCPGameServer(const std::string& IP, int Port) {
    GSocket = SetupListener();
    std::unique_ptr<std::thread> ClientThread {};
    std::unique_ptr<std::thread> NetMainThread {};
    std::unique_ptr<std::thread> DirectVehicleThread {}; // BeamMP-Launcher#245
    while (!TCPTerminate && GSocket != -1) {
        debug("MAIN LOOP OF GAME SERVER");
        GConnected = false;
        if (!CServer) {
            warn("Connection still alive terminating");
            NetReset();
            TCPTerminate = true;
            Terminate = true;
            break;
        }
        if (CServer) {
            if (g_CombinedMode) {
                // Combined host: create the in-process server's host client + the UDP feed BEFORE the
                // client runs, so the version/key handshake TCPClientMain sends over the in-memory
                // link is received by the virtual client's auth. Blocks only until the server is ready.
                StartCombinedBridge();
            }
            // TCPClientMain drives the version/key/mod handshake + the server->game read loop. In
            // combined it runs over the in-memory link (no socket); otherwise over TCPSock.
            ClientThread = std::make_unique<std::thread>(TCPClientMain, IP, Port);
        }
        CSocket = accept(GSocket, nullptr, nullptr);
        if (CSocket == -1) {
            debug("(Proxy) accept failed with error: " + std::to_string(WSAGetLastError()));
            break;
        }
        // LAN-only build: this localhost socket relays *all* game traffic to the
        // launcher, so disabling Nagle's algorithm here removes coalescing delay
        // from the outbound path (including the high-rate position/node streams).
        {
            int NoDelay = 1;
            if (setsockopt(CSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&NoDelay, sizeof(NoDelay)) != 0) {
                debug("Failed to set TCP_NODELAY on game proxy socket: " + std::to_string(WSAGetLastError()));
            }
        }
        debug("(Proxy) Game Connected!");
        GConnected = true;
        if (CServer) {
            // NetMain runs AutoPing + UDPClientMain in BOTH modes; in combined those use the link.
            NetMainThread = std::make_unique<std::thread>(NetMain, IP, Port);
            // Direct vehicle socket (BeamMP-Launcher#245): bind a UDP socket on port+2 that VEs send
            // their per-vehicle data straight to (bypassing the GE VM). Its DVRcv forwards to the
            // server via ServerSend, which is already combined-host aware (in-memory bridge), so this
            // needs no combined-mode special-casing. Inert until a VE registers + connects.
            DirectVehicleThread = std::make_unique<std::thread>(DVClientMain, "127.0.0.1", options.port + 2);
            CServer = false;
        }
        int32_t Size, Rcv;
        int Temp;
        char Header[10] = { 0 };
        std::vector<char> data {};

        // Read byte by byte until '>' is rcved then get the size and read based on it
        do {
            try {
                Utils::ReceiveFromGame(CSocket, data);
                std::string pkt(data.data(), data.size());
                // "/savelogs" in-game sends this launcher-local command; gather logs on a
                // detached thread and do NOT forward it to the server.
                if (pkt.rfind("savelogs:", 0) == 0) {
                    std::thread(HandleSaveLogs, pkt.substr(9)).detach();
                } else {
                    ServerSend(pkt, false);
                }
            } catch (const std::exception& e) {
                error(std::string("Error while receiving from game on proxy: ") + e.what());
                break;
            }
        } while (!TCPTerminate);
        debug("(Proxy) Connection closing");
        // Mark the proxy socket invalid so relay sends (GameSend) on other
        // threads bail out instead of writing to a stale handle (avoids 10038).
        GConnected = false;
        SOCKET Old = CSocket;
        CSocket = (SOCKET)-1;
        if (Old != (SOCKET)-1 && Old != SOCKET_ERROR) {
            KillSocket(Old);
        }
    }
    TCPTerminate = true;
    GConnected = false;
    Terminate = true;
    if (g_CombinedMode) {
        // Close the in-memory link BEFORE joining the client/net threads: in combined they block in
        // TCPRcv/UDPRcv on the link (not on a socket), so without this the joins below would deadlock.
        // This also drops the host client so other players see the leave, and resets the bridge so the
        // next join (a fresh TCPGameServer from StartSync) builds a new client + handshake.
        StopCombinedBridge();
    }
    if (ClientThread) {
        debug("Waiting for client thread");
        ClientThread->join();
        debug("Client thread done");
    }
    if (NetMainThread) {
        debug("Waiting for net main thread");
        NetMainThread->join();
        debug("Net main thread done");
    }
    if (DirectVehicleThread) { // BeamMP-Launcher#245: DVClientMain exits its recv loop on TCPTerminate
        debug("Waiting for direct vehicle thread");
        DirectVehicleThread->join();
        debug("Direct vehicle thread done");
    }
    if (CSocket != SOCKET_ERROR)
        KillSocket(CSocket);
    debug("END OF GAME SERVER");
}
