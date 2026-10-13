# TwistedMetal1234Recomp

Static recompilations of the four PlayStation Twisted Metal games as native
Windows programs, built on [psxrecomp](https://github.com/mstan/psxrecomp) and
[recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui) with a unified launcher.

| Id | Game | Serial | Supported disc (USA) |
|---|---|---|---|
| `tm1` | Twisted Metal | SCUS-94304 | retail release |
| `tm2` | Twisted Metal 2 | SCUS-94306 | retail release |
| `tm3` | Twisted Metal III | SCUS-94249 | Rev 1 |
| `tm4` | Twisted Metal 4 | SCUS-94560 | Rev 1 |

Added on top of the original games:

- Widescreen, 16:9 or 21:9
- 30 and 60 FPS modes for Twisted Metal 1 and 2
- Smooth Motion enabled for Twisted Metal 4.
- Synced display, audio and control settings by all games.

This is an unofficial fan project. It is not affiliated with or endorsed by
Sony Interactive Entertainment.

## Play

You need your own disc image of each game you want to play (`.chd`, or `.cue`
with its `.bin`), in the version listed in the table above.

1. Download the latest release zip and unzip it anywhere.
2. Run `TwistedMetal1234Recomp.exe`. The cover art downloads in the
   background the first time.
3. Press **Play** on a game. The first time, its launcher opens: choose your
   disc image, then click Play.
4. **Settings** opens the psxrecomp launcher at any time to update display, audio, controls,
   memory cards.

Default settings are 16:9 ratio, borderless fullscreen, vsync, 60 FPS enabled for Twisted Metal 1 and 2, and Smooth Motion on for Twisted Metal 4.

## What is and is not in this repository

The repository holds configuration, build files, patches and small code hooks.
It does **not** contain disc images, a PlayStation BIOS, game assets, cover
art, or recompiled game code. The recompiled C for each game is generated on
your machine from your own disc (`tools/generate.sh`), and game data is read
from your disc image when a game runs. The games run on the open-source
OpenBIOS bundled with psxrecomp.

Release zips contain the built programs, which include a machine-translated
build of each game's own code, plus OpenBIOS. They contain no disc data, game
assets, retail BIOS or cover art.

## Code hooks in this repository

These are hidden from the launcher's Mods list and run on their own.

- `tm1.framerate`, `tm2.framerate`: the frame rate patches behind FPS Mode,
  and the corrections that keep handling, steering, smoothing and weapon
  timing the same per second at 30 and 60 as at the original 20.
- `tm3.wideview`, `tm4.wideview`: the games draw terrain from lists prepared
  for a 4:3 view, which leaves holes at the edges of a 21:9 picture. These
  also draw the neighbouring camera headings' terrain while the view is wider
  than 16:9, and tell the runtime which draw packets are HUD on each level.

## Framework patches

Both submodules stay pinned to upstream commits; `tools/setup_framework.sh`
applies these to their working trees.

- `psxrecomp-launcher-aspect-ratio.patch` lets the launcher offer 16:9 and
  21:9. Stock psxrecomp leaves widescreen to per-game mods.
- `psxrecomp-launcher-fps-mode.patch` adds `[video] fps_mode` to
  settings.toml and switches a game's hidden `fps_mode` mod feature from it
  at launch.
- `psxrecomp-second-hud-packet-range.patch` adds a second HUD packet range.
  Twisted Metal III and 4 build their HUD in two alternating buffers.
- `psxrecomp-shared-settings.patch` keeps the shared settings sections and
  the button binding files in one place next to the menu, and adds
  `[video] frame_generation_allowed` so the three games Smooth Motion does
  not suit ignore the shared switch.
- `psxrecomp-widescreen-hud-rows.patch` moves HUD primitives that form one
  row together, so centred text that spans the screen is not pulled apart
  when the HUD is re-anchored to the wide corners.
- `recomp-ui-display-settings.patch` turns the launcher's aspect-ratio and
  window size controls into dropdowns (window sizes go up to the desktop
  resolution) and adds the FPS Mode dropdown and the render thread, present
  thread and Smooth Motion checkboxes to the Display settings.

## License

PolyForm Noncommercial 1.0.0; see `LICENSE`. psxrecomp is under the same
license and recomp-ui is MIT; the patches here are changes to those projects.

Twisted Metal is a trademark of Sony Interactive Entertainment, and the games
are copyright their owners. This repository contains none of their code, data
or artwork.
