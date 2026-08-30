# Starfighter

A Linux port of the 2006 Starfighter arcade game, rendered with SDL2.

This repository contains the port in `src/` and the game assets it loads in
`assets/`.

## Build the Linux port

SDL2, FreeType 2, and a C++17 compiler are required. On Arch/CachyOS:

```sh
sudo pacman -S --needed make pkgconf sdl2 freetype2 unzip
```

On Raspberry Pi OS/Debian:

```sh
sudo apt install make g++ pkg-config libsdl2-dev libfreetype6-dev unzip
```

```sh
make
./build/bin/starfighter --assets assets
```

Run the unit and smoke tests with:

```sh
make test
```

For an arcade cabinet, start borderless fullscreen and hide the pointer:

```sh
./build/bin/starfighter --assets assets --cabinet
```

Two game modes are available via `--mode`:

- `arcade` (default): the shipped dipswitch difficulty; enemies fight back
  with a per-enemy missile scheduler;
- `recruit`: practice mode - enemies hold their fire entirely and every
  weapon rapid-fires without spending its gauge or ammunition.

```sh
./build/bin/starfighter --assets assets --mode recruit
```

Start a game with `5` (coin), followed by Space, left Alt, left mouse, or `1`.
During play:

- mouse or `I/J/K/L`: aim the independent reticle;
- arrows or `W/A/S/D`: rotate the turret;
- Space, left Alt, or left mouse: fire;
- `M`, comma, period: select laser, particle beam, or photon bolts;
- `1`, Enter, or right mouse: cycle weapons;
- `3`, `B`, or middle mouse: activate Death Blossom when available;
- `0`: diagnostics from attract mode; `F11`: fullscreen; Esc: quit.

Up to two SDL joysticks are supported: joystick 1 rotates the turret and
joystick 2 moves the reticle. On a single dual-stick controller, axes 0/1 are
the turret and axes 2/3 the reticle; either controller's buttons fire. Select
the shipped Spanish dialogue with `--voice es` (`--voice en` is the default).

Cabinet features mirror the release: the attract loop cycles the Star League
emblem and high-score table with a rotating showcase of each enemy, a
greeting sample and rendered attract music play in the background, `0` opens
an editable diagnostics bank for every dipswitch (UP/DOWN select, LEFT/RIGHT
adjust, `0` persists to `starfighter-diagnostic.dat`), coins feed a real credit ledger priced
by the dipswitches, finished games that place in the top ten prompt for
initials, and the table persists to `starfighter-scores.dat` beside the
working directory so the shipped arrays stay untouched. During play an
engine loop runs under the effects and Death Blossom has its own whine; the
shipped `Strftr music.mid` is rendered by a built-in wavetable voice.
Operators who prefer different attract audio can drop any WAV at
`Sounds/attract-music.wav` inside the asset directory; it replaces the
rendered MIDI without touching anything else.

Rendering uses an 800x600 logical canvas with
automatic 4:3 letterboxing, so the same executable behaves consistently on a
Raspberry Pi or x86 Linux system and does not stretch on a widescreen panel.

The port runs the complete default level order from target practice through
the Ko-Dan command ship. It includes the recruitment and
Gunstar launch intro, the shipped voice and effect recordings, shipped
Gunstar, enemy, target, meteor, missile, and command-ship meshes, weapons,
damage, scoring, results, continue, and victory flow. The target-practice
lifecycle matches the original game.

For visual inspection of the release meshes and their flight-facing angles:

```sh
./build/bin/starfighter --assets assets --model-gallery
```

To capture a particular point in the recruitment intro without
waiting in real time:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./build/bin/starfighter --assets assets \
  --intro-preview-ms 4000 --screenshot build/intro-smoke.bmp
```

The renderer is portable C++/SDL2 and uses no x86 intrinsics, assembly, or
endianness-dependent model reads. Keep that constraint for Raspberry Pi 5.

The code is licensed under MIT (see `LICENSE`). The game and its media are a
non-commercial fan work based on copyrighted material; this repository is
intended for personal, archival, and educational use only.
