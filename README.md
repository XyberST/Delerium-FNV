# DeliriumFNV - phase 1 (discovery)

Target: Fallout New Vegas **1.4.0.525** with xNVSE. This phase changes **no gameplay**. It answers two questions:
1. Does the 32-bit engine run inside the game process? (smoke test)
2. Which addresses are the ragdoll takeover, per-frame drive and death handler? (probes)

## Build
1. Engine: on Windows, in a clone of `YoucefNabil/delirium-physics` (branch `skyrim`), run `tools/build_engine_win32.bat`.
2. Plugin: `cmake -S . -B build -A Win32` then `cmake --build build --config Release`.
3. Copy `build/stage/Data` into the game folder, and put the 32-bit `delirium_physics.dll` in `Data/NVSE/Plugins/DeliriumFNV/`.

## Run
1. Start the game with NVSE, load a save near some NPCs. Open `Documents\My Games\FalloutNV\NVSE\DeliriumFNV.log` (next to nvse.log; the game folder under Program Files is often not writable).
2. Check the smoke lines. Expected: balance `com z ~1.0, fallen=0`, after shot `fallen=1`.
3. Press **Insert** (mark), kill or knock down one NPC, wait until they have fully fallen, then press **Delete** (dump).
4. Do the same once for **getting up** (a stimpak or a knockdown that recovers), and once for a plain **idle** period as a control.
5. Send me the log. Probes hit only around the event are the candidates; the log lists rarest first.

## Safety
- Function probes verify the expected prologue bytes first and are skipped on any mismatch.
- Vtable probes only replace a slot that already holds a code pointer. Set `Probes=0` in the ini to turn all probes off.
- Thunks save and restore all registers and flags and then jump to the original code.
- Verified here: the plugin compiles for 32-bit Windows (MinGW, warning-free) and the generated thunk was emulated for argument capture and register/stack/flag restore. **Not verified: any run inside the game.**

## Next phases
- Phase 2: read the actor's skeleton and write poses back using `data/fnv_character_rig.txt`; take over on the death hook; feed cell collision to `nm_world_add_mesh`.
- `tools/gen_engine_api.py` regenerates `src/engine_api.inc` if the engine C API changes.
