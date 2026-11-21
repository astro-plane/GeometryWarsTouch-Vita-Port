# Neon Grid – a GWars:Touch style shooter for the PS Vita

A twin-stick arena shooter written from scratch in C for the PS Vita, in the
manner of GWars:Touch (Activision, 2010, iPhone). It does not run or translate
the iOS program, and it contains none of the game's code or art: every ship,
enemy, grid line and letter is drawn by this project's own code as glowing
lines. The only things taken from the iPhone game are its **sound effects and
music**, converted from your own copy by `tools/prep_assets.py`.

"Neon Grid" is a placeholder name.

## Status

First playable version: the **Evolved** mode (endless, three ships, three
bombs).

- Twin-stick controls on the Vita's two analogue sticks
- A spring grid under the arena that warps around explosions, bombs and
  gravity wells
- Six enemy types: seeker (chases you), dodger (steps out of the way of shots),
  drifter (wanders), splitter (bursts into three shards when hit), shard, and
  gravity well (pulls in your ship, your shots, geoms and other enemies; it
  bursts if it swallows too many)
- Geoms: shot enemies drop them; picking them up raises the score multiplier,
  which resets when you lose a ship
- Bombs clear the field (no points); an extra ship every 75,000 points and an
  extra bomb every 100,000
- Swarms from the four corners every 30 seconds
- High score, saved on the memory card
- The game's own sound effects and music

Not yet: the other modes (Deadline, King, Pacifism, Waves, Sequence, Titans,
Meteor, Minelayer, Black Hole), the original menu text, and fine-tuning against
the original.

## What you need

1. **Your own copy of the game**: `GWarsTouch_v1.2.0_os30.ipa`, GWars:Touch
   version 1.2.0 (bundle id `com.activision.gw3pad`), about 42 MB.
2. **Python 3** for the converter; **ffmpeg** for the music (the sound effects
   need nothing else). Any ffmpeg will do: the converter uses libvorbis when
   ffmpeg has it, and ffmpeg's own Vorbis encoder when it does not.
3. **VitaSDK**, with: `vdpm install sdl2 libvorbis libogg`

## Making app_data

```sh
python3 tools/prep_assets.py ~/Downloads/GWarsTouch_v1.2.0_os30.ipa app_data
```

This makes `app_data/sfx/` (119 sound effects), `app_data/music/` (11 tracks)
and `app_data/manifest.txt`, about 28 MB. Without ffmpeg it still makes the
sound effects; run it again later to add the music. **Do not share app_data or a
VPK built with it**: it is the game's sound.

## Building

```sh
export VITASDK=/usr/local/vitasdk      # wherever yours is
export PATH="$VITASDK/bin:$PATH"
cmake -S . -B build
cmake --build build                    # -> build/gwvita.vpk
```

The VPK carries `app_data/` inside it (it is small), so there is nothing to
copy by hand. To keep it out, configure with `-DEMBED_DATA=OFF` and copy
`app_data/` to `ux0:data/gwvita/app_data/` instead. Without app_data the game
runs, silent.

A desktop build for testing (needs SDL2 and libvorbis):

```sh
cmake -S . -B build-pc -DDESKTOP=ON && cmake --build build-pc
GW_DATA=app_data ./build-pc/gwvita
```

## Controls

| Vita | Desktop | |
|---|---|---|
| Left stick (or d-pad) | WASD | move |
| Right stick (or △ ○ ✕ □) | arrow keys | fire in that direction |
| L or R | Space | bomb |
| START | P | pause; then ○ quits to the title |
| ✕ | Enter | start / continue |

## Testing

```sh
test/run_tests.sh app_data /tmp/gwvita_shots
```

Plays the game headless with simulated sticks (software renderer, address and
undefined-behaviour sanitizers) and checks starting, firing, scoring, the
multiplier, bombs, pausing, losing all ships, and the high score being saved
and loaded. Snapshots of the screens land in the folder given.

## Layout

```
src/game.c       the game: drawing, grid, particles, ship, enemies, scoring, screens
src/plat_sdl.c   SDL2 platform layer (Vita and desktop): sticks, audio, batched drawing
src/plat.h       the interface between the two
tools/prep_assets.py   .ipa -> app_data/ (sound effects and music)
tools/livearea_png.py  art -> 8-bit indexed PNG for sce_sys/ (icon and LiveArea)
sce_sys/         icon and LiveArea images (placeholders, this project's own)
test/            headless test: software renderer and a scripted game
```
