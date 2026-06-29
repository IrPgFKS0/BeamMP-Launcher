/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#pragma once

#include <atomic>
#include <string>

// Combined-host (--combined): the launcher runs the BeamMP server IN-PROCESS and bridges the
// host's own client to it over an in-memory channel (InMemoryLink) instead of loopback sockets.
// LAN2 and any other client still connect to the embedded server over the real network. See
// memory: combined-host-inprocess-merge.
extern std::atomic<bool> g_CombinedMode;

// Start the embedded server on a background thread and, when it's ready, register the in-memory
// virtual client. Call once, early, in --combined mode. argc/argv are passed through to the
// server (minus --combined) so --config / --port / --working-directory still work.
void StartEmbeddedServer(int argc, const char** argv);

// Client->server sends over the in-memory link (--combined): push the data to the virtual client's
// in queues. Used both by the rerouted ServerSend() and by the bridge-aware TCPSend/UDPSend so the
// launcher's client (incl. the join handshake) talks to the in-process server. No-ops until ready.
void CombinedServerSendTCP(const std::string& Data);
void CombinedServerSendUDP(const std::string& Data);

// Server->client receives over the in-memory link (--combined): block until the in-process server
// pushes a TCP/UDP message to the host client (or the link closes), then return it. Used by the
// bridge-aware TCPRcv/UDPRcv so the launcher's client read loops drain the server's responses.
std::string CombinedClientRecvTCP();
std::string CombinedClientRecvUDP();

// Create the in-process server's host client + start the client->server UDP feed. Called from
// TCPGameServer when a join begins. At most one active client per session (guarded by gLinkMtx /
// gHostClient); a duplicate call while a bridge is active is a no-op. StopCombinedBridge clears it.
void StartCombinedBridge();

// Tear down the host client when the session ends (game disconnect / proxy shutdown): close the
// in-memory link so blocked readers unwind, and reset so the next join builds a fresh client. Must
// be called before joining TCPClientMain/UDPClientMain (they block on the link). Idempotent.
void StopCombinedBridge();

// Synchronous (unbuffered, flushed) stderr debug marker -- survives an abrupt process exit/crash,
// unlike the async info() logger. Used to trace the combined-mode startup path.
void CombinedDbg(const char* msg);
