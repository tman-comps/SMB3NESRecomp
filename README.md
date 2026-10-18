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

- The original picture stays authoritative in the middle, so game logic is untouched.
  The extension is presentation only.
- The camera holds at the start and end of a level instead of showing empty space.
- The status bar stays centred.
- The title screen, world maps and vertical levels fall back to centred 4:3, as does
  anything else not yet supported.
- SMB3's right-edge colour glitch is gone, because those columns come from the rebuilt view.

Known limitation: enemies still spawn where the original game spawns them, just past the
4:3 edge, so in wide view they can appear inside the margins. Wide-view spawning is next.

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
and includes no Nintendo code or assets.
