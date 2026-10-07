# NX on Orbis

> **This is the `experimental-fastmem` branch** (tests 25-29), not the release. Fastmem works, but races no longer load (direct memory runs out). For the build that reaches races use `main` / v0.1.0. Details in [docs/STATUS.md](docs/STATUS.md#the-experimental-branch-tests-25-29).

An experimental port of the **Eden** Nintendo Switch emulator (a yuzu fork) to a jailbroken
**PS4 Pro** (OpenOrbis toolchain, Mesa RADV Vulkan driver for the PS4 GPU).

> **Status: closed experiment (stopped for lack of time), published so anyone can pick it up.** It boots, runs real games,
> and Mario Kart 8 Deluxe plays full races, but at **13-16 fps** with red and blue swapped in the
> whole image. It is not a way to play Switch games on a PS4. Read [docs/STATUS.md](docs/STATUS.md)
> for what was expected, what was reached, and the projected performance ceiling.

| | |
|---|---|
| Console | PS4 Pro, firmware 12.02, GoldHEN (the only console it was tested on) |
| Base | Eden `5f142c79` (the commit the PS5 port ProsperoEden pins) + 31 patches in `patches/eden/` |
| Toolchain | OpenOrbis 0.5.4 + [orbis-sdk-v1](https://github.com/orbis-ports/orbis-porting-kit/releases/tag/orbis-sdk-v1) (orbis-compat, Mesa RADV for "Liverpool" GFX7), LLVM 18, libc++ 18 built for the PS4 |
| Release | [v0.1.0](../../releases/tag/v0.1.0): `.pkg` + the exact ELF (for symbolizing crash logs) |
| License | GPL-3.0-or-later (see `LICENSE` and `NOTICE.md`) |

## Not affiliated with Eden. Built with AI.

- This project is **not affiliated with, endorsed by, or supported by the Eden project** or its
  developers. **Do not report problems from this port to Eden** (issues, Discord or anywhere else).
- It was written with heavy use of AI assistants (Anthropic's Claude, and OpenAI's Codex for a
  couple of test rounds), directed and tested on the console by the author. The Eden project
  prohibits AI-generated contributions; that is why this lives here, separately, and nothing from
  it has been or will be submitted upstream.
- No keys, firmware, games or Sony system files are included. You need to dump keys and firmware
  from your own Switch (Lockpick_RCM, TegraExplorer / NXDumpTool) and your own games.

## Expectations, and how far it got

The goal was to run Mario Kart 8 Deluxe on a PS4 Pro through Eden: first get into a race, then make
it fast, then fix the colors. The first part was reached; speed and colors were not.

| Expectation | Result |
|---|---|
| Eden runs as a native PS4 app | Yes |
| A commercial game boots | Yes: MK8D menus at 40-60 fps, Cuphead menu at ~30 fps |
| MK8D gets into a race | Yes: races play, sound is clean |
| Playable speed | No: 13-16 fps in races (projected ceiling on this console ~20-25 fps) |
| Correct colors | No: red and blue are swapped in the whole image (3D, videos and UI) |

**Tested on:** one PS4 Pro (firmware 12.02, GoldHEN); MK8D (base game), Cuphead (menu and game
start) and the Homebrew Menu; handheld mode; sessions of about 15 minutes. **29 test builds** were
run on the console in five days; every one is listed with its result in
[docs/STATUS.md](docs/STATUS.md#every-test-on-the-console). The project stopped there for lack of
time to keep testing.

## What works (v0.1.0, measured on the console)

- Boots with its own game picker (Up/Down + Cross) over the PS4 video output; DualShock 4 input;
  audio through `sceAudioOut` (clean, no crackle).
- **Mario Kart 8 Deluxe**: title screen and menus at 40-60 fps; races load and play (two-lap
  sessions, ~15 minutes without crashing) at **13-16 fps**, with stutters while new shaders compile.
- Cuphead reaches its menu at ~30 fps. The bundled Homebrew Menu (nx-hbmenu) runs without keys.

## What does not

- **Colors**: red and blue are swapped everywhere in the game image: 3D models, videos and UI.
  The sampling/format causes tested were ruled out; the presentation path was never tested and is
  now the first suspect. See `docs/STATUS.md`.
- **Speed**: the emulated CPU is the bottleneck (the GPU is never waited on). See
  `docs/STATUS.md` for the profile and the projected ceiling.
- **Memory**: a race uses ~4.5 GiB of the ~4.6 GiB of direct memory the PS4 gives the process.
  Occasional hangs or crashes when a race loads are likely memory exhaustion.

## Documentation

- [docs/STATUS.md](docs/STATUS.md): goals and expectations, results test by test, profiling,
  projected maximum performance, and where to continue.
- [docs/INSTALL.md](docs/INSTALL.md): installing the release and laying out your own files.
- [docs/BUILDING.md](docs/BUILDING.md): rebuilding everything from source (Windows + Git Bash).
- [docs/TECHNICAL.md](docs/TECHNICAL.md): platform findings (several are useful to any PS4
  homebrew port: the SDK's 16-bit `wmemchr`, the package layout that grants 4.4 GiB, the GPU
  driver's memory behaviour, fault handling, address-space limits).
- [docs/dev-log-es.md](docs/dev-log-es.md): the full development log in Spanish, test by test.

## Branches

- `main`: v0.1.0, the last build that reached races reliably (internally "test 24").
- `experimental-fastmem`: tests 25-29. Fastmem works (an 8 GiB view of guest memory aliasing the
  backing, JIT faults redirected to dynarmic's slow path), plus a direct-memory map in the log,
  the environment moved out of the heap after a heap-corruption crash, and a smaller GPU arena.
  It never got back into a race: loading one runs direct memory out. Notes in `docs/STATUS.md`.

## Credits

Eden and yuzu developers (the emulator), ProsperoEden (the PS5 port this started from, and the Eden
commit it pins), the orbis-ports project (orbis-compat and the PS4 Mesa RADV driver), OpenOrbis
(toolchain), switchbrew (nx-hbmenu), DejaVu fonts. Port by Alejo ([@alechurri](https://github.com/alechurri)).
