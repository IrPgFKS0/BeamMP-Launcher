/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "CombinedHost.h"
#include "Logger.h"

#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

std::atomic<bool> g_CombinedMode { false };

void CombinedDbg(const char* msg) {
    // Combined-host startup trace. Goes to Launcher.log (and the console with --verbose) so the
    // path is diagnosable if --combined ever regresses, without leaving a stray file behind.
    debug(std::string("[combined] ") + msg);
}

#ifdef BEAMMP_EMBED_SERVER

// The server/boost headers below bring a conflicting `error` (the boost::asio::error namespace)
// into scope, which makes the launcher's global error() ambiguous (C2872). Capture it here, BEFORE
// those includes, behind a distinct name. info()/debug() don't clash, so they're used directly.
namespace {
inline void LogErr(const std::string& s) { error(s); }
}

#include "Client.h" // TClient, InMemoryLink
#include "Common.h" // Application::SetEmbedded
#include "ServerRuntime.h" // BeamMPServerMain, SetServerReadyHook, MainArguments
#include "TNetwork.h" // TNetwork::AddVirtualClient / HandleVirtualUDP

#include <memory>
#include <mutex>

namespace {
// gLinkMtx guards gHostClient. The host's virtual client (and the InMemoryLink it owns) can be
// REPLACED on a leave->rejoin, so every accessor copies the shared_ptr under this lock and derives
// the link from that copy -- the copy keeps the client+link alive for the whole call, so a slow
// reader from the OLD session can never deref a freed link (the leave/rejoin use-after-free). The
// lock is only ever held briefly (a pointer copy/assign); blocking condvar waits use the link's OWN
// mutex, never this one. gHostClient being non-null IS the "bridge active" state (no separate flag,
// which is what let a fast rejoin wedge before).
std::mutex gLinkMtx;
std::shared_ptr<TClient> gHostClient;       // the virtual client (guarded by gLinkMtx)
TNetwork* gHostNetwork = nullptr;           // set once in the ready hook (gated by gServerReady), then stable
std::atomic<bool> gServerReady { false };   // ready hook fired (gHostNetwork usable); gates client creation

constexpr size_t kMaxUDPQueue = 16; // bound (~0.27s at the 60Hz ceiling): drop-oldest so a stall sheds STALE positions instead of hoarding latency. Was 256 (~4s of hidden lag).

// Copy the host client under the lock. Callers hold the returned shared_ptr for their whole call,
// which keeps the InMemoryLink alive even if StopCombinedBridge() resets gHostClient meanwhile.
std::shared_ptr<TClient> HostClient() {
    std::lock_guard<std::mutex> Lk(gLinkMtx);
    return gHostClient;
}
}

void StartEmbeddedServer(int argc, const char** argv) {
    CombinedDbg("StartEmbeddedServer: entered");
    // Run the in-process server HEADLESS: no interactive console, no stdin reader, no stdout writes
    // (the launcher owns the terminal in wide/_O_U8TEXT mode), no signal handlers, no std::exit on
    // shutdown. Server output goes to Server.log. MUST be set before the server thread starts.
    Application::SetEmbedded(true);
    // The hook fires on the server thread once TServer + TNetwork are wired (before the run loop).
    SetServerReadyHook([](TServer& /*Server*/, TNetwork& Network) {
        // Do NOT create the host's virtual client here. The ready hook fires ~1s into startup, but
        // the game (BeamNG + 17GB of mods) takes far longer to load and only connects to the proxy
        // when the user clicks "join". If the client were created now, its auth would wait 10s for a
        // handshake from a game that hasn't connected yet and time out ("Connection closed during
        // version handshake"). Instead just record the network; StartCombinedBridge() creates the
        // client at game-connect, so auth waits on an already-connected game.
        gHostNetwork = &Network;
        gServerReady.store(true);
        info("Combined host: in-process server ready; host client will attach when the game connects.");
    });

    MainArguments Args {};
    Args.argc = argc;
    Args.argv = const_cast<char**>(argv);
    Args.InvokedAs = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a != "--combined") {
            Args.List.emplace_back(argv[i]);
        }
    }
    std::thread([Args = std::move(Args)]() mutable {
        CombinedDbg("server thread: about to call BeamMPServerMain (needs ServerConfig.toml in CWD; port must be free)");
        try {
            int rc = BeamMPServerMain(std::move(Args));
            CombinedDbg("server thread: BeamMPServerMain RETURNED (embedded server stopped)");
            (void)rc;
        } catch (const std::exception& e) {
            CombinedDbg("server thread: BeamMPServerMain THREW std::exception");
            LogErr(std::string("Combined host: embedded server exited with exception: ") + e.what());
        } catch (...) {
            CombinedDbg("server thread: BeamMPServerMain THREW a non-std exception");
        }
    }).detach();
    CombinedDbg("StartEmbeddedServer: server thread detached, returning to launcher");
}

void CombinedServerSendTCP(const std::string& Data) {
    auto Client = HostClient();
    if (!Client) {
        return;
    }
    InMemoryLink* Link = Client->Link(); // kept alive by `Client` for this scope
    if (Link == nullptr) {
        return;
    }
    size_t Depth;
    {
        std::lock_guard<std::mutex> Lk(Link->FromClientMtx);
        Link->FromClientTCP.emplace(Data.begin(), Data.end());
        Depth = Link->FromClientTCP.size();
    }
    Link->FromClientCv.notify_all(); // wakes both the server's TCPRcv and the UDP drain
    // Reliable/event queue: NEVER drop (unlike the latest-wins UDP queues) -- but warn if it backs up
    // past a generous cap, which means the in-process server's virtual-client read has stalled. Logs
    // once per crossing (== cap) to avoid spam.
    constexpr size_t kTcpBacklogWarn = 4096;
    if (Depth == kTcpBacklogWarn) {
        warn("Combined host: launcher->server event queue backlog (" + std::to_string(Depth) + ") -- server read may be stalled.");
    }
}

void CombinedServerSendUDP(const std::string& Data) {
    auto Client = HostClient();
    if (!Client) {
        return;
    }
    InMemoryLink* Link = Client->Link(); // kept alive by `Client` for this scope
    if (Link == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> Lk(Link->FromClientMtx);
        while (Link->FromClientUDP.size() >= kMaxUDPQueue) {
            Link->FromClientUDP.pop(); // drop-oldest (positions are latest-wins)
        }
        Link->FromClientUDP.emplace(Data.begin(), Data.end());
    }
    Link->FromClientCv.notify_all();
}

void StartCombinedBridge() {
    // Called from TCPGameServer the moment the game connects. Wait for the embedded server to finish
    // wiring (ready hook -> gHostNetwork) -- done WITHOUT the lock so a concurrent StopCombinedBridge
    // is never blocked behind this ~30s wait.
    for (int i = 0; i < 600 && !gServerReady.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!gServerReady.load() || gHostNetwork == nullptr) {
        LogErr("Combined host: embedded server never became ready; in-memory bridge NOT started.");
        return;
    }

    // Create the host's virtual client and publish it under the lock. AddVirtualClient() is
    // non-blocking (it spawns a detached auth thread and returns), so the brief lock hold is safe.
    // If a bridge is already active (a duplicate/overlapping Start on a fast rejoin) keep the
    // existing client and bail -- never register two virtual clients on the server.
    std::shared_ptr<TClient> Client;
    {
        std::lock_guard<std::mutex> Lk(gLinkMtx);
        if (gHostClient) {
            return; // already attached for this session
        }
        try {
            gHostClient = gHostNetwork->AddVirtualClient();
        } catch (const std::exception& e) {
            LogErr(std::string("Combined host: AddVirtualClient failed: ") + e.what());
            return;
        }
        Client = gHostClient;
    }
    info("Combined host: host client attached to the in-process server over the in-memory link.");

    // client -> server UDP: the in-process server has no socket recv for the virtual client, so drain
    // its UDP-in queue here and feed it through the normal parser via HandleVirtualUDP. (TCP-in is
    // read by the server's own virtual-client TCPRcv; server->client TCP+UDP are read by the launcher's
    // bridge-aware TCPRcv/UDPRcv in TCPClientMain/UDPClientMain -- so there is no server->game drain.)
    // Captures `Client` by value, keeping the client+link alive for the whole drain even across a
    // leave/rejoin that replaces gHostClient.
    std::thread([Client]() {
        InMemoryLink* Link = Client->Link();
        while (!Link->Closed.load()) {
            std::vector<uint8_t> Msg;
            {
                std::unique_lock<std::mutex> Lk(Link->FromClientMtx);
                Link->FromClientCv.wait(Lk, [&] {
                    return !Link->FromClientUDP.empty() || Link->Closed.load();
                });
                if (Link->FromClientUDP.empty()) {
                    continue;
                }
                Msg = std::move(Link->FromClientUDP.front());
                Link->FromClientUDP.pop();
            }
            if (gHostNetwork != nullptr) {
                gHostNetwork->HandleVirtualUDP(Client, std::move(Msg));
            }
        }
    }).detach();

    info("Combined host: in-memory bridge ready (handshake + traffic over the link).");
}

void StopCombinedBridge() {
    // Host left/disconnected: detach the client + close its link so every blocked reader unwinds --
    //  * the launcher's combined TCPRcv/UDPRcv return "" (TCPClientMain/UDPClientMain exit, so the
    //    proxy can join them without deadlocking),
    //  * the server's virtual-client TCPRcv returns empty (the in-process server drops the host, so
    //    other players see the leave),
    //  * the UDP drain's wait wakes and its loop exits on Closed.
    // We move gHostClient out under the lock (-> bridge inactive, so the NEXT join builds a fresh one).
    // Readers that already copied the shared_ptr keep the OLD client+link alive until they return, so
    // there is no use-after-free; the old client frees once the drain + every in-flight reader release
    // their copies.
    std::shared_ptr<TClient> Client;
    {
        std::lock_guard<std::mutex> Lk(gLinkMtx);
        Client = std::move(gHostClient);
    }
    if (Client) {
        InMemoryLink* Link = Client->Link();
        if (Link != nullptr) {
            Link->Closed.store(true);
            Link->ToClientCv.notify_all();
            Link->FromClientCv.notify_all();
        }
    }
    info("Combined host: host disconnected; in-memory bridge torn down (next join starts fresh).");
}

// Launcher client side: block until the in-process server pushes a TCP message to the host client
// (or the link closes), then return it. The queue preserves message boundaries, so there is no
// 4-byte length framing to strip. Returns "" when the link is closed.
std::string CombinedClientRecvTCP() {
    auto Client = HostClient();
    if (!Client) {
        return "";
    }
    InMemoryLink* Link = Client->Link(); // `Client` keeps it alive across the blocking wait below
    if (Link == nullptr) {
        return "";
    }
    std::unique_lock<std::mutex> Lk(Link->ToClientMtx);
    Link->ToClientCv.wait(Lk, [&] { return !Link->ToClientTCP.empty() || Link->Closed.load(); });
    if (Link->ToClientTCP.empty()) {
        return "";
    }
    auto Msg = std::move(Link->ToClientTCP.front());
    Link->ToClientTCP.pop();
    return std::string(Msg.begin(), Msg.end());
}

std::string CombinedClientRecvUDP() {
    auto Client = HostClient();
    if (!Client) {
        return "";
    }
    InMemoryLink* Link = Client->Link(); // `Client` keeps it alive across the blocking wait below
    if (Link == nullptr) {
        return "";
    }
    std::unique_lock<std::mutex> Lk(Link->ToClientMtx);
    Link->ToClientCv.wait(Lk, [&] { return !Link->ToClientUDP.empty() || Link->Closed.load(); });
    if (Link->ToClientUDP.empty()) {
        return "";
    }
    auto Msg = std::move(Link->ToClientUDP.front());
    Link->ToClientUDP.pop();
    return std::string(Msg.begin(), Msg.end());
}

#else // !BEAMMP_EMBED_SERVER -- stubs so the launcher still builds without the embedded server

void StartEmbeddedServer(int, const char**) { }
void CombinedServerSendTCP(const std::string&) { }
void CombinedServerSendUDP(const std::string&) { }
std::string CombinedClientRecvTCP() { return ""; }
std::string CombinedClientRecvUDP() { return ""; }
void StartCombinedBridge() { }
void StopCombinedBridge() { }

#endif
