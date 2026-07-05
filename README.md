# BeamMP-Launcher

> ### ⚠️ This is the **LAN-only fork** of the BeamMP launcher (branch `lan`)
> Modified to run without the BeamMP backend (no login/auth), and extended into the
> **combined host**: built with the server embedded, one exe runs the dedicated server
> *and* bridges your own game over an in-memory channel (`--combined`). Docs, prebuilt
> binaries and the build scripts live in the fork's main repo:
> **[IrPgFKS0/BeamMP](https://github.com/IrPgFKS0/BeamMP/tree/lan)** →
> [`docs/lan/`](https://github.com/IrPgFKS0/BeamMP/tree/lan/docs/lan) (setup/tuning),
> [`dist/`](https://github.com/IrPgFKS0/BeamMP/tree/lan/dist) (release zip),
> [`tools/`](https://github.com/IrPgFKS0/BeamMP/tree/lan/tools) (build/host scripts —
> this repo is built via `build-launcher.bat` from a sibling checkout with
> [BeamMP-Server](https://github.com/IrPgFKS0/BeamMP-Server)'s vcpkg).
> The sections below are inherited from upstream BeamMP.

The launcher is the way we communitcate to outside the game, it does a few automated actions such as but not limited to: downloading the mod, launching the game, and create a connection to a server.

## [Getting started](https://docs.beammp.com/game/getting-started/)

## License

BeamMP Launcher, a launcher for the BeamMP mod for BeamNG.drive
Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
