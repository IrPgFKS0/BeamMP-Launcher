/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "Network/network.hpp"
#include "Zlib/Compressor.h"
#include <stdexcept>
#include <thread> // upstream #268: paced UDP registration burst

#if defined(_WIN32)
#include <ws2tcpip.h>
#elif defined(__linux__)
#include "linuxfixes.h"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h> // fcntl O_NONBLOCK for the latest-wins drain
#endif

#include "Logger.h"
#include "CombinedHost.h" // --combined: g_CombinedMode + in-memory link send/recv
#include <array>
#include <future>          // upstream #269: async magic registration burst
#include <mutex>           // upstream #269: UDPSendMutex (burst thread vs game/direct-socket senders)
#include <string>
#include <vector>          // latest-wins drain batch
#include <unordered_map>   // latest-wins dedup (vehicle key -> newest index)

SOCKET UDPSock = -1;
sockaddr_in* ToServer = nullptr;

std::mutex UDPSendMutex; // upstream #269: the async magic burst sends concurrently with the game/direct-socket senders

void UDPSend(std::string Data) {
    std::scoped_lock lock(UDPSendMutex);
    if (g_CombinedMode) {
        // In-memory link: hand the raw payload to the in-process server. No ClientID:/prefix and no
        // compression -- HandleVirtualUDP already knows the host client and feeds GlobalParser directly.
        CombinedServerSendUDP(Data);
        return;
    }
    if (ClientID == -1 || UDPSock == -1)
        return;
    if (Data.length() > 400) {
        auto res = Comp(std::span<char>(Data.data(), Data.size()));
        Data = "ABG:" + std::string(res.data(), res.size());
    }
    std::string Packet = char(ClientID + 1) + std::string(":") + Data;
    int sendOk = sendto(UDPSock, Packet.c_str(), int(Packet.size()), 0, (sockaddr*)ToServer, sizeof(*ToServer));
    if (sendOk == SOCKET_ERROR)
        error("Error Code : " + std::to_string(WSAGetLastError()));
}

void SendLarge(std::string Data) {
    if (Data.length() > 400) {
        auto res = Comp(std::span<char>(Data.data(), Data.size()));
        Data = "ABG:" + std::string(res.data(), res.size());
    }
    TCPSend(Data, TCPSock);
}

void UDPParser(std::string_view Packet) {
    if (Packet.substr(0, 4) == "ABG:") {
        auto substr = Packet.substr(4);
        try {
            auto res = DeComp(std::span<const char>(substr.data(), substr.size()));
            std::string DeCompPacket = std::string(res.data(), res.size());
            ServerParser(DeCompPacket);
        } catch (const std::runtime_error& err) {
            error("Error in decompression of UDP, ignoring");
        }
    } else {
        ServerParser(Packet);
    }
}
// --- latest-wins drain helpers (regular client) ---------------------------------------------------
// Toggle the UDP socket's blocking mode so we can drain a backlog non-blocking. Returns false if the
// toggle failed -> the caller must NOT enter the drain loop (a blocking recvfrom there would hang).
static bool SetUDPNonBlocking(SOCKET s, bool nb) {
#if defined(_WIN32)
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1)
        return false;
    return fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}
// For a position packet "Zp:<pid>-<vid>:<json>" return the "<pid>-<vid>" vehicle key; empty for any
// other packet (events, pings, compressed ABG: blobs) which must NEVER be deduped/dropped.
static std::string_view ZpVehKey(std::string_view p) {
    if (p.size() < 5 || p[0] != 'Z' || p[1] != 'p' || p[2] != ':')
        return {};
    auto c = p.find(':', 3);
    if (c == std::string_view::npos)
        return {};
    return p.substr(3, c - 3);
}

void UDPRcv() {
    if (g_CombinedMode) {
        // In-memory link: block for one server->client UDP message. Empty => link closed; stop the
        // recv loop (otherwise CombinedClientRecvUDP would return instantly and busy-spin).
        std::string Msg = CombinedClientRecvUDP();
        if (Msg.empty()) {
            Terminate = true;
            return;
        }
        UDPParser(std::string_view(Msg.data(), Msg.size()));
        return;
    }
    sockaddr_in FromServer {};
#if defined(_WIN32)
    int clientLength = sizeof(FromServer);
#elif defined(__linux__)
    socklen_t clientLength = sizeof(FromServer);
#endif
    ZeroMemory(&FromServer, clientLength);
    static thread_local std::array<char, 10240> Ret {};
    if (UDPSock == -1)
        return;
    int32_t Rcv = recvfrom(UDPSock, Ret.data(), Ret.size() - 1, 0, (sockaddr*)&FromServer, &clientLength);
    if (Rcv == SOCKET_ERROR)
        return;

    // LATEST-WINS DRAIN. When the game stalls (a frame hitch), GameSend (the TCP proxy to BeamNG)
    // blocks, so this UDP socket backlogs; replaying that stale backlog makes remote ghosts lag/jump
    // after the hitch. Drain the whole backlog non-blocking and forward only the NEWEST position per
    // vehicle ("Zp:<pid>-<vid>:..."); every other packet (events/pings/ABG: blobs) is forwarded
    // unchanged, in order. (Combined host returned above -- its in-memory bridge already coalesces.)
    static thread_local std::vector<std::string> batch;
    batch.clear();
    batch.emplace_back(Ret.data(), Rcv);
    if (SetUDPNonBlocking(UDPSock, true)) {
        for (int i = 0; i < 512; ++i) { // cap the drain so a flood can't spin here forever
            int32_t r = recvfrom(UDPSock, Ret.data(), Ret.size() - 1, 0, (sockaddr*)&FromServer, &clientLength);
            if (r <= 0)
                break;
            batch.emplace_back(Ret.data(), r);
        }
        SetUDPNonBlocking(UDPSock, false);
    }
    if (batch.size() == 1) { // no backlog (the common case) -- forward as-is, no dedup overhead
        UDPParser(batch[0]);
        return;
    }
    // newest index per vehicle key (string_views point into the now-stable batch)
    static thread_local std::unordered_map<std::string_view, size_t> lastIdx;
    lastIdx.clear();
    for (size_t i = 0; i < batch.size(); ++i) {
        auto k = ZpVehKey(batch[i]);
        if (!k.empty())
            lastIdx[k] = i;
    }
    size_t dropped = 0;
    for (size_t i = 0; i < batch.size(); ++i) {
        auto k = ZpVehKey(batch[i]);
        if (!k.empty() && lastIdx[k] != i) {
            ++dropped; // an older position for this vehicle, superseded later in the batch -> drop it
            continue;
        }
        UDPParser(batch[i]);
    }
    if (dropped) // visible only with --debug; lets you confirm the drain fires under a receive backlog
        debug("latest-wins drain: coalesced " + std::to_string(dropped) + " stale position(s) (backlog " + std::to_string(batch.size()) + ")");
}
void UDPClientMain(const std::string& IP, int Port) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(514, &data)) {
        error("Can't start Winsock!");
        return;
    }
#endif

    delete ToServer;
    ToServer = new sockaddr_in;
    ToServer->sin_family = AF_INET;
    ToServer->sin_port = htons(Port);
    inet_pton(AF_INET, IP.c_str(), &ToServer->sin_addr);
    UDPSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (UDPSock == -1) { // matches the existing UDPSock validity checks; INVALID_SOCKET isn't defined on Linux
        // #2: without this the UDP session dies silently -- UDPSend() just no-ops on an invalid
        // socket and nobody ever learns positions stopped flowing.
        error("Failed to create UDP socket: " + std::to_string(
#ifdef _WIN32
            WSAGetLastError()
#else
            errno
#endif
            ));
#ifdef _WIN32
        WSACleanup(); // balance the WSAStartup at the top of UDPClientMain
#endif
        return;
    }
    // LAN: bump the UDP receive buffer so a burst of position packets (many cars x physicsRateSend
    // x every player, relayed by the server) doesn't overflow the OS default (~64-256KB) and get
    // silently dropped -- the cause of remote-car drift under load. 8MB holds thousands of the
    // small position packets. The OS may cap this (Linux net.core.rmem_max) -- see the LAN docs.
    if (!g_CombinedMode) {
        // Combined host: this UDP socket is unused (the in-memory bridge carries UDP), so its receive
        // buffer has no effect -- skip it so it isn't misleading. The SERVER's own receive buffer
        // (for LAN2's real network UDP) still matters and is set server-side. See LAN-TUNING.md.
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(UDPSock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    }
    // The magic UDP registration teaches the server which UDP endpoint maps to this client. In
    // combined mode there is no endpoint -- the in-process server already has the host client bound
    // to the in-memory link (HandleVirtualUDP) -- so skip it (otherwise we'd push magic packets the
    // server would mis-parse). The UDP socket created above is left unused; the bridge handles I/O.
    std::future<void> magicSend;
    if (!g_CombinedMode && !magic.empty()) {
        // upstream #268 spaced the 10-packet burst out so the server's registration handling can't
        // miss the whole volley; upstream #269 then moved it off the connect path -- the burst now
        // runs on its own thread (50 ms spacing, ~500 ms total) so the P/H handshake below is no
        // longer held up by it. Joined after the receive loop, before the socket is killed.
        magicSend = std::async(std::launch::async, []() {
            for (int i = 0; i < 10; i++) {
                UDPSend(magic);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    GameSend("P" + std::to_string(ClientID));
    TCPSend("H", TCPSock);
    UDPSend("p");
    debug("Starting UDP receive loop");
    while (!Terminate) {
        UDPRcv();
    }
    debug("UDP receive loop done");
    if (magicSend.valid()) {
        magicSend.get(); // #269: never kill the socket under the burst thread
    }
    KillSocket(UDPSock);
    WSACleanup();
}
