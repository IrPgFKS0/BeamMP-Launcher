/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "Options.h"

#include "Logger.h"
#include <cstdlib>
#include <filesystem>

void InitOptions(int argc, const char *argv[], Options &options) {
    int i = 1;

    options.argc = argc;
    options.argv = argv;

    std::string AllOptions;
    for (int i = 0; i < argc; ++i) {
        AllOptions += std::string(argv[i]);
        if (i + 1 < argc) {
            AllOptions += " ";
        }
    }
    debug("Launcher was invoked as: '" + AllOptions + "'");


    if (argc > 2) {
        if (std::string(argv[1]) == "0" && std::string(argv[2]) == "0") {
            options.verbose = true;
            options.no_download = true;
            options.no_launch = true;
            options.no_update = true;
            warn("You are using deprecated commandline arguments, please use --dev instead");
            return;
        }
    }

    options.executable_name = std::string(argv[0]);

    while (i < argc) {
        std::string argument(argv[i]);
        if (argument == "-p" || argument == "--port") {
            if (i + 1 >= argc) {
                std::string error_message =
                    "No port specified, resorting to default (";
                error_message += std::to_string(options.port);
                error_message += ")";
                error(error_message);
                i++;
                continue;
            }

            int port = options.port;

            try {
                port = std::stoi(argv[i + 1]);
            } catch (std::exception& e) {
                error("Invalid port specified: " + std::string(argv[i + 1]) + " " + std::string(e.what()));
            }

            if (port <= 0) {
                std::string error_message =
                    "Port invalid, must be a non-zero positive "
                    "integer, resorting to default (";
                error_message += std::to_string(options.port); // was `+= options.port` (int) -> appended a char by code point, not the number
                error_message += ")";
                error(error_message);
                i++;
                continue;
            }

            options.port = port;
            i++;
        } else if (argument == "-v" || argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--no-download") {
            options.no_download = true;
        } else if (argument == "--no-update") {
            options.no_update = true;
        } else if (argument == "--no-launch") {
            options.no_launch = true;
        } else if (argument == "--full-mod-hash") {
            options.full_mod_hash = true;
        } else if (argument == "--combined" || argument == "--server-only") {
            // Run-MODE selectors, consumed in main() before InitOptions runs (--server-only returns
            // into the server; --combined starts the in-process server and falls through to here).
            // Recognized so they are not reported as "Unknown option". See the --help MODES section.
        } else if (argument == "--dev") {
            options.verbose = true;
            options.no_download = true;
            options.no_launch = true;
            options.no_update = true;
        } else if (argument == "--user-path") {
            if (i + 1 >= argc) {
                error("You must specify a path after the `--user-path` argument");
                break; // no value follows; don't read argv[argc] (== nullptr) into user_path
            }
            options.user_path = argv[i + 1];
            i++;
        } else if (argument == "--" || argument == "--game") {
            options.game_arguments = &argv[i + 1];
            options.game_arguments_length = argc - i - 1;
            break;
        } else if (argument == "--help" || argument == "-h" || argument == "/?") {
            const std::string exe = std::filesystem::path(options.executable_name).filename().string();
            std::cout << "USAGE:\n"
                "\t" + exe + " [MODE] [OPTIONS] [-- <GAME ARGS>...]\n"
                "\n"
                "This is the combined BeamMP LAN binary: it contains BOTH the launcher (which bridges\n"
                "your game to a server) AND the dedicated server itself. Pick a MODE:\n"
                "\n"
                "MODES (choose one; default is launcher-only):\n"
                "\t(no mode)            Launcher only. Connects THIS PC's game to a separate BeamMP\n"
                "\t                     server over the network -- the classic client role.\n"
                "\t--combined           Combined host: ONE window, TWO processes. Runs the dedicated\n"
                "\t                     server in-process + headless (logs to Server.log) AND connects\n"
                "\t                     this PC's own game to it over an in-memory channel. Other\n"
                "\t                     players join this PC over the network. Host and play on one PC.\n"
                "\t                     Run it from the server folder (with ServerConfig.toml).\n"
                "\t--server-only        Dedicated server only. Headless, no game is launched; output\n"
                "\t                     goes to the console + Server.log. Others join over the network.\n"
                "\n"
                "OPTIONS:\n"
                "\t--port <port>    -p  Change the default listen port to <port>. This must be configured ingame, too\n"
                "\t--verbose        -v  Verbose mode, prints debug messages\n"
                "\t--no-download        Skip downloading and installing the BeamMP Lua mod\n"
                "\t--no-update          Skip applying launcher updates (you must update manually)\n"
                "\t--no-launch          Skip launching the game (you must launch the game manually)\n"
                "\t--full-mod-hash      Re-verify cached/mounted mods by full SHA256 each connect (default: fast name+size validate)\n"
                "\t--dev                Developer mode, same as --verbose --no-download --no-launch --no-update\n"
                "\t--user-path <path>   Path to BeamNG's User Path\n"
                "\t--game <args...>     Passes ALL following arguments to the game, see also `--`\n"
                << std::flush;
            exit(0);
        } else {
            warn("Unknown option: " + argument);
        }

        i++;
    }
}
