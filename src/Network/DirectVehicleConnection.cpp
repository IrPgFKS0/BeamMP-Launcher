/*
 Copyright (C) 2026 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

// Direct vehicle socket (port of BeamMP-Launcher#245, stefan750). A UDP socket on port+2 that each
// VE Lua instance sends its per-vehicle data straight to, bypassing the VE->GE Lua queue + the GE
// proxy. DVRcv validates the sender against the registered set (activeVehicles, filled by 'Va' on
// the core channel), learns the VE's source port on its first packet, and forwards to the server via
// ServerSend -- which is already combined-host aware, so no combined special-casing is needed.
// Return traffic for a connected vehicle is redirected here from ParserAsync (see GlobalHandler.cpp).
// INERT until a VE registers + sends: nothing changes for the existing GE-proxy path.
//
// Fork adaptations vs the upstream PR: use `-1` instead of INVALID_SOCKET (the fork's Linux build
// doesn't define it, see VehicleData.cpp), and DVSock matches the fork's uint64_t socket convention.

#include "Network/network.hpp"
#include <stdexcept>

#if defined(_WIN32)
#include <ws2tcpip.h>
#elif defined(__linux__)
#include "linuxfixes.h"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "Logger.h"
#include <array>
#include <mutex>
#include <string>

SOCKET DVSock = -1;
static sockaddr_in ToVehicle; // template only (family + address); the port is stamped on a per-call COPY in DVSend
std::unordered_set<std::string> activeVehicles;
std::unordered_map<std::string, int> vehiclePortMap;
// Both containers are touched from FOUR threads (Core thread Va/Vd insert/erase, this DV thread's
// find/insert/clear, and the TCP + UDP receive threads' find in ParserAsync). An unordered_map
// rehash or erase racing a find is UB on both STLs -- a rare, unexplained crash that on the
// combined host is the whole session. Hold this for every access; never across a send.
std::mutex DVMapMutex;

// Pull the "<serverVehicleID>" from a "<code>:<serverVehicleID>:<data>" packet. Empty on malformed.
static std::string_view ExtractServerVehicleID(std::string_view Data) {
    size_t first = Data.find(':');
    if (first == std::string_view::npos)
        return {};
    first += 1;
    size_t len = Data.find(':', first);
    if (len != std::string_view::npos)
        len -= first;
    return Data.substr(first, len);
}

void DVSend(std::string_view Data, int Port) {
    if (DVSock == (SOCKET)-1)
        return;
    // Per-call COPY, never the shared ToVehicle: DVSend is reached from BOTH receive threads
    // (TCPClientMain and NetMain, via ServerParser -> ParserAsync). Mutating one module-scope
    // sockaddr's port and then sending meant thread A could stamp port X, thread B overwrite it
    // with Y, and A's packet leave for Y -- a position/input packet delivered to the WRONG
    // vehicle's socket. The h39 DVMapMutex covered the registries but not this.
    sockaddr_in Dest = ToVehicle;
    Dest.sin_port = htons(uint16_t(Port));
    int sendOk = sendto(DVSock, Data.data(), int(Data.size()), 0, (sockaddr*)&Dest, sizeof(Dest));
    if (sendOk == SOCKET_ERROR)
        error("(Direct VE) Failed to send data. Error Code : " + std::to_string(WSAGetLastError()));
}

static void DVRcv() {
    sockaddr_in FromVehicle;
    socklen_t size = sizeof(FromVehicle);
    ZeroMemory(&FromVehicle, size);
    static thread_local std::array<char, 10240> Ret {};
    if (DVSock == (SOCKET)-1)
        return;
    int32_t Rcv = recvfrom(DVSock, Ret.data(), Ret.size() - 1, 0, (sockaddr*)&FromVehicle, &size);
    if (Rcv == SOCKET_ERROR)
        return;

    std::string Data(Ret.data(), Rcv);
    std::string_view sidView = ExtractServerVehicleID(Data);
    if (sidView.empty()) {
        debug("(Direct VE) Failed to parse serverVehicleID from data: " + Data);
        return;
    }
    std::string serverVehicleID(sidView);
    int port = ntohs(FromVehicle.sin_port);
    // Reliability by payload type: position/inputs ('Z'/'V') stay unreliable (latest-wins), but
    // chunked deformation ('Xd') MUST arrive complete -- and on the combined host the unreliable
    // path is a 16-deep drop-oldest queue that a multi-vehicle chunk burst overflows instantly,
    // evicting chunks (no snapshot ever assembles) AND the position packets sharing the queue
    // (ghost starvation -- seen as the 2026-07-09 LAN2 watchdog storm). Route the whole 'X' family
    // reliable; everything else keeps the fast lossy lane.
    const bool rel = !Data.empty() && Data.at(0) == 'X';
    // Decide under the lock, act outside it (ServerSend takes its own locks; never nest).
    enum { Unregistered, KnownPort, WrongPort, NewPort } verdict;
    int knownPort = -1;
    {
        std::scoped_lock lock(DVMapMutex);
        if (!activeVehicles.contains(serverVehicleID)) {
            verdict = Unregistered;
        } else {
            auto portIter = vehiclePortMap.find(serverVehicleID);
            if (portIter != vehiclePortMap.end()) {
                knownPort = portIter->second;
                verdict = (knownPort == port) ? KnownPort : WrongPort;
            } else {
                vehiclePortMap.insert({ serverVehicleID, port });
                verdict = NewPort;
            }
        }
    }
    switch (verdict) {
    case Unregistered:
        debug("(Direct VE) Received data from unregistered vehicle: " + serverVehicleID);
        return;
    case WrongPort:
        debug("(Direct VE) Data for " + serverVehicleID + " from wrong port: " + std::to_string(port) + " != " + std::to_string(knownPort));
        return;
    case KnownPort: {
        // Periodic ack keepalive: covers a VE that missed the registration ack (UDP). Each
        // active vehicle sends ~36 msg/s, so every 32nd packet ~= one ack/s to that sender.
        static uint32_t ackCounter = 0; // DVRcv runs only on the DVClientMain thread
        if ((++ackCounter & 31u) == 0u)
            sendto(DVSock, "ok", 2, 0, (sockaddr*)&FromVehicle, size);
        ServerSend(std::move(Data), rel);
        return;
    }
    case NewPort:
        debug("(Direct VE) Registering port for vehicle " + serverVehicleID + ": " + std::to_string(port));
        // Ack the registration straight back to the VE's socket. The VE treats ANY datagram on its
        // socket as proof a listening launcher owns this port and only then abandons the GE path --
        // without this, an OLD launcher (no direct socket) let sends vanish into the void silently
        // (LAN2 2026-07-09: car frozen for everyone else while its sends reported success).
        sendto(DVSock, "ok", 2, 0, (sockaddr*)&FromVehicle, size);
        ServerSend(std::move(Data), rel);
        return;
    }
}

void DVClientMain(const std::string& IP, int Port) {
    debug("(Direct VE) Starting direct vehicle socket on " + IP + ":" + std::to_string(Port));

#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(514, &data)) {
        error("(Direct VE) Can't start Winsock!");
        return;
    }
#endif
    sockaddr_in DVListenAddr;
    ZeroMemory(&DVListenAddr, sizeof(DVListenAddr));
    DVListenAddr.sin_family = AF_INET;
    DVListenAddr.sin_port = htons(uint16_t(Port));
    inet_pton(AF_INET, IP.c_str(), &DVListenAddr.sin_addr);

    ZeroMemory(&ToVehicle, sizeof(ToVehicle));
    ToVehicle.sin_family = AF_INET;
    ToVehicle.sin_addr = DVListenAddr.sin_addr;

    DVSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (DVSock == (SOCKET)-1) {
        error("(Direct VE) Socket creation failed with error: " + std::to_string(WSAGetLastError()));
        WSACleanup();
        return;
    }
    if (bind(DVSock, (const sockaddr*)&DVListenAddr, sizeof(DVListenAddr)) == SOCKET_ERROR) {
        error("(Direct VE) Socket bind failed with error: " + std::to_string(WSAGetLastError()));
        KillSocket(DVSock);
        DVSock = -1;
        WSACleanup();
        return;
    }
    debug("(Direct VE) Starting direct vehicle receive loop");
    while (!TCPTerminate) {
        DVRcv();
    }
    debug("(Direct VE) Direct vehicle receive loop done");
    KillSocket(DVSock);
    DVSock = -1;
    WSACleanup();
    {
        std::scoped_lock lock(DVMapMutex); // a late Vd on the Core thread can race this teardown
        activeVehicles.clear();
        vehiclePortMap.clear();
    }
}
