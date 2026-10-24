# The Warriors - XBERecomp

An unofficial native PC port of the original Xbox version of The Warriors via static recompilation, built into a 64 bit Windows executable. The game's x86 code is translated to C++ ahead of time, the Xbox kernel, Direct3D and DirectSound calls are implemented on Windows, and everything is drawn with Direct3D 12 and played through XAudio2. Made with the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp) tool.

> [!NOTE]
> This project does not include any game assets or game code. You must provide the files from your own legally acquired copy of the game to play.

> [!WARNING]
> This is an **alpha**. The game boots, plays and sounds right through the opening missions, but it has not been played through to the end, and you may run into crashes, missing effects or broken sound later on. Please [report what you find](#reporting-a-problem).

## Status

### First working boot

<img width="854" alt="The Warriors running natively on PC" src="docs/screenshots/first-boot.png"/>

### Video

[![The Warriors XBE Recomp - 1](https://img.youtube.com/vi/NpzpBngV25E/maxresdefault.jpg)](https://youtu.be/NpzpBngV25E)

## Playing the alpha

### What you need
- Windows 10 or 11, 64-bit.
- A GPU and driver with Direct3D 12 support.
- 8 GB of RAM or more.
- About 1 GB of free space for the port, plus the game itself (up to 8 GB as a disc image).
- **The original Xbox version of The Warriors, NTSC (USA) retail disc**. The PS2 and PSP versions do not work, and other Xbox releases (PAL, other regions) have a different `default.xbe` that this build was not made from.
- An Xbox or XInput controller is recommended, but the keyboard and mouse work too.

You do **not** need Visual Studio, a compiler or anything else installed: everything the port needs is in the zip.

### 1. Download
Download `Warriors.zip` from the [Releases](https://github.com/Antriksh1984/TheWarriors-XBERecomp/releases) page.

### 2. Extract
Extract the whole zip into a new, empty folder of its own, for example `C:\Games\The Warriors`. Keep the folders the way they are in the zip.

- Pick a folder you can write to: not `C:\Program Files`, and preferably not one OneDrive syncs (Desktop and Documents often are). The port writes its game code, saves, settings and logs beside `Warriors.exe`.
- Do not run it from inside the zip.

The folder should look like this:
```
The Warriors\
    assets\              <- the game goes here
    compiler\            <- builds the game code on the first start
    Warriors.exe
    msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll
    README.md, THIRD_PARTY.txt
```

### 3. Add your game
Put your copy of the game in the `assets` folder, in **one** of these two forms:

**A disc image (easiest).** Drop the `.iso` straight into `assets`, with any file name. These all work:
- a full disc dump, such as a [Redump](http://redump.org/) style image (about 7.3 GB), which has the video partition in front of the game,
- the game partition alone, often called an xiso (made by tools such as extract-xiso or xdvdfs).

Only `.iso` is read. A `.7z`, `.zip`, `.rar`, `.cso` or split image must be extracted or converted to a single `.iso` first.

**The extracted game files.** Copy the contents of the disc into `assets`, so `default.xbe` sits directly in it:
```
assets\
    audio\
    bik\
    default.xbe
    Main.wad0 ... Main.wad3
    Paks.wad0 ... Paks.wad2
    System.wad0
    XBoxWad.idx
```

The port checks the game's `default.xbe`, which must be the NTSC retail release it was made from:
```
sha1 fe246a74501cd10d2ce89ca5dbda5080d2b2edcc
```
If yours is a different release, the port says so and asks whether to start it anyway. It will most likely crash.

### 4. First start: building the game code
Run `Warriors.exe`. The first time, a progress window appears while the port turns your game's code into `Warriors.dll`, with the Clang compiler in the `compiler` folder. Nothing from the game ships in the zip, so it is made on your PC from your own copy.

- It takes a few minutes: about 2 minutes on an 8 core CPU with 16 GB of RAM, longer with less memory or fewer cores. It builds more files at once the more RAM is free, so closing other programs helps.
- Let it finish. If it is closed part way, the next start builds again.
- Your antivirus may scan every file the compiler writes, which slows it down a lot. Adding the port's folder as an exclusion helps.
- Windows SmartScreen may say "Windows protected your PC", since the exe is not signed. Click **More info**, then **Run anyway**.

This happens once. Later starts go straight into the game. A new release of the port builds again on its first start.

### 5. Play
The game starts in a borderless window on your main display. Press **F10** at any time for the settings panel: display mode, resolution, frame rate cap, VSync, render scale, anisotropic filtering, mouse sensitivity, every key binding, and the controller's camera speed, dead zone, vibration and button layout.

With a controller it plays as on the Xbox. With the keyboard and mouse, the mouse turns the camera and the [default keys](#default-keys) are below. The button prompts in the game show your keys while you use the keyboard and mouse, and the controller's buttons once you pick up the pad.

### Files the port writes
All of these sit beside `Warriors.exe`:

| File or folder | What it holds |
|---|---|
| `Warriors.dll` | The game code built on the first start. |
| `compiler\work\` | The first start's build files, kept to tell when a rebuild is needed. |
| `hdd\` | The Xbox hard drive: **your saves**. |
| `settings.toml` | Everything set in F10. Delete it to go back to the defaults. |
| `logs\` | `warriors.log` from the last run and `compile.log` from the first start. |
| `CrashDumps\` | A dump for each crash, for bug reports. |

### Updating to a new release
Extract the new zip into a new folder, then copy over your `hdd` folder (saves) and `settings.toml` from the old one, and move your game into the new `assets`. The new version builds its game code again on its first start.

### Troubleshooting
| Problem | What to do |
|---|---|
| "Put the game in the assets folder" | The `assets` folder has neither an `.iso` nor `default.xbe`. Check that the image is not still zipped, and that extracted files are not one folder too deep (`assets\The Warriors\default.xbe`). |
| "...is a different release of the game" | Your `default.xbe` is not the NTSC retail one. Only that release is supported for now. |
| The first start fails | The message names the step that failed; the details are in `logs\compile.log`. Most often it is low disk space, the antivirus removing `clang.exe` or `lld-link.exe` (restore them and add an exclusion), or a folder Windows will not let the port write to. |
| It closes or freezes in game | Open an issue with `logs\warriors.log` and the newest dump in `CrashDumps`, and what you were doing. |
| Black screen or no window | Update your GPU driver. On a laptop with two GPUs, set `Warriors.exe` to the high performance GPU in Windows Settings, System, Display, Graphics. |
| No sound | Set the playback device you want as the Windows default before starting the game. |
| The controller does nothing | Connect it before starting the game. Xbox and XInput pads work best. |
| The mouse is not captured | Click into the game window. Alt+Tab frees the mouse. |

### Reporting a problem
Open an [issue](https://github.com/Antriksh1984/TheWarriors-XBERecomp/issues) with:
- what happened and what you were doing (mission, menu, cutscene),
- `logs\warriors.log` (and `logs\compile.log` if the first start failed),
- the newest file in `CrashDumps` for a crash, zipped,
- your GPU, and whether you used the disc image or the extracted files.

## Default keys
| Action | Keys |
|---|---|
| Move, walk | W A S D, Left Alt |
| Camera | Mouse |
| Light attack (X) | Left mouse |
| Heavy attack, charge (A) | Right mouse, Enter |
| Grab, throw (B) | E, Backspace |
| Action: jump, climb, mug (Y) | Space |
| Sprint (LT) | Left Shift |
| Target, reverse a grapple (RT) | Q |
| Block (Black) | F |
| White | R |
| Rage (both triggers) | Z |
| Gang commands (left stick click) | C |
| Right stick click | Middle mouse, V |
| D-pad | Arrow keys, 1 2 3 4 |
| Pause (Start), Back | Escape, Tab |
| Settings panel | F10 |

In menus the left mouse button selects, the right button backs out and the wheel scrolls. Every key can be rebound in F10.

## Known issues
- Only the opening missions have been played so far. Later missions may crash or show broken effects.
- The game runs at 30 FPS, its own frame pacing; nothing unlocks it yet. The camera follows the mouse at that rate too.
- 3D sounds are panned by their direction with an interaural delay and a head shadow model, not the console's own HRTF filters. LFOs, the pitch and filter envelope and the I3DL2 occlusion filters are not reproduced.
- The game's Bink version differs from the one the SDK knows, so Bink movies do not get the SDK's frame rate fix.
- Icons the HUD draws as pictures (the stick by the minimap) still show the controller, and prompts in languages other than English name the pad in words.
- Only the NTSC retail release is supported.

## Progress
- Recompiled from the NTSC retail XBE (XDK 5849, sha1 `fe246a74501cd10d2ce89ca5dbda5080d2b2edcc`).
- XDK symbols the signature scan could not resolve on a retail disc, found by hand and documented in `config/game.toml`.
- DirectSound is the SDK's with what XACT relies on in this game, built in its place (`src/audio/dsound.cpp`): mix-in buffers, so XACT's 3D sounds play through their submixes as on the console; XACT's 3D voice data, with its distance, cone, front, rear, centre and reverb levels, Doppler pitch and the direction that pans each sound between the ears; amplitude envelopes, and stops that let them release; the library's stream pause and flush rules; and the voice properties XACT reads back when it moves a sound to another submix.
- DirectSound exports the SDK has no name for (pitch, LFO, cones, 3D voice data, output buffers, sound streams made through `IDirectSound`), handed to it from `src/game/dsound.cpp`.
- The MCPX APU sample counter that XACT reads directly, kept running in `src/common/apu_clock.cpp`.
- XACT's streamed cues (cutscene dialogue and music) wait until they are started and stay stopped once stopped, so skipped cutscenes no longer keep playing their audio, and the line after a stopped one is no longer held silent (which also slowly used up XACT's voices).
- `D3DDevice_DrawIndexedVerticesUP`, which the SDK has no version of, drawn through the host's `DrawVerticesUP` (`src/game/d3d.cpp`).
- Sound pitch and pitch variation, carried over to the host mixer's frequency.
- The Rockstar logo can be skipped like the other boot movies (`src/game/movies.cpp`).
- Keyboard and mouse for player 1, alongside a controller on the same port (`src/game/kbm.cpp`). The mouse turns the camera itself by its own angle, as on PC, through the camera's yaw and pitch speeds (`src/game/camera.cpp`), and stands in for the right stick where the camera does not read it (the gang command menu).
- Button prompts show the keys bound to them while the keyboard and mouse are in use, and the controller's buttons again once it is used (`src/game/prompts.cpp`): the text's button icons, the words that name the pad ("click the left thumbstick", "pull and release both triggers") and the title screen's PRESS START, which becomes PRESS ANY KEY. They follow rebinding at once.
- A settings panel on F10 (`src/game/menu.cpp`).

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp) by TGP482, the recompiler and runtime this is built on, and [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp), which this project follows.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [LLVM](https://llvm.org/) for the Clang compiler and LLD linker that build the game code on the player's PC.
