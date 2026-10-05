# Kickle Cubicle v0.0.1

> **Source repository:** Build instructions are in **Building this source
> snapshot** below. No executable or game ROM is included. This snapshot was
> compiled successfully using local ROMs, with generated output outside the
> publishing folder. The following player guide describes the resulting app.


An unofficial NES recompilation of Kickle Cubicle with controller navigation,
optional widescreen, offline achievements, and additional game modes.

## Getting started

1. Keep the executable, DLLs, and `assets` folder together.
2. Run `KickleCubicleNESRecomp.exe`.
3. Choose **Load ROM** and select your own **Kickle Cubicle (USA)** NES ROM.
4. Choose **Start Game**.

No ROM or remembered ROM path is included. The launcher checks the supplied
ROM against the original USA version. Load the original ROM even when using
Gale Festival; its level changes are provided by this build.

## Clean defaults

- Controller enabled for Player 1; the first available compatible pad is used.
- Windowed display at 3x, integer scaling on, linear filtering off.
- Audio enabled at 100%.
- Original game mode; cheats, widescreen, and display filters off.
- Offline achievements enabled; launcher shown at startup.

Use **Controls** to select Keyboard instead or remap controller, keyboard, and
shortcut bindings. Settings are saved beside the executable in `config.ini`.
Controller input uses SDL-compatible gamepads, including XInput controllers.

## Controls

| Action | Controller default | Keyboard default |
| --- | --- | --- |
| Move | D-pad / left stick | Arrow keys |
| NES A | A | Z |
| NES B | X | X |
| Start | Start / Menu | Enter |
| Select | Back / View | Backslash |
| Open game overlay | RB | Escape |
| Fast-forward (hold) | RT | Tab |
| Rewind (hold) | LT | R |
| Save state | Unassigned | F8 |
| Load state | Unassigned | F9 |
| Fullscreen toggle | Unassigned | F11 |
| Screenshot | Unassigned | F12 |

The keyboard gameplay bindings apply when Keyboard is selected as the input
source. Mouse and controller can navigate the launcher. A selects, B backs out.
Fast-forward presents at up to 30 FPS while simulation advances faster.

## Menus and features

**Display Options:** Windowed, Exclusive Fullscreen, Borderless Fullscreen,
widescreen, and Display Filter (Off, CRT Soft, LCD Grid, Sharp, Warm Composite).
Boss arenas and princess rooms retain their original width in widescreen mode.
Isometric mode remains experimental in the source and is hidden from menus.

**Audio Options:** Enable/disable audio and adjust volume.

**Cheats:** Infinite Lives and Invincibility.

**Level Select:** Choose Garden, Fruit, Cake, or Toy Land and a puzzle.
Start Selected Level skips the visible title screen and proceeds to the map
introduction for the selection. This starts a fresh run and disables Boss Rush.
Garden, Fruit, and Toy have 17 puzzles each; Cake has 16.

**Additional Modes:**

- **Boss Rush:** Fight all four bosses in order.
- **Gale Festival:** Play the remixed puzzle levels.
- **1 Life, 1 Credit Clear:** One-life full-game challenge. Cheats, rewind,
  fast-forward, save/load states, and level selection are unavailable.
  Resetting to the title screen abandons the attempt.

**Achievements:** 23 local challenges inspired by
https://retroachievements.org/game/1777 plus Boss Rush Champion. Unlocks show
an achievement card and sound. Cheats, rewind, fast-forward, and save/load are
allowed. Regular achievements exclude Boss Rush. Achievements can be disabled
or reset with confirmation. No RetroAchievements account, login, or reporting
is used.

In the **RB overlay**, **System** is at the bottom. It contains save/load state,
Resume, Reset to Title Screen, and Quit. Reset ends the current run; earned
achievements remain unlocked.

## Files and progress

- `config.ini`: display, audio, controls, and feature preferences.
- `rom.cfg`: remembered ROM path, created after selecting a ROM.
- Save states and other runtime files are created by the game as needed.
- Offline achievements are stored separately at
  `%APPDATA%\KickleRecomp\KickleCubicle\offline-achievements.ini`.

This clean folder includes no personal save states or achievement file. On a
PC that already used this recompilation, existing AppData achievements still
appear. Use **Achievements > Reset Achievements** to start earning them again.
The clean package does not delete existing progress.

## Notes and credits

This is an early v0.0.1 build. The release executable compiled successfully;
this package has not received a separate gameplay verification pass. Some
achievement conditions and experimental features may need further refinement.
Save states should be kept with the version that created them.

Built using mstan/nesrecomp and the Zelda recomp-ui frontend. Display filters
are fixed CPU adaptations of mstan/snesrecomp's shared CC0-1.0 shader presets,
originally written for Mega Man X Recomp. They are not a general GLSL loader.
Menu artwork, icon, and achievement sound were supplied for this project.
Kickle Cubicle and related artwork belong to their respective owners.

No game ROM is distributed in this folder.

## Building this source snapshot

This folder is the source package, not a ready-to-run binary. The game sources
are in `KickleRecomp/`; the modified framework and UI are vendored alongside
it. Build output and generated ROM data must stay out of version control.

### Requirements (Windows x64)

- CMake 3.20 or newer, Python 3.11 or newer, and Ninja.
- A C11/C++17 compiler and Windows resource compiler. LLVM/Clang with the
  required Windows libraries is the toolchain used by the current build.
- Your original Kickle Cubicle (USA) ROM and Gale Festival patched ROM.
  The latter is needed to regenerate the private Gale Festival byte table.
- SDL2 development files are included in the dependency snapshot.

From this repository's root, using PowerShell:

```powershell
cmake -S KickleRecomp -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DKICKLE_ROM="C:/your-local-roms/Kickle Cubicle (USA).nes" `
  -DKICKLE_GALE_ROM="C:/your-local-roms/Kickle Cubicle Gale Festival.nes"
cmake --build build --parallel 8
```

If needed, supply `-DCMAKE_C_COMPILER=...`, `-DCMAKE_CXX_COMPILER=...`,
`-DCMAKE_RC_COMPILER=...`, and `-DPython3_EXECUTABLE=...` to point at your tools.
The build bootstraps NESRecomp and generates native code from your local ROM.
Gale Festival data is generated inside the build folder; it is not committed.

The output executable, SDL2 DLL, assets, and default `config.ini` are placed in
`build/`. A Clang toolchain may also require its C++ runtime DLLs beside the
executable. Include the runtime DLLs supplied by your toolchain when packaging
an executable. Rebuilding restores the provided default config in that build
folder; use a separate play folder for personal settings.

The executable has no preselected ROM path unless the developer explicitly
sets `-DKICKLE_DEVELOPMENT_ROM=ON`. Controller is the default input source.

### Publishing to GitHub

Publish this folder's contents as the repository root. Keep `.gitignore` and
all dependency license files. Do not upload `roms/`, any build directory, local
ROM images, `gale_data.h`, save states, or personal settings. The clean runtime
folder can be uploaded separately as a release asset.

See `THIRD_PARTY_NOTICES.md` for licenses. NESRecomp uses the PolyForm
Noncommercial license; the packaging step does not assign a new license to
the Kickle-specific modifications or supplied media.

