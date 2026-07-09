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
#include <string>

SOCKET DVSock = -1;
static sockaddr_in ToVehicle;
std::unordered_set<std::string> activeVehicles;
std::unordered_map<std::string, int> vehiclePortMap;

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
    ToVehicle.sin_port = htons(uint16_t(Port));
    int sendOk = sendto(DVSock, Data.data(), int(Data.size()), 0, (sockaddr*)&ToVehicle, sizeof(ToVehicle));
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
    if (!activeVehicles.contains(serverVehicleID)) {
        debug("(Direct VE) Received data from unregistered vehicle: " + serverVehicleID);
        return;
    }
    int port = ntohs(FromVehicle.sin_port);
    auto portIter = vehiclePortMap.find(serverVehicleID);
    if (portIter != vehiclePortMap.end()) {
        if (portIter->second == port) {
            ServerSend(std::move(Data), false); // unreliable (UDP), same as the proxy position path
        } else {
            debug("(Direct VE) Data for " + serverVehicleID + " from wrong port: " + std::to_string(port) + " != " + std::to_string(portIter->second));
        }
    } else {
        debug("(Direct VE) Registering port for vehicle " + serverVehicleID + ": " + std::to_string(port));
        vehiclePortMap.insert({ serverVehicleID, port });
        ServerSend(std::move(Data), false);
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
    activeVehicles.clear();
    vehiclePortMap.clear();
}
