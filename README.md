# GWars:Touch on the PS Vita

On the Vita it is called *Geometry Wars 2: Portable*.

A PS Vita port of GWars:Touch, all ten single-player modes, (Activision, 2010,
iPhone/iPad), built by reverse engineering the iPhone program.

The rules of play were recovered from the game's own binary (`GWTouchPad`
1.2.0, armv7 slice) by disassembly and decompilation, then written out in C.
That covers the spawn director, each enemy's movement, the black hole, the gun,
geoms, scoring, lives and bombs. Each rule in `src/game.c` names the function
and address it came from. The method and the full write-ups are in `../notes/`
and `../decomp/` (beside this folder in `gwtouch-reverse`).

This is not an emulator: the ARM code is not run on the Vita. The C here is
this project's own, and so are the art and text: everything is drawn as
glowing lines. Only the **sound effects and music** come from the game, and
`tools/prep_assets.py` converts them from your own copy.

## Where the rules came from

| Rule | Source in the binary |
|---|---|
| Fixed 60 ticks/s; movement = velocity x dt x 0.75 | `gTick`, `Entity::Update` 0x2ad1c |
| Arena +-300 x +-200 | `kMapWidth`, `kMapHeight` |
| Spawn director: timer `clamp(2 - 0.02 x level, 0.275, 2)`, level +0.7 per spawn, brackets 2/5/10/40, corner swarms, rings, mixed rush, rocket walls, hurry rule, 3 s pause after a death | `LevelControl::TickStateNormal` 0x5baf4, `LevelControl::Update` 0x5c97c |
| Grunt, Wanderer, Weaver (dodges bullets within 64), Spinner (splits into 3), TinySpinner, Snake (23 bullet-proof body segments), Proton, Rocket, Duck | each class's `Tick` / `Damage` (see `notes/Enemies.md`) |
| Black hole: 6 hits to kill (`StartHits`), bursts into 15 Protons above mass 12 (`MaxHits`), asleep until shot, every hit shoves everything within 256, pull of 16/d on the ship and 1500/d^2 on enemies, bends bullets, eats enemies, 125-unit blast ring, kills in the blast worth 5x | `GravityWell::Damage` 0x2e99c, `ApplyForce` 0x2ed00, `Tick` 0x2f448, `Killed` 0x2ead4 |
| Gun: 3 bullets (speeds 520/600/520, +-0.03 rad) every 0.1 s | `Player` fire code (see `notes/Player_gun.md`) |
| Geoms: count per enemy type, values from {1x12, 2x4, 5x2, 10}, last 4.225 s, magnet range 45; they raise the multiplier | Geom/Mote code (see `notes/Scoring_and_player.md`) |
| Points: `floor(mult x 0.25 x base)`, rounded down to a multiple of 5 | scoring code |
| Lives and bombs: start with 3 of each; one more of each at 100,000, then each further x10; at most 9 | same |
| Bomb: ring of radius 400 growing at 533 units/s; geoms still drop, no points | `SmartBomb` |
| Player: thrust 50, damping 0.8; comes back where it died after 1 s | `Player::Tick` |
| Every tuning number (colours, speeds, radii) | `Debug::VarBase` table, recovered by emulating the 77 static initialisers |

### Two things the binary settled that differ from memory

- **Bombs are not dropped by enemies in single-player Evolved.** Pickups come
  only from `SpawnBonus`, and only multiplayer calls it. In single player
  the extra bombs come from score: 100,000, then 1,000,000, and so on.
- **The gun never powers up in Evolved.** The 7-bullet gun (every 1/15 s)
  belongs to the Weapon pickup, and that is multiplayer only too.

### Still approximate

Read only in part, so these are close but not exact: the Gate (what triggers
it, blast radius), the Repulsar's dash and how it deflects shots, how hard
geoms are pulled in (`UpdateMotes` not read), how long the ship is protected
after respawning (about 2 s), the radii of the ring spawns (120/170), the
spacing of the wall spawns, and how long snake body segments last after the
head dies.

### The other nine modes

Choose the mode on the title screen with up/down. Each mode's setup function
(`GrEvolved`, `GrCasual`, `GrPacifism` and the rest, 0x4e58c-0x4e910) sets its
lives, bombs and rules. `UpdateGameControl` (0x5b910) then hands spawning to
one of three directors: `LevelControl::Update` (per-mode branches),
`CasualGameUpdate` (Deadline's adaptive budget) or `NewGameControlUpdate`
(Sequence). Everything is written up in `../notes/Modes.md`.

| Mode | What the binary says |
|---|---|
| Deadline | 3 minutes, endless ships. Each 5-second wave is sized from what you killed and collected in the last one; dying or bombing shrinks it |
| Pacifism | no gun (`Player::FireProjectile`); clumps of Grunts from a corner, and gates. Flying through a gate sets off a 70-unit blast at each end; chains of gates score 100 x chain^2 |
| Waves | walls and half-walls of rockets from shuffled sides; more come with every level |
| King | three zones (`Hill`). You can only fire from inside one, and enemies are pushed out of them. A zone fades 2.5 s after you enter it and a new one appears elsewhere. Scores are doubled |
| Sequence | 15 hand-made levels, 30 s each, played in the game's own order. The level scripts are decoded from the game's `Data.bin` by `prep_assets.py` |
| Minelayer | no gun; three minelayers that can't be destroyed lay mines. A mine goes off a second after you pass it, and its shockwave kills enemies for 5x points. NUFO formations cross every 5 levels |
| Titans | big enemies (15 hits) that split into two (8 hits), which burst into a swarm of their kind |
| Whirlpool | a black hole in the middle swirls everything round (ship included) and reverses every 6-10 s |
| Meteor | 32-meteor showers cross the arena, plus wanderers and grunts; rocket walls after level 30 |

Not exact: the NUFO's weaving, how the titans wander, the Amoeba (it appears
in only two Sequence levels and is simplified), and the screens between
Sequence levels. Touching the black hole, a mine or a titan isn't fatal in this
port. That's inferred from a shared flag, because the original creates the black
hole on top of the ship. Evolved's rocket walls still use the port's earlier
side order. The camera shows the whole arena rather than the iPhone's scrolling
view, and the original menus are not reproduced.

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

This makes `app_data/sfx/` (119 sound effects), `app_data/music/` (11 tracks),
`app_data/levels.txt` (the Sequence level scripts)
and `app_data/manifest.txt`, about 28 MB. Without ffmpeg it still makes the
sound effects; run it again later to add the music. **Do not share app_data or a
VPK built with it**: it is the game's sound.

The app's music is mono AAC at about 144 kbps; it is converted at Vorbis
quality 6. For better music, put your own copies of the tracks (flac, wav, mp3,
m4a...) in a folder, named like the game's (`Pacifism.flac`, `Main Menu.mp3`,
`Retro Evolved.wav`, ...), and they are used instead, in stereo at quality 7:

```sh
python3 tools/prep_assets.py ~/Downloads/GWarsTouch_v1.2.0_os30.ipa app_data --music-dir ~/Music/gw
python3 tools/prep_assets.py ~/Downloads/GWarsTouch_v1.2.0_os30.ipa app_data --redo-music   # the app's own, again
```

If something looks wrong on the Vita, `ux0:data/gwvita/log.txt` lists the
renderer and any draw or texture the Vita refused.

`prep_assets.py` also makes `app_data/icon0.png`, the Vita's home-screen icon,
from the app's own icon (the IPA's 512x512 `iTunesArtwork`, at 128x128 in 8-bit
colour), or from a picture of your own with `--icon my_icon.jpg` (a round picture
on black is trimmed to fill the Vita's round icon); the build uses it in place of
the placeholder in `sce_sys/`.

The start-up splash (shown for 9 seconds while the sounds load) is
`app_data/splash.bmp`, made from a picture of your own:

```sh
python3 tools/make_splash.py my_splash.jpg app_data/splash.bmp
```

Without it the splash is the game's name on black. The ship's outline is the
`player` model from `Data.bin` (Model record 28), read with the game's own
reflection schema (`Model`, `Part`, `SectionMesh`, `MeshSegment`, `Vertex`).

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
undefined-behaviour sanitizers) and checks it against the recovered rules:
the gun's rate (15 bullets in 0.5 s), a Grunt kill worth 10, geoms raising the
multiplier, a sleeping black hole not pulling, one volley waking it, three hits
not killing it, its pull on the ship, an overfed one bursting into Protons, the
bomb ring, the Spinner splitting, the snake, pausing, losing all ships, and the
high score being saved and loaded. Snapshots of the screens go in the folder
given.

## Threads

The game runs on three of the Vita's cores: the main thread hands each
finished frame to the GPU and waits for the display; the game thread runs the
game and builds the next frame's vertices meanwhile. A helper on the third
core moves the background grid, builds the grid's and the sparks' lines and
turns them into GPU quads (`plat_batch_lines`), all while the game draws
everything else. The game's core then adds that finished block to the frame
with one copy. At start-up the Vita build sets full clock speeds: 444 MHz
ARM, 222 MHz bus and GPU, 166 MHz GPU crossbar. Set `GW_THREADS=0` (desktop)
to run everything on one thread.

## Layout

```
src/game.c       the game: recovered rules (spawning, enemies, black hole, gun,
                 scoring), drawing, grid, particles, screens
src/plat_sdl.c   SDL2 platform layer (Vita and desktop): sticks, audio, batched drawing
src/plat.h       the interface between the two
tools/prep_assets.py   .ipa -> app_data/ (sound effects, music, Sequence levels)
tools/livearea_png.py  art -> 8-bit indexed PNG for sce_sys/ (icon and LiveArea)
sce_sys/         icon and LiveArea images (placeholders, this project's own)
test/            headless test: software renderer and a scripted game
```
