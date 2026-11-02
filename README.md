# DeliriumFNV

Delirium (Euphoria-style active ragdoll) physics for dying NPCs in Fallout New Vegas 1.4.0.525, as an xNVSE plugin.
When a humanoid NPC dies, a Delirium body is started from the NPC's real pose and the velocity the game's own ragdoll gave it
(including the kill impulse). Its result is written into the NPC's bones every frame; when it comes to rest the final pose is held.

## Install
1. Install xNVSE. Put `DeliriumFNV.dll` in `Data\NVSE\Plugins\`.
2. Put the `DeliriumFNV` folder (the ini, the rig, the joints file, `skyrim_character.json` and `delirium_physics.dll`) next to it.
3. Start the game with `nvse_loader.exe`. The log is `Documents\My Games\FalloutNV\NVSE\DeliriumFNV.log` (the previous run is `.prev`).

Releases: every push to `main` is built by GitHub Actions and published as the `latest` release.

## Settings
See `DeliriumFNV.ini`. `Drive=0` makes the plugin watch only; `Enabled=0` turns it off.

## Build
`cmake -S . -B build -A Win32` then `cmake --build build --config Release` for the plugin; the engine DLL is built from
`YoucefNabil/delirium-physics` (branch `skyrim`) for `i686-pc-windows-msvc`. The workflow in `.github/workflows` does both.

## Source layout
`src/main.cpp` entry points | `probe.*` vtable-slot hooks | `observe.*` death takeover and bone writing | `actor.*` guarded access to
the skeleton | `pick.*` the game's ray cast | `engine.*` loader for the Delirium DLL | `rig.*` bone mapping | `ingame.*` HUD messages and NVSE messages.

## License
GPL-3.0-or-later (builds on Delirium and skyrim-delirium, both GPL-3.0).
