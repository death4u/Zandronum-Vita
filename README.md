# Zandronum Vita

A port of [Zandronum](https://zandronum.com/) to the PlayStation Vita. Zandronum is the multiplayer-oriented Doom source port that continues Skulltag.

This repository is a fork of [TorrSamaho/zandronum](https://github.com/TorrSamaho/zandronum) (Zandronum 3.3-alpha) with the Vita changes added on top. The original Zandronum readme is in [README.zandronum.md](README.zandronum.md).

## Status (v1)

- Runs Doom/Doom II/Freedoom and mods on a real Vita, with GZDoom's OpenGL renderer through vitaGL.
- Tested mainly with **Brutal Doom v21** and **HaloDoom** on **Freedoom 2**, in single player and in offline games with bots.
- Online play has not been tested.
- Heavy mods can still drop below 25 fps in big fights and can stall briefly when a map starts.

## Requirements

- A Vita with homebrew enabled (HENkaku / Ensō) and VitaShell to install the VPK.
- `libshacccg.suprx` in `ur0:data/` (or `ur0:data/external/`). vitaGL compiles its shaders on the device with it. Without it the game falls back to the much slower software renderer.
- An IWAD: `doom2.wad`, `doom.wad`, `freedoom2.wad` or another supported game.

## Installing

1. Install `zandronum.vpk` with VitaShell.
2. Copy your IWAD to `ux0:data/zandronum/`.
3. Copy mods (`.wad` / `.pk3`) to `ux0:data/zandronum/mods/`.
4. Start Zandronum. The launcher lists the games and mods it found:
   - **X** selects the game or toggles a mod (mods load in the order you pick them).
   - **Square** clears the mods.
   - **L/R** change page.
   - **Start** plays.
   - **Select** quits.

Config, saves, controls and logs are kept in `ux0:data/zandronum/`.

## Controls

Left stick moves, right stick looks.

### All games (default layout)

| Button | Action |
|---|---|
| R | Fire (also restarts the level after you die in single player) |
| L | Alt fire |
| Cross | Jump |
| Circle | Crouch |
| Square | Use (open doors, press switches); holding Square is use too |
| Triangle | Next weapon |
| D-pad left / right | Previous / next weapon |
| D-pad down | Reload |
| D-pad up | Automap |
| Select | Scoreboard |
| Start | Main menu |

### HaloDoom

HaloDoom is detected automatically and gets its own layout with HaloDoom's commands:

| Button | Action |
|---|---|
| R | Fire |
| L | Throw grenade |
| Cross | Jump |
| Circle | Melee |
| Square | Tap: reload. Hold: pick up weapon / use |
| Triangle | Switch weapon |
| D-pad left | Switch grenade type |
| D-pad down | Crouch (toggle) |
| D-pad up | Zoom |
| Select | Scoreboard |
| Start | Main menu |

### Menus, console and chat

| Button | Action |
|---|---|
| D-pad | Move |
| Cross | Accept |
| Circle | Back |
| Start | Close the menu |
| L / R | Page up / down |
| Square | Backspace |
| Triangle | Opens the on-screen keyboard (console and chat) |

### Changing the controls

- Every button can have a tap action and a separate hold action. Holds are set in the controls menu by holding the button while binding.
- Each game and mod combination keeps its own layout in `ux0:data/zandronum/controls/`. Changes are saved automatically.

## Building

You need a VitaSDK toolchain plus these libraries installed into `$VITASDK/arm-vita-eabi`:

- [vitaGL](https://github.com/Rinnegatamante/vitaGL), with `src/vita/vitagl-zandronum.patch` applied. Build it with `make NO_DEBUG=1`.
- [SDL 1.2 for Vita](https://github.com/Northfear/SDL-1.2-vita), with `src/vita/sdl12-vita-vitagl.patch` applied. Build it with `SDL_VITAGL=1`.
- vitaShaRK, SceShaccCgExt, libmathneon, taiHEN stubs, zlib, zstd and opus.

Then build Zandronum:

```sh
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
         -DCMAKE_BUILD_TYPE=Release -DNO_ASM=ON -DNO_GTK=ON -DNO_SOUND=ON
make
```

The VPK is written to `build/src/zandronum.vpk`. `-DNO_SOUND=ON` only turns off FMOD; the Vita has its own SDL-based sound and music mixer.

The Vita-specific code is mostly in `src/vita/`, plus `__vita__` sections throughout the engine and the renderer (`src/gl/`).

## Notes

- `vita-halodoom.pk3` (inside the VPK) holds re-encoded copies of eight HaloDoom textures. The Vita can't decode the originals (progressive JPEGs). The textures belong to the HaloDoom authors.
- When something goes wrong, `ux0:data/zandronum/boot.log` and `console.txt` say what happened. They're the files to attach to a bug report.

## License

Same as Zandronum: see [LICENSE.txt](LICENSE.txt) and the license notes in the original readme.
