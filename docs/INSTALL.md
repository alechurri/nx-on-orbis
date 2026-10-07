# Installing

Tested only on a **PS4 Pro, firmware 12.02, GoldHEN**. Other firmwares or a base PS4 may or may not
work (a base PS4 would also be slower). Read [STATUS.md](STATUS.md) first: this is a closed
experiment, games run but slowly and with wrong colors.

Do the steps in this order: the files go on the console **before** the package is installed, so
the first start already finds them.

## What you need

| File | Where it comes from | Where it goes on the PS4 |
| --- | --- | --- |
| `prod.keys` (and `title.keys` if you have it) | dumped **from your own Switch** with Lockpick_RCM | `/data/edenps4/keys/` |
| the firmware `.nca` files | dumped **from your own Switch** (TegraExplorer / NXDumpTool) | `/data/edenps4/firmware/` |
| your games, `.nsp` or `.xci` | your own cartridges or eShop purchases | `/data/edenps4/roms/` |
| `nx-on-orbis-v0.1.0.pkg` | the [Releases page](https://github.com/alechurri/nx-on-orbis/releases) | installed as a package |

Nothing of the above is included in this project, and nobody here can provide it.

Without keys and firmware the bundled Homebrew Menu still runs, which is a quick way to check the
port itself works on your console.

## Step 1: copy your files over FTP

1. On the PS4, with GoldHEN loaded: *Settings → GoldHEN → Server Settings → Enable FTP Server*.
   Note the console's IP address.
2. In FileZilla connect to that IP, port **2121**, with empty user name and password, and set
   *Transfer → Transfer type → Binary* (text mode damages binary files).
3. Create the folders `/data/edenps4/keys`, `/data/edenps4/firmware` and `/data/edenps4/roms`.
4. Upload `prod.keys` (and `title.keys`) to `keys/`, the firmware `.nca` files to `firmware/` and
   your games to `roms/`. The names must stay exactly as dumped (`prod.keys`, not `prod.keys.txt`).
5. Upload the package to `/data/pkg/` (create it if needed), or put it on a USB drive.

## Step 2: install the package

*Settings → GoldHEN → Package Installer* (or *Debug Settings → Game → Package Installer*), pick
`nx-on-orbis-v0.1.0.pkg` and install it. It appears as **NX on Orbis** (title ID `EDPS00001`).
Installing a newer package over it keeps everything in `/data/edenps4/`.

## Step 3: play

1. Start **NX on Orbis**. The first start copies the firmware into its emulated system memory,
   which takes a moment.
2. Pick a game with Up/Down and press Cross.
3. The first runs of a game stutter while shaders compile; they are cached for later runs.

Controls (Switch layout by position): Circle = A, Cross = B, Triangle = X, Square = Y, L1/R1 = L/R,
L2/R2 = ZL/ZR, Options = +, touch pad click = -, L3/R3 = sticks. Hold **Options + touch pad for 2
seconds** to quit.

## Optional files

| File in `/data/edenps4/` | Effect |
| --- | --- |
| `updates/` | update and DLC `.nsp` files, applied when the game starts |
| `game.txt` | full path of a game to start directly |
| `nomenu.txt` (empty) | skip the game menu and start the last game played |
| `settings.txt` | overrides, one `key=value` per line (below) |

| Key | Values (default first) | What |
|---|---|---|
| `cpu_accuracy` | `unsafe`, `auto`, `accurate` | dynarmic accuracy (unsafe = faster floating point) |
| `gpu_accuracy` | `low`, `high` | GPU emulation accuracy |
| `reactive_flushing` | `off`, `on` | flush GPU data back when the CPU reads it |
| `async_shaders` | `on`, `off` | compile shaders in the background |
| `astc` | `cpu`, `async`, `gpu` | ASTC texture decoding (async and gpu crashed in tests) |
| `profile` | `on`, `off` | sampling profiler in `boot.log` |
| `dyna_state` | `0`-`3` (2) | Vulkan extended dynamic state level |
| `vertex_input_dynamic` | `on`, `off` | |
| `bgra`, `rg`, `a2b10`, `swizzle` | see `frontend/main.cpp` | color self-tests and workarounds (diagnostics) |
| `env` | `NAME=VALUE` | environment for the GPU driver, e.g. `env=ORBIS_ARENA_MIB=1280` |

## Troubleshooting

Everything is logged to `/data/edenps4/boot.log` (the previous run is `boot.old.log`). Download it
before starting the app again.

| Line in `boot.log` | Meaning |
| --- | --- |
| `missing /data/edenps4/keys/prod.keys - dump it from your Switch` | the keys are not where step 1 puts them, or the file name differs |
| `no firmware/ folder; games that need the firmware will not start` | the firmware folder is missing |
| `firmware: N NCAs installed now, ... M failed` | some firmware files could not be copied (damaged or incomplete dump) |
| `no game: put an .nsp or .xci in /data/edenps4/roms` | no game found: the Homebrew Menu starts instead |
| `!! CRASH ...` followed by `eboot+0x...` lines | a crash; symbolize the offsets with the release's ELF: `llvm-symbolizer --obj=nx-on-orbis-v0.1.0.elf -C -f 0x<offset>` |

Other logs: `/data/edenps4/mesa.log` (GPU driver) and `/data/edenps4/log/eden_log.txt` (Eden).

Please do not send reports about this port to the Eden project.
