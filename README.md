# VF4 Final Tuned (Ver. B): native recomp

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20this%20project-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/P5P81EV1M0)

*Virtua Fighter 4 Final Tuned (Ver. B)* (Sega AM2, Naomi 2, 2004) as a
native Windows program, for Windows and for Steam under Proton.

The game's SH-4 program is translated to C and compiled into a Windows
executable **on your PC, from your own MAME set**. The Naomi 2 hardware is
done by the program itself, and the pictures are drawn on your GPU. No
emulator runs at play time.

> [!IMPORTANT]
> This repository **does not contain the game, its code, its data or an
> executable**. You need your own MAME `vf4tuned` set: `gds-0036f.chd` and
> `vf4tuned.zip` (the 317-0387-COM PIC). Everything the build creates comes
> from your set and stays on your PC. Never share those files.

## Installing a release

Download `vf4ft-…-windows.zip` from this repository's Releases page. It
holds the launcher and builder only, no game. You also need your MAME
`vf4tuned` set: `vf4tuned.zip` and `vf4tuned/gds-0036f.chd`.

**On Windows:**

1. Extract the zip into a folder of its own, for example `C:\Games\VF4FT`.
2. Run `vf4ft-launcher.exe`.
3. **Game files:** Use this set (one it found), or Choose folder… and pick
   the folder that holds `vf4tuned.zip`.
4. **Build:** the game goes into `game\` beside the launcher, or elsewhere
   with Change…. Press Build. The first build downloads about 200 MB of
   compiler and takes a few minutes.
5. **Play.** Optional: to have the game in your Steam library too, press
   **Add to Steam** on the Play page. Steam takes a new shortcut only while
   it is closed (it rewrites its shortcuts when it exits), so if Steam is
   open the launcher asks you to exit it (Steam menu, Exit) and then
   choose Try again. The entry gets its pictures in Steam's library (the
   cover, the banner and the icon), made from your own build: the first
   time this takes about half a minute. Press it again any time to
   recreate the shortcut. (Steam has no description for a game added this
   way.)
   Or turn on **Start the game directly** on the Play page: from then on
   the launcher opens the game straight away, and the game's menu has
   **Open the launcher**.

**On Linux or a Steam Deck**, through Steam's Proton:

1. Extract the zip into a folder of its own, for example `~/Games/vf4ft`.
2. In Steam: Games menu, "Add a Non-Steam Game to My Library", Browse, set
   the file type to "All Files", pick `vf4ft-launcher.exe` in that folder,
   "Add Selected Programs".
3. Right-click the new entry, Properties. Its name (the file's name at
   first) can be changed at the top, to Virtua Fighter 4 Final Tuned for
   example. Compatibility: tick "Force the use of a specific Steam Play
   compatibility tool" and choose Proton Experimental.
4. Start it from your Steam library, then do steps 3 and 4 above, and
   press **Play**: the game starts under the same Proton, with Steam
   Input. This entry is how you play from now on (it opens on Play once
   the game is built). To have the entry open the game straight away,
   turn on **Start the game directly** on the Play page; the game's menu
   then has **Open the launcher**.

If Wine is installed (opening `vf4ft-launcher.exe` from your file manager
starts it), steps 2 and 3 can be done by the launcher instead: open it with
Steam closed and press **Add to Steam** on the Play page (after Game files
and Build). It adds itself to your Linux Steam, named Virtua Fighter 4
Final Tuned, set to Proton Experimental and with its pictures; with Steam
open it asks you to exit Steam first. Pressed from the Steam entry, it asks you to exit Steam
and open `vf4ft-launcher.exe` again from its folder.

On a Steam Deck, do steps 1 to 3 in Desktop Mode and step 4 in Game Mode
([more for the Deck](#steam-deck-m6)). Plain Wine is not supported for
playing: the game runs through Steam's Proton. Under plain Wine, Play first
says what to expect (Steam Input missing, Wine's graphics and sound) and
offers Add to Steam, Play anyway or Cancel.

**Two players on two PCs** (on one network, from 0.5.6): both PCs need the
same release. On the PC that hosts, press **Two PCs** on the Play page,
then **Host as player 1** or **Host as player 2**: its game shows this
PC's address and (from 0.5.7) a six-digit join code while it waits. On the other PC
press **Two PCs**, then **Join…**, type that address, press **Next**, type
the code and press **Join**: it is the other player. Each code works once,
and three wrong ones stop it working; the host's player gets a new one in
the game's menu (**Join code**). The host's settings and saves are used;
each player's card stays on their own PC. If the joining player leaves,
the host's game goes on and its player plays alone; from 0.5.7 another PC
can then join that game the same way, with the new code the host's game
shows, and takes over the other player's side, its own card in. If
Windows asks, allow the game through its firewall (it uses UDP port
29404).

**Latency optimization** (from 0.5.11; the launcher's Settings, Game tab,
and the game's menu during a two-PC game) is for a slow connection, such
as busy Wi-Fi or a VPN. Off, each PC waits for the other player's
controls, and the game stops while they are late. On, it guesses them and
goes on, and when a guess was wrong it puts it right at once by running
the last few frames again unseen (rollback). **Auto**, the default, turns
it on while the connection needs it: on a home network it stays off. Each
PC chooses for itself. It takes processor time, and a **Steam Deck may not
be fast enough** for it: if the game slows down on the Deck, set it to Off
there (the other PC can keep it on).
([How it works](docs/design.md#57-latency-optimization-guessing-the-other-players-controls-2026-11-16).)
([How it works](docs/design.md#46-two-pcs-on-a-lan-the-link-the-start-the-checks-2026-11-14);
[choosing a side, playing on alone](docs/design.md#47-two-pcs-the-hosts-side-and-the-host-playing-on-alone-2026-11-15);
[joining a game going on](docs/design.md#49-joining-a-game-going-on-2026-11-15);
[the join code](docs/design.md#50-the-join-code-only-the-pc-told-it-joins-2026-11-15).)

## Screenshots

**The launcher** makes the game from your set in three steps. These are
the Linux launcher's pages; the Windows release is the same program.

| Game files | Build |
|---|---|
| <img src="docs/images/launcher-game-files.png" alt="The launcher's Game files page: it found the set in ~/Games/naomi2"> | <img src="docs/images/launcher-build.png" alt="The launcher's Build page: step 4 of 6, Compiling, 45%, with the builder's last lines and Cancel"> |
| It finds your MAME set, or you choose its folder. | About a minute. Cancel stops it and keeps the game you had. |

| Play | Settings |
|---|---|
| <img src="docs/images/launcher-play.png" alt="The launcher's Play page: Play, Settings, Add to Steam, Build again, Game files, and the Two PCs row"> | <img src="docs/images/launcher-settings-gamepad.png" alt="The launcher's Settings, Gamepad tab: a controller and a set of buttons for each player, each column named as that player's gamepad names them"> |
| Play, add the game to Steam, or build it again. | The game's settings and controls in tabs. The defaults are the PlayStation 2 version's layout. |

| Cards | Card editor |
|---|---|
| <img src="docs/images/launcher-cards.png" alt="The launcher's Settings, Cards tab: three cards, Kage given to player 1 and Lau to player 2, with Kage's character, rank, ring name, record, games left and items"> | <img src="docs/images/launcher-card-editor.png" alt="The card editor's Costume 1 tab: four color parts, hair and eye color, and the item slots"> |
| Each player's card, as on a cabinet. New makes a ready card with every item. | Ring name, character, rank, and each costume's colors and worn items. |

**The game's menu**, over the paused game: Insert or Esc, a gamepad's Guide
button, or View + Menu held for a second. The keys and buttons at its foot
are the ones you last used, here a DualSense's. Settings that apply at the
next start keep a dot until then.

Each kind of gamepad has its own names for the buttons: Xbox One and
Series, Xbox 360, DualSense, DualShock 4 and 3, Switch, GameCube, the
Steam Deck and Steam Controller, and any other pad. In the menus a button
does what its label says (A or ✕ selects, B or ○ goes back, wherever a
Nintendo pad has them); the game's controls go by where the buttons sit.

<img src="docs/images/glyphs.png" alt="Every gamepad input as each kind of pad names it: Xbox, Xbox 360, PS5, PS4, PS3, Switch, GameCube, Steam and generic">

<img src="docs/images/menu-picture.png" alt="The game's menu, Picture tab: the renderer changed to Software, marked to apply at the next start, with DualSense button glyphs">

| Game (keyboard) | Controls (DualSense) |
|---|---|
| <img src="docs/images/menu-game.png" alt="The game's menu, Game tab: Resume, Credits, Card units, Operator test menu, Restart, Quit, with keyboard keys"> | <img src="docs/images/menu-controls.png" alt="The game's menu, Gamepad tab: each player's controller and buttons, changed in the game"> |

## Status

Playable with sound, built on your PC from your own set by the builder
(M5): from the command line or with the launcher. M6 added saves, free
play, the card units, a settings page with rebinding, the GPU renderer at
up to 4 times the resolution, the Steam Deck route, and the operator's
test mode. The game has its own menu over the picture (Insert, Esc, a
gamepad's Guide button, or View + Menu held): it pauses the game, changes
settings, reaches test mode from a gamepad, restarts and quits. Two
players can play each other on two PCs on one network, and a PC can join
a game going on with the host's join code. The latest
release, 0.5.23 (Windows, and the Steam Deck through Proton), is on the
Releases page. The plan, with its
milestones: [docs/design.md](docs/design.md).

| Milestone | State |
|---|---|
| M0: cross build, window, input, Steam/Proton | Done 2026-11-12. `vf4ft.exe` runs under Proton Experimental from Steam (Direct3D 12 through vkd3d-proton by default, Vulkan too) and natively on Linux; with Steam Input on it sees exactly one controller (Steam's virtual pad) and reads stick, D-pad and buttons |
| M1: translator | Done 2026-11-12. Both programs translate to C (6,632 + 2,120 routines) that compiles with no warnings for Windows and Linux; 1.8 million checks against VF4debug's interpreter, none failed ([details](docs/design.md#12-m1-results-2026-11-12)) |
| M2: runtime with the hardware models, headless | Done 2026-11-12. The translated game boots to attract and plays CPU vs CPU matches on VF4debug's hardware models, instruction for instruction the same run as VF4debug's (36,000 attract frames and two whole matches compared hash for hash); about 4 times the real hardware's speed with no picture yet ([details](docs/design.md#13-m2-results-2026-11-12)) |
| M3: picture, input, real time | Done 2026-11-12. The game runs in a window at 60 fps natively on Linux and under Proton (Direct3D 12), with keyboard and gamepads, and gives the same run as VF4debug's; played through coin, start and a match with a DualSense. The picture is VF4debug's software renderer, 8 to 9 ms a frame with the machine while playing ([details](docs/design.md#15-m3-results-2026-11-12)) |
| M4: sound | Done 2026-11-12. The game's own sound driver runs on an ARM7 interpreter with a model of the AICA chip and its effect DSP: music, voices, effects and the game's reverb play in the window on Linux and under Proton, and every run is deterministic ([details](docs/design.md#17-m4-results-2026-11-12), [the DSP](docs/design.md#18-the-effect-dsp-2026-11-12)) |
| M5: builder and launcher | Done 2026-11-12. Your MAME set to an installed game in about a minute, with no MAME tools and nothing to install: the builder reads the CHD itself, keeps the disc's encrypted image (the game decrypts it at start), downloads a pinned compiler and SDL3, translates and compiles. The Windows launcher (with its own Python) built and played the game under Proton in a fresh prefix ([details](docs/design.md#19-m5-results-2026-11-12)) |
| M6: polish | Done 2026-11-12. Saves (the cabinet's backup RAM and settings EEPROM), free play by default the operator's way (or arcade coins), the two card units, `user/settings.txt` with the launcher's Settings page and rebinding. The GPU renderer (Vulkan, natively and under Proton) draws at 1 to 4 times 640 x 480 and matches the software picture; the machine's frame went from 7.0 to 3.1 ms, which puts the Steam Deck at an estimated 10 to 12 ms a frame. The Deck runs the Windows release under Proton (not yet tried on a real Deck). Test mode runs the game's own test program, `test.bin`, and EXIT starts the game again ([details](docs/design.md#20-m6-results-2026-11-12)) |

## How it works

1. The builder reads your MAME set (its own CHD reader: no MAME tools),
   checks it is the supported version, and takes out the DIMM image and its
   key; it decrypts the program it translates.
2. The translator turns every one of the game's 6,632 routines into C. It
   uses the routine tables of the matching decompilation
   ([VF4-Final-Tuned-Rev-B-Decomp-Source](https://github.com/djanice1980/VF4-Final-Tuned-Rev-B-Decomp-Source)).
3. clang (llvm-mingw) compiles that C together with the runtime, which takes
   the place of the Naomi 2 hardware: inputs, the DIMM board, the ELAN and
   rendering chips (drawn with SDL_GPU), and the AICA sound chip.
4. You play the resulting `vf4ft.exe` on Windows, or add it to Steam as a
   non-Steam game and run it with Proton.

## Building the game from your set (M5)

You need your MAME `vf4tuned` set in one folder: `gds-0036f.chd` and
`vf4tuned.zip` side by side, or MAME's own layout (`roms/vf4tuned.zip` and
`roms/vf4tuned/gds-0036f.chd`; then give the `roms` folder). The builder
checks both files and stops with a message if either is not the right one.
It downloads the pinned compiler (llvm-mingw, 84 MB on Linux, 191 MB on
Windows) and SDL3 on its first run, into `~/.cache/vf4ft-builder` (Linux)
or `%LOCALAPPDATA%\vf4ft-builder` (Windows), and checks their SHA-256. A
build takes about a minute on a fast PC.

**From this repository on Linux** (Python 3.12 or later: `python3
--version`; Arch: `sudo pacman -S python`). Replace the two folders with
yours:

```bash
python3 -I tools/make_game.py --set ~/Games/naomi2 --out ~/Games/vf4ft
```

That makes `~/Games/vf4ft/vf4ft.exe` (with `SDL3.dll`, `data/` and `user/`).
To play it from Steam with Proton, close Steam (Steam menu, Exit), then:

```bash
python3 -I tools/steam_shortcut.py add ~/Games/vf4ft/vf4ft.exe
```

Start Steam and play "Virtua Fighter 4 Final Tuned" from the library. For
a native Linux program instead (needs clang and SDL3: `sudo pacman -S clang
sdl3 pkgconf`), add `--target linux` to the first command; that makes
`~/Games/vf4ft/vf4ft`, which you run directly.

After updating this repository (`git pull`), rebuild the install with the
new code. The set is not needed again, and `user/` is kept:

```bash
python3 -I tools/make_game.py --update ~/Games/vf4ft
```

**With the launcher.** A release is a zip with `vf4ft-launcher` and
everything it runs (on Windows also SDL3.dll and Python's embeddable
package, so nothing has to be installed). It walks through three steps,
shown at its top: Game files (choose the set's folder; a set it finds in
the usual places is offered with Use this set, never taken by itself),
Build (where the game goes, with Change…; then the stage, a progress bar,
the time so far and the builder's last lines; Cancel stops it cleanly),
and Play. The Play page
also has Settings (the game's settings and controls, in tabs), Add to Steam
(adds the launcher as a non-Steam game, or recreates that entry; with
Steam open it asks for Steam to be closed first; under Wine on Linux it
adds it to the Linux Steam, set to Proton), Build again, an
Update card when the builder is newer than the game it built, and **Start
the game directly**: with it on, the launcher starts hidden, opens the
game and closes with it, so its Steam entry plays the game; it still shows
itself when it is needed (nothing built, an update, an error), and the
game's menu has **Open the launcher** ([details](docs/design.md#29-start-the-game-directly-2026-11-13)). The mouse,
the keyboard (arrows, Enter, Esc; Q and E switch tabs) and a gamepad
(D-pad, A, B; LB and RB switch tabs) all work it. Its window starts
maximized (and fullscreen when the game's Fullscreen setting says so);
take it out of maximized and it starts as a plain window from then on.
The game's window starts maximized too, unless Fullscreen is on or Window
size names a size
([design.md s70](docs/design.md#70-the-launchers-and-the-games-windows-start-maximized-2026-11-24)).
The game goes into
`game/` beside the launcher unless you choose another folder with Change…
on the Build page: an empty folder or an earlier install is used as it is,
and any other folder gets a `VF4 Final Tuned` folder of its own inside it
(the builder never writes among other files). Changing it copies your
saves and settings into the new folder at the next build; the old folder
stays as it was. The Windows release runs on Windows and, on Linux and
the Steam Deck, under Proton (Steam, or `umu-run`); plain Wine is not
supported ([why](docs/design.md#27-install-folder-set-choice-and-plain-wine-2026-11-13)). The launcher's
window draws as the game's Renderer and GPU settings say, and changes with them
([how](docs/design.md#53-renderer-and-gpu-an-order-to-fall-back-on-changed-at-once-2026-11-15));
when no GPU can draw it, SDL's own order is used, Vulkan first under Proton and Wine
([why](docs/design.md#52-the-launchers-loop-waits-for-work-vulkan-first-under-wine-2026-11-15)).
`SDL_RENDER_DRIVER` picks another of SDL's renderers. To build the launcher from this
repository: `cmake --build build/linux --target vf4ft-launcher` (it needs no
translated code), then `build/linux/vf4ft-launcher`. A release folder is put
together by `tools/package_release.py` (its usage is at its top).

**Updates.** A release checks this repository's Releases page each time
the launcher starts (one request to GitHub, no download). When a newer
release is out, the Play page says so:

- **Update now** downloads it, puts it in place, restarts the launcher and
  builds the game again.
- **Install on next launch** downloads it now; it installs the next time
  you start the launcher, which builds the game again. (Its files are put
  in place as the launcher closes.)

Saves and settings stay. **Check for updates**, at the foot of the Play
page, asks again. A development build never updates itself; only a
release folder does (it has `release.json`). The game added with Add to
Steam starts without the launcher, so its own menu has **Check for
updates** too (Game tab). When a newer release is out, turn on **Update
when I quit**: quitting the game then starts the launcher, which updates,
builds the game again and closes ([details](docs/design.md#24-updates-2026-11-13),
[from the game](docs/design.md#25-updates-from-the-games-menu-2026-11-13)).

Release 0.5.0 has no updater. To leave it, extract the new zip, copy
everything inside its folder into your 0.5.0 folder, replacing the files,
then start the launcher and press Update on its "new builder" card. Your
Steam entry, saves and settings stay.

Whatever the builder makes is made from your copy of the game: never share
the install folder.

## Steam Deck (M6)

The Deck runs the Windows release under Proton, in Game Mode. This has not
been tried on a real Deck yet; the same release was built and played under
Proton on a desktop PC.

1. **A release zip.** Download the latest `vf4ft-…-windows.zip` from this
   repository's Releases page (about 15 MB; it holds no game data). Or make
   one on a Linux PC from this repository: steps 1 to 3 of "Development
   build" below (step 3 makes `build/win64/vf4ft-launcher.exe`), then

   ```bash
   python3 -I tools/package_release.py windows build/win64/vf4ft-launcher.exe ~/vf4ft-release
   ```

   That makes `~/vf4ft-release.zip`.
2. **On the Deck, in Desktop Mode** (Steam button, Power, Switch to Desktop):
   copy the zip over (USB stick, SD card or a network share), right-click
   it in Dolphin, Extract, Extract archive here. Put the folder anywhere,
   for example `~/Games/vf4ft-release`.
3. **Your set** (`gds-0036f.chd` and `vf4tuned.zip`): the launcher looks
   for it in EmuDeck's ROM folders (`Emulation/roms/arcade`, `naomi2`,
   `naomi` or `mame`, on the Deck or the SD card), or in `~/Games`,
   `~/Games/naomi2`, `~/roms`, `~/Downloads` or `~/Desktop`, and offers
   what it finds: Use this set. Anywhere else, pick it with Choose
   folder… on the Game files page.
4. **Add it to Steam:** open Steam on the desktop, Games menu, "Add a
   Non-Steam Game to My Library", Browse, set the file type to "All Files",
   pick `vf4ft-launcher.exe` in the release folder, then "Add Selected
   Programs". Right-click the new entry, Properties: rename it at the top
   if you like (Virtua Fighter 4 Final Tuned, say); Compatibility, tick
   "Force the use of a specific Steam Play compatibility tool" and choose
   Proton Experimental.
5. **In Game Mode**, start it from the library (the Non-Steam tab). The
   D-pad and A work the buttons (B goes back, LB and RB switch tabs):
   Use this set (or Continue), Build (the first run downloads about 190
   MB of compiler, and takes several minutes on the Deck), then Play. The
   game runs fullscreen in the same Proton prefix. Quitting it closes the
   launcher too (the game's menu has Open the launcher to go back to
   it). Settings is where controls, the coin setting, the internal
   resolution and the rest are changed.

The game knows a Deck and which one (LCD or OLED) from its firmware, and
on a Deck it starts fullscreen with the 16:10 picture, which fills the
screen (Settings, Picture changes it). The OLED's screen can run at 90
Hz, and the game's 60 pictures a second look uneven at that rate: the
first start on an OLED says how to set 60 Hz for this game (the Quick
Access button, Performance, Use per-game profile, Refresh rate 60). A PC
whose screen runs at a rate like 144 or 75 Hz gets the same note once. The
game checks the rate the system reports against how fast its own pictures
reach the screen, since the report can be wrong (under Proton, 60 Hz was
reported for screens running at 120 and at 50 Hz). Where it goes by its
own count, the note says so as a guess (design.md sections
[61](docs/design.md#61-the-screens-rate-for-its-notice-held-against-the-presents-2026-11-17) and
[62](docs/design.md#62-the-notices-rate-fewer-presents-count-too-section-61-put-right-2026-11-17)).

**Frame pacing** (Settings, Picture; the game's menu, Picture tab) is how
the game's 60 frames a second meet the screen's refresh.

- **Game clock** (the default, from 0.5.13): the game keeps its own time
  and each frame is put on a refresh of the screen. Where the screen's
  time is a little off the game's (59.94 Hz), one frame is passed over or
  shown again each time the two have gone a whole refresh apart (about
  every 17 seconds at 59.94 Hz), and every other frame is shown evenly.
  It costs a few milliseconds of delay.
- **Screen refresh** (0.5.12's way): on a screen at 60 Hz or a multiple of
  it (59.94, 120, 240) the game follows the screen, so no frame is passed
  over, and runs up to a quarter of a percent fast or slow with it.
- **Off**: the newest frame at each refresh, as before 0.5.12 (uneven for
  seconds or minutes at a time on a screen near 60 Hz).

A two-PC game uses the host's choice on both PCs, and Game clock where
the host chose Screen refresh, since both PCs must keep one time. How it
works: design.md sections
[58](docs/design.md#58-the-games-pace-from-the-screens-refresh-2026-11-17) and
[59](docs/design.md#59-frame-pacing-the-game-by-its-own-clock-2026-11-17).

On the Deck's controls the game's defaults are the PlayStation 2 versions'
layout: X guard, Y and A punch, B kick, L1 throw (P+G), R1 P+K, L2 P+K+G,
R2 K+G, Menu start, View coin (not needed with free play). Holding View + Menu for a second opens the game's menu (below,
"Playing"): settings, the operator's test mode, restart and quit. The Steam
button stays Steam's.

## Development build (CachyOS / Arch)

Steps 1 to 6 build the toolchain and the window. The game itself needs the
translated code (M1, "Translating the game") and the decompilation's files:
"Playing (development, M3 to M6)" below. Until the code is translated, only the
CPU runtime is built and there is no `vf4ft` program.

1. Tools. Each prints a version line:

   ```bash
   pacman -Q cmake ninja curl sdl3
   ```

   If any is missing:

   ```bash
   sudo pacman -S --needed cmake ninja curl sdl3
   ```

2. The Windows cross toolchain (llvm-mingw 20261104) and SDL3 3.4.20 built
   with it, into `toolchain/` (about 400 MB, SHA-256 checked):

   ```bash
   tools/fetch_toolchain.sh
   ```

3. Build the Windows program (`build/win64/vf4ft.exe` + `SDL3.dll`):

   ```bash
   cmake -S . -B build/win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build/win64
   ```

4. Build the native Linux program (`build/linux/vf4ft`):

   ```bash
   cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux
   ```

5. Run the Windows program under Proton without Steam. This needs
   `umu-launcher` (`sudo pacman -S umu-launcher`) and Steam's "Proton -
   Experimental" installed (Steam: Library, search "Proton Experimental",
   Install). The prefix is kept in `build/proton-prefix`:

   ```bash
   cd build/win64 && WINEPREFIX="$PWD/../proton-prefix" GAMEID=umu-default PROTONPATH="$HOME/.steam/steam/steamapps/common/Proton - Experimental" umu-run ./vf4ft.exe
   ```

   Then read `build/win64/vf4ft.log`. Controls and options: "Playing
   (development, M3 to M6)" below.

6. Run it from Steam (Steam Input). Close Steam first (Steam menu, Exit),
   then add the program as a non-Steam game set to Proton Experimental:

   ```bash
   python3 -I tools/steam_shortcut.py add build/win64/vf4ft.exe --name "VF4 Final Tuned (recomp test)"
   ```

   It backs up `shortcuts.vdf` and `config.vdf` beside themselves
   (`*.vf4ft-backup-<time>`) and changes nothing if it cannot read them
   exactly. `python3 -I tools/steam_shortcut.py list` shows the non-Steam
   games. Start Steam, launch the entry, and check `build/win64/vf4ft.log`:
   with Steam Input on, the pad reads "Steam virtual" (vendor 28de). Under
   Proton the real pad behind it is seen too, and a line says it is left
   out ([design.md s30](docs/design.md#30-steam-inputs-twin-pads-2026-11-13)).

## Translating the game (development, M1)

The translator reads the two decrypted programs (`main.bin`, `test.bin`).
In development they come from a
[decompilation](https://github.com/djanice1980/VF4-Final-Tuned-Rev-B-Decomp-Source)
checkout beside this one (`../vf4ft-decomp/work/`, made there from the MAME
set by its `tools/extract.sh`). Its output goes to `work/gen/`, which is
made from your copy of the game: never commit or share it.

1. Translate both programs (Python 3, no packages). Each run prints a
   summary and writes `work/gen/<program>/report.txt`:

   ```bash
   python3 -I translator/translate.py --program main --image ../vf4ft-decomp/work/main.bin --out work/gen/main
   ```

   ```bash
   python3 -I translator/translate.py --program test --image ../vf4ft-decomp/work/test.bin --out work/gen/test
   ```

2. Build as in steps 3 and 4 above: CMake picks up `work/gen/` and compiles
   it into `vf4ft_gen_main` and `vf4ft_gen_test` (warnings are errors).

3. The lockstep tests against VF4debug's interpreter. They need the
   [VF4debug](https://github.com/djanice1980/VF4debug) checkout at
   `../VF4debug` and gcc with 32-bit support; check with:

   ```bash
   pacman -Q gcc lib32-glibc lib32-gcc-libs
   ```

   If any is missing (multilib must be enabled in `/etc/pacman.conf`):

   ```bash
   sudo pacman -S --needed gcc lib32-glibc lib32-gcc-libs
   ```

   Then build and run (about 3 minutes; the last line is the verdict):

   ```bash
   make -C tests/lockstep -j16 test
   ```

## Playing (development, M3 to M6)

`build/linux/vf4ft` and `build/win64/vf4ft.exe` (steps 3 and 4, once
`work/gen/` exists) are the game in a window. Each reads `data/main.bin`
and `data/vf41_s.dec` beside itself, which the build makes as links to
`../vf4ft-decomp/work/` (or `-DVF4_DECOMP=<path>`). (An install made by the
builder has the disc's `data/VF41_S.BIN` and `data/dimm.key` instead; the
game prefers those when they are there.) Linux:

```bash
build/linux/vf4ft
```

Windows under Proton: step 5 above; from Steam: step 6. The start-up
before the warning screen (about 97 s of nothing on the real board) runs
as fast as it can, so the warning screen shows after 2 to 3 seconds. The
game runs at 60 frames a second from then on. It is Japanese settings with
free play: press Start.

The default controls (all of them can be changed, below). The gamepad's
are the PlayStation 2 versions' (VF4 and VF4 Evolution, from their manuals:
square guard, triangle and cross punch, circle kick; Evolution's TYPE A
puts P+G, P+K, P+K+G and K+G on L1, R1, L2 and R2), by position on any pad:

| | Keyboard (player 1) | Gamepad (each player's) |
|---|---|---|
| Move | arrows or WASD | left stick or D-pad |
| Guard / Punch / Kick | J / K / L | X (square) / Y (triangle) and A (cross) / B (circle) |
| Punch + Guard (throw) | U | LB (L1) |
| Punch + Kick / Punch + Kick + Guard | I / O | RB (R1) / LT (L2) |
| Kick + Guard | P | RT (R2) |
| Start | Enter | Menu |
| Coin | 5 (slot 1), 6 (slot 2) | View |
| Test / Service (the operator's) | F2 / F3 | A / X on the operator bar (the menu's Operator test menu) |
| Fullscreen | F11 or Alt+Enter | |
| The game's menu (pause, settings, test mode, restart, quit) | Insert or Esc | Guide (the Xbox, PS, Home or Steam button), or hold View + Menu for a second |

The launcher's Settings and the game's menu have a Gamepad tab and a
Keyboard tab, each a
table with a column for player 1 and one for player 2: every action can
have its own key or button for each player (both players' gamepad buttons
start the same; player 2 has no keys until you set some). The Gamepad
tab's first row is each player's controller: Auto gives the gamepads out
in the order they connected (the first is player 1's), or a player can be
given one of the gamepads connected, which is remembered. Each column
shows its buttons as that player's gamepad names them, so an Xbox pad and
a DualSense side by side show letters in one column and shapes in the
other ([design.md
s69](docs/design.md#69-a-controller-and-buttons-for-each-player-2026-11-24)).

Each pad is one player, also through Steam Input: under Proton the game
sees both Steam's pad and the real pad behind it, and leaves the real one
out, so one press is never player 1's and player 2's at once ([design.md
s30](docs/design.md#30-steam-inputs-twin-pads-2026-11-13)). The buttons
shown are the real pad's (a DualSense's shapes, not Xbox letters).

**The game's menu** pauses the game until it closes, over a blurred copy of
the picture. Its tabs are Game, Cards, Picture, Sound, Gamepad and Keyboard: Q and E (or
LB and RB) switch them. Up and Down choose, Left and Right change, A or
Enter selects, B or Esc goes back (A and B by their labels, so on a Switch
pad A is the right button). The mouse works too: the row under it is
chosen, a click selects it (or changes an option onward), a right click
changes an option back (elsewhere it closes the menu), the wheel moves
through the rows, and the tabs, the Q and E caps, the Ko-fi badge and the
first start's notice buttons can be clicked. The keys or buttons for each are shown
at its foot, as the keyboard or the gamepad last used names them, and the
panel on the right says what the chosen row does. Picture is 4:3, 16:10
(the Steam Deck), 16:9 or 21:9 without stretching (the 3D scene goes on
at the sides; the HUD and the 2D screens stay 4:3 in the middle), or
Stretch. Window size is Maximized by default: the game's window fills
the screen's free room, with the picture at its shape in the middle. Or it
is the window in pixels at the picture's
shape, in the usual screens' sizes from 640 x 480 to 5120 x 2160 (21:9 at
the ultrawide screens' own widths; Auto is the largest of the sizes that
leaves a tenth of the screen free); Internal resolution is the size the GPU renderer draws at, before
the picture is scaled to the window or the screen. Ambient occlusion: SSAO
makes corners, folds and the places where things meet (a foot on the
floor) a little darker, which the cabinet's picture does not have; it is
off by default. Ray tracing: Vulkan has the graphics chip trace rays for
the picture's light: the fighters' shadows get soft edges, the fighters
shade themselves and each other, and glass mirrors the scene; DirectX 12,
in the Windows game (on Windows or under Proton), traces the same picture
through DirectX instead. It needs a
graphics chip with ray tracing; it is off by default. Ray tracing quality
says how much work that is: High traces every pixel of the picture, Medium
(the default) a quarter of them and fills the picture in, which looks
nearly the same for much less work, and Low does that with fewer rays, for
a slow chip. For a PC whose
processor cannot keep the game at full speed, the same tab has Lighting:
GPU (the graphics chip works out the lights) and Frame skip: Every other
frame (the game keeps its full speed and 30 pictures a second are drawn).
With the GPU renderer the graphics chip also draws the reflection a stage
with water has, which the processor drew before
([design.md s78](docs/design.md#78-a-stages-reflection-drawn-by-the-graphics-chip-2026-11-25)).
Picture, window size,
scaling, fullscreen, display, renderer, GPU, internal resolution and volume
change at once (the launcher's window follows Renderer and GPU too). A GPU
the game has drawn on stays open in the game until it quits, also after
you choose another
([why](docs/design.md#60-one-device-a-gpu-for-the-whole-run-2026-11-17)). The
card units are kept for the next start (marked with a dot until then),
and credits (free play or arcade coins), Network and Region apply at the
next restart. Network is the game's own network setting, off by default: off,
the game skips its network check as it starts (there is no network
service yet, so on only waits and reports a time-out). Region is the
board's region, which on a cabinet comes from its BIOS: Japan (the
default) shows the warning screen at power on, win streaks in Japanese
and Shun Di reddening as he drinks; USA, Export, Korea and Australia skip
the warning and show win streaks in English, and USA adds a rating screen
to the attract loop. The game's text is the same in every region. Operator
test menu presses the cabinet's Test switch and shows the operator bar,
where A is Test and X is Service until B or the test menu's EXIT. Restart
the game turns the cabinet off and on (records, settings and cards are
kept); it and Quit ask for a second A. Quit closes the game, and the
launcher too when it started the game; Open the launcher goes back to
it. Changes are saved to
`settings.txt` when the menu closes. The Gamepad and Keyboard tabs are
the launcher's: each player's controller, buttons and keys can be changed
here too, so the launcher is not needed for them
([design.md s72](docs/design.md#72-the-controls-changed-in-the-games-menu-too-2026-11-24)).

**The cabinet's clock** is your PC's: the game starts with your date and
time, as a cabinet runs on its battery clock (in a two-PC game both PCs
have the host's). The game uses the date: in a one-player game the order
of the opponents, and so of their stages, goes by the day of the month.
The log (`vf4ft.log`) says what the clock started at, and names the stage
and the two characters of each match by the game's own numbers
([design.md s77](docs/design.md#77-the-boards-clock-is-the-pcs-and-the-log-names-each-matchs-stage-2026-11-25)).

**Cards** keep a player's character, rank, record, items and colors, as a
cabinet's card did, and a card holds its player to its character: the
select screen picks the card's character and the player cannot move off
it. So no player has a card by default, and every character is free;
giving a player a card is a choice (for tournament-style play with a
fixed character). The launcher's Settings has a **Cards** tab: the cards in
`user/cards`, with each one's character, rank, record and games left.
Player 1 and Player 2 give the chosen card to a player (pressed again:
no card), Edit opens the card editor, New makes a ready card (Akira,
10th kyu, the ring name PLAYER, every item, 500 games) and opens it in
the editor, and Copy, Rename and Delete do what they say (Delete asks
first and moves the file to `cards/Deleted`). The **card editor** has a
Card tab (ring name, character, rank from 10th kyu to 10th dan, games
left, items owned) and a tab for each of the character's two costumes
(four color parts with 19 colors each, hair and eye color, and the four
item slots by item number, 1 to 199). Save writes the card as the game's
own changes do, with its checks; Backspace or Y undoes the changes. A
card never used (the game's new card) becomes a ready card when it is
edited. The game's menu has a Cards tab too: each player's card, chosen
from the same list (None, the default, for no card) or a new one the game
makes, with the chosen card's character, rank, ring name, versus record,
games left and items beside it, swapped in between games (choosing the
other player's card swaps the two; a card the game is using leaves once
the game has written it). Its **Edit player 1's card** and **Edit player
2's card** open the same card editor in the menu (Esc or B saves and goes
back, Cancel at its foot leaves the card as it was); the changes go onto
the card as the menu closes, and onto a card a game is using (from Start
to the end of the match) once the game lets go of it, keeping the game's own record and games left. On a cabinet the ring name was set
on VF.NET and ranks above 10th dan came from it; here the editor sets the
name, and a card goes as far as 10th dan.

**Test mode,** the operator's: Test (F2) opens the game's test menu (its
own test program, `test.bin`, as the cabinet's BIOS starts it). Service
(F3) moves down the menu and Test chooses; screens such as the input test
end with Service and Test together. EXIT starts the game again, from power
on, keeping what the cabinet keeps: the game assignments and book keeping
(`backup.bin`) and the cards. Free play and coins are not in the game's
test menu (they are the BIOS's): they are the `coins` setting below.
The test menu's NETWORK row (Game Assignments) holds through EXIT, until
the game is next opened or restarted from its menu: those put the
`network` setting back in. Test
and Service can be moved to other keys or pad buttons like any control;
with only a gamepad, the menu's Operator test menu does the same.

**The player's folder,** `user/` beside the program (or `--user DIR`):

- `settings.txt`: what the launcher's Settings page writes, and the game
  reads when it starts. One "name = value" a line: `coins` (free play |
  arcade), `fullscreen` (auto | on | off; auto is on for the Steam Deck and
  Big Picture; the launcher's window follows it too), `picture` (4:3 | 16:10 | 16:9 | 21:9 | stretch; the wide
  ones keep the cabinet's screen in the middle, unstretched, and draw what
  the camera sees past its sides; default 4:3, and 16:10 on a Steam
  Deck), `window` (maximized | auto | the window's height in pixels;
  default maximized; a file from before 0.5.21 that says auto, the default
  then, is read as maximized), `filter` (smooth | sharp),
  `renderer` (gpu | software), `gpu` (auto | a GPU's name, as the menu
  lists it), `display` (auto | a display's name), `scale` (1 to 4 times
  640 x 480; default 2),
  `sound` (on | off), `volume` (0 to 100; default 100), `cards` (on |
  off), `network` (off | on; default off: the game's own NETWORK setting,
  written into its settings record as it starts, so it skips its network
  check), `region` (japan | usa | export | korea | australia; default
  japan: the board's region, as the cabinet's BIOS gives it), `latency`
  (auto | on | off: a two-PC game's latency optimization; default auto),
  `pacing` (game | screen | off: Frame pacing, above; default game.
  0.5.12's `pace = timer` is read as off),
  `lighting` (cpu | gpu: which chip works out the lights with the GPU
  renderer; gpu takes that work off a slow processor; default cpu:
  [design.md s67](docs/design.md#67-lighting-on-the-gpu-as-an-option-2026-11-24)),
  `frameskip` (off | half: half draws every second picture, 30 a second,
  while the game keeps its full speed, for a PC whose processor cannot
  keep up; each PC of a two-PC game chooses for itself; default off:
  [design.md s71](docs/design.md#71-a-slow-processor-less-work-on-the-machines-thread-and-frame-skip-2026-11-24)),
  `occlusion` (off | ssao: ssao darkens corners and the places where
  things meet a little, worked out of the picture's depth by the GPU
  renderer; default off:
  [design.md s73](docs/design.md#73-ambient-occlusion-out-of-the-depth-buffer-ssao-as-an-option-2026-11-24)),
  `raytracing` (off | vulkan | directx: traces the light of the GPU
  renderer's picture with the GPU's ray tracing, through Vulkan or
  through DirectX 12; directx is the Windows game's alone (on Windows, or
  the Windows game built for Steam on Linux): the native Linux game takes
  it as off and does not offer it; default off:
  [design.md s74](docs/design.md#74-ray-traced-light-step-1-vulkan-2026-11-24),
  [s75](docs/design.md#75-ray-traced-light-through-directx-12-2026-11-24)),
  `rayquality` (low | medium | high: how much work ray tracing is: high
  traces every pixel of the picture, medium a quarter of them (half the
  internal resolution each way), filled in over the picture, low that
  with a quarter of the rays; default medium:
  [design.md s76](docs/design.md#76-ray-tracing-quality-2026-11-25)),
  then the controls: `key1.punch =
  K` (`key1.test`, `key1.service` the operator's switches), `key2.*` for
  keyboard player 2, `pad1.*` and `pad2.*` for each player's gamepad (a
  `pad.*` line of 0.5.20 or before is both players'), with SDL's key
  and button names, several separated by commas, `-` for none;
  `pad1.controller` and `pad2.controller` (auto, or the gamepad the
  launcher's Settings gave that player). The file
  the launcher writes lists every name with its choices. A line the game
  does not know is skipped, with a message in `vf4ft.log`.
- `backup.bin`: the cabinet's battery-backed RAM and settings EEPROM,
  everything the board keeps with the power off, written every 10 seconds
  when it has changed and when the game ends.
- `cards/`: the card profiles, one file each (`NAME.card`, 128 bytes, the
  card as the cabinet's card unit holds it). `card1` and `card2` in
  `settings.txt` say which is each player's (`none` by default; the
  game's menu also offers a new `p1` or `p2`, made the first time that
  player saves to it). Cards the launcher deletes go to `cards/Deleted/`.
- `kofi-notice` and `refresh-notice`: the first start's Ko-fi notice and
  the screen refresh notice were shown (remove one to see it again).

**The GPU renderer** (`renderer = gpu`, the default) needs Vulkan: it is
used natively, and under Proton through Proton's own Vulkan. It draws on
the GPU chosen in the Picture tab's GPU row. A chosen GPU that is not
there (an external one unplugged) or cannot draw is passed over for the
first in the row's list that works, and Auto (the default) takes the
first that works; the list is in Vulkan's order, which usually puts the
GPU your screen is on first. When no GPU works, or with `--gpu
direct3d12`, the game shows the software picture at 640 x 480, as in M3.
`vf4ft.log` says which renderer and GPU it used, and why.

Options: `--fullscreen`, `--gpu direct3d12|vulkan`, `--frames N` (quit
after N vertical blanks), `--unpaced` (no waiting, for measuring),
`--real-boot` (the start-up at its real pace too), `--image FILE`,
`--dimm FILE` (a decrypted image, or the disc's `VF41_S.BIN` with
`dimm.key` beside it), `--log FILE`, `--silent` (no sound chip: VF4debug's
mailbox model, as in M3), `--user DIR`, `--host [PORT]` and
`--join ADDRESS[:PORT]` (a game on two PCs, UDP port 29404 unless given)
with `--code CODE` (the joining PC: the six digits the host's game shows),
`--host-player N` (the host's player, 1 by default, or 2) and
`--delay N` (its input delay in vertical blanks, 2 by default), and
for tests `--net-lag MS[,JITTER[,LOSS]]` (in a two-PC game, each datagram
this PC sends goes MS ms later, give or take JITTER, and LOSS in 100 are
dropped), `--pace-ppm N` (the game's own timer N parts in a million fast),
`--sync-log FILE` (when each frame was handed over and which frame
each present showed; [design.md s58](docs/design.md#58-the-games-pace-from-the-screens-refresh-2026-11-17))
and `--refresh-first HZ` (the screen's refresh is taken for HZ at first:
[design.md s63](docs/design.md#63-the-refreshs-period-taken-again-when-the-phase-keeps-starting-again-2026-11-17)),
`--hash-every N` (the machine's fingerprint in the log every N
vertical blanks), `--snapshot-at N FILE` (the whole machine at the end of
vertical blank N into FILE) and `--resume FILE` (the run goes on from
FILE, made by the same build; [design.md s48](docs/design.md#48-the-whole-machine-saved-and-resumed-in-another-process-2026-11-15);
neither with `--host` or `--join`), `--press LIST`
(buttons at given vertical blanks, as `6500:p1:start,6800:p1:p+g`),
`--art DIR` (no window: Steam's pictures from the attract sequence into
DIR, as Add to Steam makes them; [design.md s36](docs/design.md#36-steams-pictures-for-the-entry-from-the-players-own-build-2026-11-13)),
`--shot N FILE` (the picture at vertical blank N to a BMP file) and
`--keys LIST` (keys and virtual gamepad buttons one after another from a
vertical blank on, as `6400:Insert,Down,shot=menu.bmp,pad:guide`, with
`shot=FILE` saving the window, menu included). Every 600
frames `vf4ft.log` (beside the program) records how long the machine took
per frame, on average and at most, how much audio is queued, how many
frames the game drew and how many of them were never shown, and what
paces the game (the screen's refresh or its own timer, and why). Sound
plays from the warning screen on.

The shaders' SPIR-V is committed (`runtime/host/gpu_shaders.h`), so
building needs no shader compiler. After changing a shader in
`runtime/host/shaders/`, install glslc (`sudo pacman -S shaderc`) and
run `python3 tools/make_shaders.py`.

The two `.hlsl` shaders there are the DirectX 12 ray tracing's; their
DXIL is committed too (`runtime/host/dx_shaders.h`). To make it again
after changing one, the tool needs Microsoft's own build of its shader
compiler, dxc: from
https://github.com/microsoft/DirectXShaderCompiler/releases take the
`linux_dxc_*.tar.gz` of a release (v1.9.2609's
`linux_dxc_2026_09_28.x86_x64.tar.gz` was used), unpack it into a folder
of its own (`mkdir dxc && tar -xf linux_dxc_*.tar.gz -C dxc`), and run
`DXC="$PWD/dxc/bin/dxc" python3 tools/make_shaders.py` (the tool finds
the compiler's libraries in `lib/` beside `bin/`). Without `DXC` and
without a `dxc` in PATH the tool leaves `dx_shaders.h` as it is. A
distribution's own package of dxc may not do: CachyOS's (1.10.2605) failed
on every shader, and a build that does not sign its DXIL is refused by the
tool, since Windows would refuse the shader. `rt_light.hlsl` and
`rt_light.comp` are the same shader in two languages (and so the two
filters): change both.

## Running the game headless (development, M2)

`build/linux/vf4ft_headless` (built by step 4 above once `work/gen/` exists)
is the translated game on VF4debug's hardware models, with no window. It
runs on virtual time and is driven by VF4debug's controller scripts. It
needs the [VF4debug](https://github.com/djanice1980/VF4debug) checkout at
`../VF4debug` (only its `tools/gen_tables.py` is run, at build time) and the
decompilation checkout at `../vf4ft-decomp` (the program image, the DIMM
image and the routine tables). Other places: `cmake -S . -B build/linux
-DVF4DEBUG_DIR=<path> -DVF4_DECOMP=<path>`.

1. Boot to the attract loop: 9,000 frames, then where the game is. The
   closing lines also count what ran on the interpreter instead of the
   translation:

   ```bash
   build/linux/vf4ft_headless --frames 9000
   ```

2. Draw what it shows into PNG files (`view/f<frame>.png`, every 600th
   frame):

   ```bash
   build/linux/vf4ft_headless --frames 9000 --view view --view-every 600
   ```

3. Check that a run is VF4debug's run. The script's `hash` lines fingerprint
   all of main RAM and the registers; this builds VF4debug's own binary
   from its checkout (into `build/vf4debug-ref`, needs gcc with 32-bit
   support as in "Translating the game" step 3), runs both on the script,
   and prints `same` or the first frame where they differ. A script that
   boots and hashes every 100 frames to frame 9000, then a check:

   ```bash
   python3 -c "open('build/every100.txt','w').write('run 100\nhash\n' * 90)"
   ```

   ```bash
   python3 -I tools/compare_runs.py build/every100.txt
   ```

   VF4debug's `scripts/` (a CPU vs CPU match, a two-player intrude match)
   work the same way once `hash` lines are added after their commands.

`--interp` runs the same binary on VF4debug's interpreter instead of the
translation (for comparisons; slower). `--sound chip` runs the sound chip
and the game's driver instead of VF4debug's mailbox model (the default,
which the comparisons need), and `--wav FILE` writes what it plays:

```bash
build/linux/vf4ft_headless --frames 9000 --sound chip --wav build/attract.wav
```

`--help` lists the rest: they are VF4debug's options.

`meta/` holds the routine lists, global lists and FPU modes taken from the
decompilation (addresses and names only), for both programs. After the
decompilation changes, refresh it with
`python3 -I tools/sync_meta.py ../vf4ft-decomp` and translate again; the
build makes the runtime's tables (`tables.c`, `tables_test.c`) from it.

## License

MIT, for this project's own work: see [LICENSE](LICENSE). The game and
everything made from it belong to Sega.

Others' work in it:

- The fonts of the launcher and the game's menu, Noto Sans and Noto Sans
  Mono (cut down by `tools/make_fonts.py` into `runtime/ui/fonts.c`): SIL
  Open Font License 1.1, [runtime/ui/OFL.txt](runtime/ui/OFL.txt). The
  builder puts a copy in the game's folder as `Font license.txt`.
- [stb_truetype](runtime/ui/stb_truetype.h) 1.26, which draws them: public
  domain or MIT, as its own end says.
- SDL3, which the builder downloads and the Windows release carries as
  `SDL3.dll`: zlib license.
- [Monocypher](runtime/crypto/ORIGIN.md) 4.0.3, the cryptography of the
  two-PC game's join code: used under CC0 1.0, one of the two licenses its
  [LICENCE.md](runtime/crypto/LICENCE.md) offers (the other is BSD
  2-clause, whose notices are kept).
- Ko-fi's "Support me on Ko-fi" button
  ([runtime/ui/kofi_badge.png](runtime/ui/kofi_badge.png)), which Ko-fi
  gives creators to link to their page: it links to this project's.

## Support

If the recomp is useful to you, you can support its development:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/P5P81EV1M0)

(In the game: the menu's Game tab, **Support on Ko-fi**.)
