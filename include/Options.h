/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#pragma once

#include <string>

struct Options {
#if defined(_WIN32)
    std::string executable_name = "BeamMP-Launcher.exe";
#elif defined(__linux__)
    std::string executable_name = "BeamMP-Launcher";
#endif
    unsigned int port = 4444;
    bool verbose = false;
    bool no_download = false;
    bool no_update = false;
    bool no_launch = false;
    bool full_mod_hash = false; // force full SHA256 re-validation of cached+mounted mods each connect (default off = fast name+size validate)
    bool no_debug = true; // DEFAULT for public release: [DEBUG] log lines suppressed (skips the string build AND the per-line log-file open/write/close in addToLog). Re-enable with --debug. (--no-debug is kept as an explicit no-op for back-compat.)
    const char* user_path = nullptr;
    const char **game_arguments = nullptr;
    int game_arguments_length = 0;
    const char** argv = nullptr;
    int argc = 0;
};

void InitOptions(int argc, const char *argv[], Options &options);

extern Options options;
