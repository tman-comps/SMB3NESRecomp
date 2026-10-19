# Super Mario Bros. 3 NES Recomp

A static recompilation of *Super Mario Bros. 3* (NES) to native PC code, built on
[nesrecomp](https://github.com/mstan/nesrecomp). The game's 6502 code is translated to C
and compiled. It is not an emulator. On top of the faithful 4:3 game it adds
**widescreen**.

**No ROM is included.** You need your own legally obtained *Super Mario Bros. 3 (USA) (Rev 1)* ROM.

## Status

Early and experimental.

- Boots, plays and renders correctly through the title, the World 1 map and 1-1. Code
  that hasn't been seeded yet runs in nesrecomp's cycle-accurate interpreter, so the
  whole game is playable. Seeded code runs natively.
- Widescreen (Fit, 16:9, 21:9 or 32:9) works in horizontal levels, with 4:3 fallback everywhere else.

## Widescreen

```
SuperMarioBros3Recomp.exe baserom.nes --widescreen fit     # default: follow the window
SuperMarioBros3Recomp.exe baserom.nes --widescreen 16:9    # also 21:9, 32:9
SuperMarioBros3Recomp.exe baserom.nes --widescreen off     # original 4:3, untouched
```

SMB3 keeps each horizontal level fully decompressed in cartridge RAM. The renderer draws
the extra columns from that memory, using the game's own tile tables, graphics banks,
palettes and the scroll the PPU actually used on each line. Where the original 256-pixel
view and the rebuilt view overlap, the background matches the real PPU output
pixel for pixel (`--widescreen-check` verifies this while playing).

- Sprites in the middle come from the original picture; background pixels come from the
  exact rebuild. That also removes SMB3's right-edge colour glitch.
- **Enemies live across the whole wide view.** They spawn and despawn at the wide edges,
  and enemies outside the original 256 pixels are drawn by running their own sprite routine
  in an isolated call, so their graphics and animation are the game's. Use
  `--widescreen-enemies classic` to keep the original 4:3 spawn timing.
- The camera holds at the start and end of a level instead of showing empty space.
- The status bar stays centred on black.
- **Title screen:** the curtain and checkered floor extend, and the hills are mirrored outward.
- **World maps:** maps that scroll show more of the map. World 1's single-screen map
  keeps its framed 4:3 look.
- Vertical levels and bonus-game screens still use centred 4:3 for now.

## Controls

Keyboard and controllers both work out of the box. Controllers are hot-pluggable, and every
binding can be changed on the launcher's **Controls** page.

| Action | Keyboard | Controller |
|---|---|---|
| Jump (NES A) | Z | A |
| Run / fireball (NES B) | X | X |
| Start / Select | Enter / Backslash | Start / Back |
| Move | Arrow keys | D-pad or left stick |
| In-game menu (settings, save/load state, view mode) | Esc | RB |
| Fast-forward (hold) | Tab | RT |
| Save / load state | F8 / F9 | via the menu |
| Fullscreen | F11 | - |

## Settings

- **Launcher → Mods → Widescreen:** on/off, aspect (Fit / 16:9 / 21:9 / 32:9), enemy timing,
  and **Status bar: Classic / Modern**. Modern removes the bar for a taller view and draws just
  lives and coins on screen (plus the P-meter while it charges and any held item cards), like
  modern 2D Mario games. It applies in levels only; maps keep the classic bar.
- **In-game menu → View mode:** switch between Native 4:3, 16:9 and Adaptive while playing.
- **Launcher → Mods → Quality of life → No slowdown** (on by default): gives the game extra CPU
  time each frame, so busy scenes no longer slow down, while the picture and music keep their
  normal pace. Turn it off for original timing.

## Building (Windows)

Requirements: CMake 3.20+, Ninja, Python 3.11+ and a C11/C++17 compiler. The project is
built with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) clang; Visual Studio 2022
should also work.

```
git clone --recurse-submodules <this repo>
cd SMB3NESRecomp
copy "path\to\your\Super Mario Bros. 3 (USA) (Rev 1).nes" baserom.nes
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
build\SuperMarioBros3Recomp.exe baserom.nes
```

Code generation runs during configure. `build.ps1` wraps these steps.

## Layout

| Path | What |
|---|---|
| `src/smb3_widescreen.c` | The widescreen compositor |
| `src/cycle_extras.c` | Hooks into the nesrecomp cycle host (options, settings) |
| `game.toml`, `cyc_seeds.txt` | Recompiler configuration and native-code seeds |
| `routes/` | Scripted input routes for headless testing |
| `tools/` | Symbol extraction from the disassembly, and an FCEUX movie converter |
| `nesrecomp/`, `recomp-ui/` | Engine and launcher (submodules) |
| `smb3-disassembly/` | Reference disassembly (submodule) |

## Credits

- **[nesrecomp](https://github.com/mstan/nesrecomp)** by mstan (Matthew Stanley) and contributors:
  the static recompiler, cycle-accurate runtime and widescreen rendering framework this port
  is built on. Part of the [R.A.I.D. community](https://github.com/RetroPortingToolKit).
- **[recomp-ui](https://github.com/mstan/recomp-ui)**: the launcher and in-game menu.
- **[SuperMarioBrosNESRecomp](https://github.com/mstan/SuperMarioBrosNESRecomp)**: the SMB1
  port whose widescreen design this follows.
- **Southbird's [Super Mario Bros. 3 disassembly](https://github.com/captainsouthbird/smb3)**
  (sonicepoch.com/sm3mix): every RAM and ROM address used here comes from it.
- [SDL2](https://www.libsdl.org/) for windowing, input and audio.

*Super Mario Bros. 3* is © Nintendo. This project is not affiliated with or endorsed by Nintendo,
and includes no Nintendo code. The launcher's box art (`art/boxart.tga`) is the original North
American cover, © Nintendo, from a scan hosted on the [Super Mario Wiki](https://www.mariowiki.com/Gallery:Super_Mario_Bros._3).
