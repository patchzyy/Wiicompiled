
# WiiCompiled

A native PC port of Mario Kart Wii, made with static recompilation.

There's no emulator in the loop, no interpreter, no JIT, no PowerPC
anywhere at runtime.

> [!IMPORTANT]
> There is no Nintendo code, no assets and no game data anywhere in this project or its releases.
> You need your own legally dumped copy of the PAL version of the game. Setup only ships the
> toolchain, the translation runs on your machine against your disc image, and nothing ever gets
> uploaded.

[What is a github, I just want to play](https://github.com/TeamWheelWizard/WheelWizard/releases/latest)

---

## What it does

**Unlocked framerate with interpolation.** 
The original game is hard-locked to 60 fps. The runtime can generate interpolated frames in between, so on a
120/144 Hz monitor things genuinely look smoother.

> [!WARNING]
> Interpolation is experimental right now and will show artifacts in specific scenarios.

**Any aspect ratio you want.** 
Drag the window bigger, wider, whatever, the camera adjusts
live.

**Native rendering via aurora.** 
The graphics layer is built on
[aurora](https://github.com/encounter/aurora). Aurora is a source-level GameCube & Wii compatibility layer.

**High internal resolution.** 
Play at several times the console's resolution.

**Music ducking.** 
Start playing something else, Spotify, a YouTube video, and
the game automatically mutes its own music until the other audio stops. Optional, if you'd
rather it didn't. All audio that shows in your display media controls on your windows pc fall under this.

**An in-game settings bar.** 
Press **F10** while the game window has focus:
- Internal resolution
- FPS counter
- Controller assignment for all four ports
- Full per-controller button mapping
- Volume, instant mute, and the music ducking toggle

Everything you change is saved to `Config.toml` on the spot and restored next launch.

**Real controller support.** 
Controllers are fed to the game as a GameCube controller.
The port does NOT pretend to be a Wii Remote or Classic Controller.
Mappings are positional (`south`, `east`, `west`, `north`) rather than Xbox-labelled, so the
same config makes sense on Xbox, PlayStation, Nintendo and generic SDL pads alike, and extra
inputs like paddles, touchpads and share buttons show up when the hardware reports them.

## Requirements

- Windows 10 or 11, 64-bit
- GPU: GTX 1650 / RX 6400 / Arc A310 or higher
- CPU: Intel Core i5-8400 / AMD Ryzen 5 2600 (4c/6c, ~3.5GHz+) or higher
- About 20 GB of free disk space during installation (Final game size ~5 GB)
- A clean, unmodified **PAL `RMCP01`** disc image of Mario Kart Wii, dumped by you. ISO, GCM,
  GCZ, CISO, WBFS, WIA and RVZ are accepted.

> [!NOTE]
> GPU/CPU minimums are set by driver support and D3D12/Vulkan feature requirements, not by the game's actual demands.

Only the clean PAL revision will work. Anything else (other
regions, patched executables) is rejected outright.

> [!NOTE]
> Nobody here will tell you where to get the game. Dumping your own disc is on you, and links to
> game files won't be provided or tolerated.

## Installing

For an easy experience, use [Wheel Wizard](https://github.com/TeamWheelWizard/WheelWizard). Pick your clean PAL `RMCP01`
image under Settings, turn on **WiiCompiled (beta)**, and hit install from the Home page.
Wheel Wizard downloads the setup tool from this repo and walks you through install, updates and
launching. The backend itself is deliberately command-line only, Wheel Wizard is a wrapper around it.

> [!CAUTION]
> Only take builds from this repository's
> [Releases](https://github.com/patchzyy/Wiicompiled/releases) page. If someone's sharing an
> installer through Discord or some random download site, don't touch it!!

## A note on related projects

WiiCompiled, Wheel Wizard, Retro rewind and other related projects are developed
**independently** and each has its **own** contribution rules and all have their own
rules. What applies here does not automatically apply there,
and vice versa. Check each project's own CONTRIBUTING and README files.

## Retro Rewind

[Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind), ZPL's Mario Kart Wii mod distribution,
can be built as its **own static profile**: instead of applying `Code.pul` as runtime patches,
the Kamek/Pulsar code is statically translated together with the base game into a separate native
executable.

Wheel Wizard drives this too.

## Building from source

Owning the game is still required even if you compile everything yourself.

You'll need: .NET 8 SDK, CMake, Ninja, and LLVM/Clang (the shipped build uses LLVM-MinGW targeting
`x86-64-v3`).

Build the translator:

```powershell
dotnet build translator/Translator.sln -c Release
```

The default test suite needs no binaries and no host C++ compiler, so you can hack on the
translator without any game data around.

For everything beyond that, feeding in your own `main.dol`/`StaticR.rel`, running the
translation, generating the manifest and build graph, and compiling. see [`translator/README.md`](translator/README.md).

## FAQ

**Is this an emulator?**
No. Everything is compiled to native code before you ever press play. At runtime there's nothing
emulating a Wii CPU or GPU.

**Do you provide the game?**
No. Don't ask. Nothing in this repo or any release contains Nintendo code or assets.

**Why does setup take so long?**
Because we **don't** ship the translated binary, most other recomp projects do, but we
don't want to risk it right now, setup has to run a static recompiler over the whole game
and then throw a C++ compiler at the result. It's a **one-time cost** on your machine.

**Which game version works?**
Clean PAL `RMCP01`. Other regions and modified executables are **rejected**. Translating
them against the wrong manifest would give you a subtly broken game that's miserable to debug for us.

**Can I recompile other GameCube/Wii games with it?**
The translator itself handles DOLs and RELs generically, see
`projects/examples/generic-dol.yml`. The catch is that a *playable* port also needs a runtime:
audio, input, GX, everything the game touches.

**The game crashed / stopped with an error.**
Errors are deliberately loud instead of quietly swallowed. Send a report along with the run log
from `%LOCALAPPDATA%\WiiCompiled\Logs`.

**Will you fix original bugs?**
Not in the base game, behavior identical to real hardware is the goal. Only report things where this port differs
from the original game. As for Retro Rewind, some base-game behavior **is** patched, so if it differs from the
base game, that's normal. If Retro Rewind behavior differs between Dolphin/Wii and WiiCompiled, open an issue on GitHub.

**How accurate are the physics?**
100% - this is proven by in-game ghosts. Since ghosts are replay files based on inputs rather
than tracked positions, matching ghosts prove the physics match across Dolphin/Wii/WiiCompiled.

**Is it done?**
Not fully. The game is in a state where everything should be playable and the physics do match
100% with the original game, but compatibility, rendering, networking and performance are all
actively being worked on. If you do find an issue, we strongly encourage you to open one on
GitHub so we can take a look at it.

## AI usage
AI coding tools were used during development of this project. 
All translated output is verified against real hardware behavior and most importantly, physics accuracy is proven synced across Wii, Dolphin, and WiiCompiled (see FAQ). 

## Credits

- **[aurora](https://github.com/encounter/aurora)** - the GX rendering/windowing backend this
  project's whole graphics layer sits on. MIT licensed.
- **[Dawn](https://dawn.googlesource.com/dawn)** - Google's WebGPU implementation, powering
  aurora's Direct3D, Vulkan and OpenGL backends.
- **[Dolphin Emulator](https://github.com/dolphin-emu/dolphin)** - an invaluable reference for Wii
  hardware behavior during development, plus the source of the free DSP coefficient ROM and the
  unmodified default WiiConnect24 bootstrap tree bundled with the runtime.
- **[Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind)** by ZPL and team - the mod
  distribution this project supports.
- **[Wheel Wizard](https://github.com/TeamWheelWizard/WheelWizard)** - the mod manager this
  project integrates with as a launch backend.
- Everyone in the static recompilation community.

Bundled third-party components and their licenses live in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).


## License

WiiCompiled is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License, version 3](LICENSE) as published by the Free Software Foundation.

WiiCompiled is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

Any mkwii distribution making use of WiiCompiled must be licensed under GPL v3.0.

Not affiliated with, endorsed by, or associated with Nintendo. Mario Kart Wii is a trademark of
Nintendo. No Nintendo intellectual property is contained in, distributed with, or obtainable
through this project.
