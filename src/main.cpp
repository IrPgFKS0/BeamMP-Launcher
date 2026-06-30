/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "Http.h"
#include "Logger.h"
#include "Network/network.hpp"
#include "Security/Init.h"
#include "Startup.h"
#include "Utils.h"
#include <curl/curl.h>
#include <iostream>
#include <thread>
#include "Options.h"
#include "CombinedHost.h" // --combined mode (in-process server + in-memory host client)
#ifdef BEAMMP_EMBED_SERVER
#include "ServerRuntime.h" // embedded BeamMP server (combined-host build, -DBEAMMP_EMBED_SERVER=ON)
#endif
#include <string_view>
#include <utility>

Options options;

[[noreturn]] void flush() {
    while (true) {
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, const char** argv) try {
    CombinedDbg("main: process start (combined binary)");
    // Initialize the executable-path cache FIRST: info()/warn()/error()/addToLog() all resolve the
    // Launcher.log path via GetEP(), and the --combined branch below logs before the original GetEP
    // init (which used to sit down at the cls point). Without this, the first info() in --combined
    // hit GetEP() with P=nullptr on its very first call -> wstring(nullptr) -> silent access violation.
    GetEP(Utils::ToWString(std::string(argv[0])).c_str());
#ifdef BEAMMP_EMBED_SERVER
    // --server-only: run the embedded BeamMP server headless (the dedicated-server mode of the
    // combined host binary -- relay only, no game bridge). Other args (--config, --port,
    // --working-directory, ...) pass straight through to the server. Default (no flag) will be
    // the embedded host+server; that wiring lands in Stage 4. Only present in the combined build.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--server-only") {
            MainArguments sa {};
            sa.argc = argc;
            sa.argv = const_cast<char**>(argv);
            sa.InvokedAs = (argv[0] != nullptr) ? argv[0] : "";
            for (int j = 1; j < argc; ++j) {
                if (std::string_view(argv[j]) != "--server-only") {
                    sa.List.emplace_back(argv[j]);
                }
            }
            return BeamMPServerMain(std::move(sa));
        }
    }
    // --combined: run the server in-process and bridge the host's own client over an in-memory
    // channel (no loopback sockets). Unlike --server-only this does NOT return -- it starts the
    // server in the background and continues into the normal launcher flow, so the local game
    // connects and the in-memory bridge takes over (see ServerSend / TCPGameServer / CombinedHost).
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--combined") {
            info("Starting in COMBINED mode: in-process server + in-memory host client.");
            CombinedDbg("main: --combined matched, about to StartEmbeddedServer");
            g_CombinedMode = true;
            StartEmbeddedServer(argc, argv);
            CombinedDbg("main: returned from StartEmbeddedServer; continuing launcher startup");
            break;
        }
    }
#endif
#if defined(_WIN32)
    system("cls");
#elif defined(__linux__)
    system("clear");
#endif

#ifdef DEBUG
    std::thread th(flush);
    th.detach();
#endif

    curl_global_init(CURL_GLOBAL_ALL);

    // (GetEP is already initialized at the top of main(); see note there.)

    InitLog();
    ConfigInit();
#ifdef BEAMMP_EMBED_SERVER
    if (g_CombinedMode) {
        // The combined binary runs from the SERVER's directory, where ./Resources is the server's
        // own tree (Resources/Client, Resources/Server, ServerConfig.toml). The launcher's default
        // cache dir is also ./Resources, so they collide -- the launcher's mod cache + mods.json +
        // pending_mod_removals.txt + prune would clobber the server's structure. Scope the launcher
        // cache to ./Resources/cache so the two never touch.
        CachingDirectory = std::filesystem::path("./Resources/cache");
        // Printed after system("cls") so it persists on screen. Makes the two-processes-in-one-window
        // nature explicit to whoever launched it.
        info("============================================================");
        info("  COMBINED HOST  -  one window, two processes:");
        info("    [1] dedicated server   (in-process, headless -> Server.log)");
        info("    [2] launcher + your game (this window)");
        info("  Cache isolated at ./Resources/cache; other players join this PC over the LAN.");
        info("============================================================");
    }
#endif
    InitOptions(argc, argv, options);
    InitLauncher();

    info("BeamMP LAN fork  -  launcher/combined build p13h31 (pairs with mod 4.21.1-LAN p13h43+)");

    info("IMPORTANT: You MUST keep this window open to play BeamMP!");

    try {
        LegitimacyCheck();
    } catch (std::exception& e) {
        error("Failure in LegitimacyCheck: " + std::string(e.what()));
        throw;
    }

    try {
        HTTP::StartProxy();
    } catch (const std::exception& e) {
        error(std::string("Failed to start HTTP proxy: Some in-game functions may not work. Error: ") + e.what());
    }
    PreGame(GetGameDir());
    InitGame(GetGameDir());
    CoreNetwork();
} catch (const std::exception& e) {
    error(std::string("Exception in main(): ") + e.what());
    info("Closing in 5 seconds");
    info("If this keeps happening, contact us on either: Forum: https://forum.beammp.com, Discord: https://discord.gg/beammp");
    std::this_thread::sleep_for(std::chrono::seconds(5));
}
