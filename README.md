# littlewing-shim

Runs three PowerPC Mac pinball games by LittleWing natively on Apple Silicon:
*Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1* (2003, Carbon programs for
Mac OS 9 and X) and *MONSTER FAIR 1.2.5* (2010, a Mac OS X application). It
emulates their CPU (Unicorn) and reimplements the Mac OS calls they make in C.
This repo contains no game files: you need your own copies of the games. The
one exception is `hd-art/`, enlarged art made from the game's pictures (see HD
art below). It is an unofficial project, not made or endorsed by LittleWing.

## Build

```bash
brew install unicorn sdl3 cmake pkg-config
cmake -S . -B build                                     # Debug (sanitizers): for development
cmake --build build
./build/loony_tests          # all tests; ./build/loony_tests <substring> to filter, =<name> for one
LOONY_SLOW_TESTS=1 ./build/loony_tests  # also the slow tests (minutes of play each)
./build/loony_tests -j 1     # one at a time in one process (for a debugger)

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release  # for playing
cmake --build build-release
cmake --build build-release --target app                # build-release/LittleWing.app
```

## Play

A release needs an Apple Silicon Mac with macOS 14 (Sonoma) or later. An app you build
yourself needs the macOS you built it on, or later, because Homebrew's Unicorn
and SDL3 do. It plays the games from these places, which must hold the game's
file:

| Game | Where | File | Download |
|---|---|---|---|
| Loony Labyrinth | `/Applications/Loony Labyrinth` | `LOONY LABYRINTH 3.0.1` | `loony_labyrinth_301a.dmg` |
| Crystal Caliburn | `/Applications/Crystal Caliburn` | `CRYSTAL CALIBURN 3.0.1` | `crystal_caliburn_301a.dmg` |
| MONSTER FAIR | `/Applications/MONSTER FAIR.app` | `Contents/MacOS/MONSTER FAIR` | the Mac OS X version, 1.2.5 (the only version supported) |

To get a game, download it from LittleWing's
[download page](http://www.littlewingpinball.com/doc/en/downloads/index.html),
open the disk image, and drag the game onto the Applications shortcut next to
it: the game's folder for Loony Labyrinth and Crystal Caliburn, the app itself
for MONSTER FAIR. Keep its name. MONSTER FAIR's own app doesn't run on Apple
Silicon; LittleWing.app runs it.

Download `LittleWing-<version>.zip` from
[Releases](https://github.com/gregjkal/littlewing-shim/releases), unzip it,
and move `LittleWing.app` to `/Applications` (or anywhere); or build it as
above and copy `build-release/LittleWing.app`. Double-click it. With two or
three games installed, it opens on a picker showing each game's title picture
(MONSTER FAIR's card shows its icon): Left and Right (or the mouse) choose,
and Return (or a click) plays. It starts on the last game played. With one
game installed, it plays that game. Choosing QUIT in a game's own menu goes
back to the picker; Cmd-Q or closing the window quits. The app carries its own
copies of Unicorn and SDL3, so Homebrew upgrades don't affect it. A release is
notarized and opens without a warning, and updates itself: it checks for a new
release once a day, and LittleWing → Check for Updates… checks now. A new
version found at launch, or after the Mac has sat idle, is offered in a
window; one found while you play only retitles that menu item ("Update to
LittleWing 0.5.0…"), so play isn't interrupted. Installing replaces the app
where it is and reopens it. Built as above, it is signed ad hoc, which is
enough on the Mac that built it. On another Mac, Gatekeeper blocks the first launch;
open System Settings, then Privacy & Security, and click Open Anyway near the
bottom. (On macOS 15 and later, right-click and Open no longer gets past it.) To give
the app to someone without that step, sign and notarize it (below). When the app can't
start or the game crashes, it says so in a message box; its log is
`~/Library/Logs/littlewing-shim/loony.log` (the run before is kept as
`loony.previous.log`).

From a terminal:

```bash
./build-release/loony                       # like the app: the picker, or the one game installed
./build-release/loony "/path/to/game folder"   # the game in that folder, no picker
./build-release/loony "/Applications/MONSTER FAIR.app"   # MONSTER FAIR, no picker
LOONY_GAME=crystal-caliburn ./build-release/loony   # that game, from its usual place
```

`LOONY_GAME` takes `loony-labyrinth`, `crystal-caliburn` or `monster-fair`.

Until it is registered, each game first shows two windows: the shareware screen
(Play Demo, Buy Now, Enter Key-Code, Quit) and the key list. Click a button, or
press Return for the outlined one. To register, click **Enter Key-Code**, type
or paste (Cmd-V) your e-mail address and key code, using Tab to move between
the fields, and click **Register** (or press Return). The license is saved with
the preferences, so later launches go straight to the game. For testing Loony
Labyrinth, `docs/test_key.txt` has an e-mail address and key code that were posted
publicly on [archive.org](https://archive.org/details/littlewing-pinball/).

The game then plays its opening and a self-playing demo. All three games use
the same keys by default (MONSTER FAIR adds X and . to nudge the table's left
and right sides):

| Key | Does |
|---|---|
| Esc | Stops the demo; press again for the menu (start a game, options, quit). In a game: pause / menu |
| Return | Choose in the menu; hold and release to pull the plunger |
| Z | Left flipper |
| / | Right flipper |
| Space | Nudge |
| Cmd-F | Full screen on or off |
| Cmd-S | Mark the moment for a clip (Loony Labyrinth; see Replays) |
| Cmd-Q or closing the window | Quit |

The keys can be changed from the game's OPTIONS menu. Unregistered, games are
time-limited.

The **Sound** menu in the menu bar has a volume slider, from silent at the left
to the game's own level at the right. It starts at 80%, 6 dB below the game's
level. The slider follows how loudness is heard, not the signal, so 50% is
18 dB down. The volume is remembered, and is the same for every game.

Each game's preferences (options, keys, the high-score table and the license)
are saved when it quits (and at any exit but a crash), in
`~/Library/Application Support/littlewing-shim/<game>/prefs.plist`, where `<game>`
is `loony-labyrinth`, `crystal-caliburn` or `monster-fair`. Any file a game
writes goes to its own folder there, never into the game's folder or app.
Delete a game's folder to start it over. The picker's last game and the volume
are kept in `littlewing-shim/picker.plist`. Saves from before the project was
renamed (`loony-shim/`) move to `littlewing-shim/` on the next launch, and
saves from before there were two games (`prefs.plist` at the top) move into
`loony-labyrinth/`.

## HD art

HD mode draws the game at 4 times its size (3200×2400), with replacement art
for the game's pictures. The game itself still runs at 800×600. The app
carries the art in `hd-art/` (`tools/make_app.sh` copies it in) and plays a
game in HD whenever it has art for it: so far, Loony Labyrinth. There is no
setting.

A build run from a terminal isn't an app, so it plays in HD only when told
where the art is. `LOONY_HD=off` turns HD off, in the app too:

```bash
LOONY_HD=hd-art/loony-labyrinth ./build-release/loony "/Applications/Loony Labyrinth"
```

To make art again, or for another game, dump the pictures it draws and enlarge
them:

```bash
LOONY_HD_DUMP=/tmp/dump ./build-release/loony   # play a while: writes each picture drawn
tools/hd_art.sh /tmp/dump /tmp/art              # de-dither and enlarge them (ImageMagick)
```

Each picture is a file `<hash>.png` at any size, so art from a better
upscaler, or redrawn by hand, can replace the generated file. `LOONY_HD_SCALE`
(2 to 8) sets the factor.

- **Without art:** a picture with no art file, and anything the game draws
  with its own code, shows its original pixels enlarged. That covers the ball
  and parts of the flippers.
- **Lamps and other sprites:** when the game copies a small picture
  pixel-for-pixel, that picture's art is drawn in its place.
- **Picture sheets:** `hd_art.sh` enlarges each picture whole, so on a sheet of
  tiles, such as the score display's font (`0e4f700e`), neighbouring tiles
  blur together. That file is left out of `hd-art/loony-labyrinth`, so the
  score display keeps the original font.
- **Other games:** art for another game goes in `hd-art/<game id>`
  (`crystal-caliburn`, `monster-fair`), and the app uses it from then on.

## Giving it to someone

A Developer ID signature and notarization let the app open on any Mac without
a warning. You need a paid Apple Developer account. Once per Mac:

1. Make a Developer ID Application certificate: in Xcode, open Settings, then
   Accounts, select your team, click Manage Certificates, and add a
   "Developer ID Application" certificate. (Only the account holder can.)
   `security find-identity -v -p codesigning` then lists it.
2. Make an app-specific password at [account.apple.com](https://account.apple.com)
   (Sign-In and Security), and save it for `notarytool`:

   ```bash
   xcrun notarytool store-credentials loony-notary --apple-id you@example.com --team-id TEAMID
   ```

Then, each time:

```bash
LOONY_SIGN_ID="Developer ID Application: Your Name (TEAMID)" cmake --build build-release --target app
tools/notarize.sh build-release/LittleWing.app loony-notary   # takes a few minutes
```

This sends the app to Apple for checking, staples the ticket to it, and
leaves `build-release/LittleWing.zip` to send. The other person
downloads the games as in [Play](#play), unzips the app into Applications,
and double-clicks it. They need their own key codes to register.

### Releases

To publish a version on the
[Releases](https://github.com/gregjkal/littlewing-shim/releases) page, from a
clean `main` that matches GitHub:

```bash
LOONY_SIGN_ID="Developer ID Application: Your Name (TEAMID)" tools/release.sh 0.1.0
```

This builds Unicorn and SDL3 from their sources (`tools/build_deps.sh`) and a
fresh Release build against them in `build-dist`, all for macOS 14, and runs
the tests. It then builds the app as version 0.1.0, checks that it runs on
macOS 14, and notarizes it. It tags the commit `v0.1.0`,
pushes the tag, and makes a draft release with `LittleWing-0.1.0.zip` and
`appcast.xml`, the feed that released apps read to find updates. Check the
draft on GitHub, then publish it: apps see the update once it's the latest
release. Other builds of the app say version 0.0.0 (set `LOONY_VERSION` to
change it), and don't update themselves.

The updates are signed with a [Sparkle](https://sparkle-project.org) key kept
in the keychain of the Mac that releases, and apps check the signature with
its public half, in `tools/sparkle_public_key.txt`. Make it once:

```bash
tools/sparkle.sh /tmp/sparkle
/tmp/sparkle/bin/generate_keys --account littlewing-shim
/tmp/sparkle/bin/generate_keys --account littlewing-shim -p >tools/sparkle_public_key.txt
```

Merge `tools/sparkle_public_key.txt` to `main`.
Back up the private key (`generate_keys --account littlewing-shim -x <file>`)
somewhere safe: without it, released apps can't be updated, since they accept
only updates signed with it. On another Mac, import it with
`generate_keys --account littlewing-shim -f <file>`.

## Debugging

```bash
LOONY_TRACE=imports ./build/loony     # log every OS call
LOONY_TRACE=imports,calls ./build/loony   # also log each call into the game (callbacks)
LOONY_TRACE=lowmem ./build/loony      # log the first write to each low-memory address
LOONY_STUB=all ./build/loony          # unimplemented OS calls return 0 instead of crashing
LOONY_EXIT_AFTER=600 LOONY_SCREENSHOT=shot.png ./build/loony   # run 10 s, save the last frame
SDL_VIDEO_DRIVER=dummy ./build/loony  # no window (with LOONY_SCREENSHOT for headless runs)
LOONY_WAV=out.wav ./build/loony       # also record the sound (44.1 kHz 16-bit stereo)
SDL_AUDIO_DRIVER=dummy ./build/loony  # no sound output
LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=play.txt SDL_VIDEO_DRIVER=dummy ./build/loony
LOONY_AUTO_ALERTS=1 ./build/loony     # answer alerts with their default button, without showing them
LOONY_DATA_DIR=/tmp/fresh ./build/loony   # use another folder for preferences and saved files
```

To read a game's own code, `tools/disasm.py` disassembles its PowerPC
functions by name. The Classic games keep a traceback table after each
function with its C++ name and length, so it can list them and label calls.
It needs Capstone, a development dependency only (the app doesn't use it):

```bash
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements-dev.txt
.venv/bin/python tools/disasm.py "/Applications/Loony Labyrinth/LOONY LABYRINTH 3.0.1" --list Event_
.venv/bin/python tools/disasm.py "/Applications/Loony Labyrinth/LOONY LABYRINTH 3.0.1" Event_StartGame
```

Offsets are code offsets, as the shim prints them (`code+0x154f0`); an
offset in place of a name shows the function holding it. Mach-O games
(MONSTER FAIR) aren't read yet.

At exit (and before going back to the picker) the log has a line on what
showing frames cost: `display: <n> presents, <bytes> bytes uploaded per present
on average; upload <ms> average, <ms> max; upload and present <ms> average,
<ms> max`. The upload copies the frame into the window's texture; the present
also draws it and waits for the display's refresh (not with
`LOONY_FIXED_CLOCK=1`). With `SDL_VIDEO_DRIVER=dummy` the bytes are right but
the times mean little: run in a real window for those. In HD mode only the
parts of the frame that changed are uploaded. `LOONY_HD_VERIFY=1` checks that
at every present, by applying the same uploads to a copy of the frame before
and comparing it with the whole new frame, and logs at exit how many presents
matched (slow: it compares 30 MB a frame).

`LOONY_FIXED_CLOCK=1` makes time advance only when the game waits, so a run is
the same every time and doesn't depend on the host's speed, as long as it
starts from the same preferences: point `LOONY_DATA_DIR` at an empty folder.
It plays no sound, but `LOONY_WAV` still records what would have played, the
same samples every run.

`LOONY_SCRIPT` plays keys and takes screenshots at given ticks (1/60 s since launch), one
action per line:

```
1720 down esc
1724 up esc
1880 screenshot menu.png
2600 quit
```

Scripts can also click (`20 click 460 270`, in emulated-screen pixels) and type
into a dialog (`50 type me@example.com`, the rest of the line), take focus away
and give it back (`60 blur`, `90 focus`), mark a clip while recording
(`70 mark`, as Cmd-S does), and may be any length. `tools/soak_script.py` writes one that keeps playing for an hour (game
starts, plunger, flippers, nudges, a screenshot every 5 minutes):

```bash
python3 tools/soak_script.py 216000 /tmp/soak > /tmp/soak.txt
LOONY_DATA_DIR=$(mktemp -d) LOONY_AUTO_ALERTS=1 LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=/tmp/soak.txt \
  SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ./build-release/loony
```

For MONSTER FAIR, add `monster-fair` after the screenshot prefix (its first
game starts later, and it nudges with X and . too), and pass
`"/Applications/MONSTER FAIR.app"` to `loony`.

Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
`lshift`, `rshift`, ...).

The original game files are only ever read, never modified. The emulated screen
is the size each game expects: 800x600 for the classic games, and 1024x768 at
16 bits for MONSTER FAIR, which takes over the whole (emulated) display. The
host window keeps its size and scales it to fit.

Tests that need a game's files skip when it isn't installed (`SKIP_UNLESS_GAME`,
`SKIP_UNLESS_CC` and `SKIP_UNLESS_MF` in `tests/test.h`). `LOONY_GAME_DIR`,
`LOONY_CC_DIR` and `LOONY_MF_APP` point them at copies elsewhere. The tests
that register take the address and key code from the environment and skip
without them: `LOONY_TEST_EMAIL` and `LOONY_TEST_KEY` (Loony Labyrinth), and
`LOONY_TEST_MF_EMAIL` and `LOONY_TEST_MF_KEY` (MONSTER FAIR; the public key in
`docs/test_key.txt` is one the game would refuse in play without the workaround
below).
The slow tests, the three-minute regression runs (`run_three_minutes_*`) and
MONSTER FAIR's play past its second license check, take a minute or more each,
so they skip unless `LOONY_SLOW_TESTS=1` is set (`SKIP_UNLESS_SLOW`). Run them
before merging anything that could change emulation, drawing or sound.

The tests run in parallel, one child process per test and one test per CPU at
a time (`-j N` to change that), each with a save folder of its own; a test's
output prints as one block when it finishes, and the summary names the
failures.

Only MONSTER FAIR 1.2.5 is supported; the shim refuses to start any other
version. MONSTER FAIR checks its license a second time during play, a couple
of minutes in, and erases a license it refuses there, such as a key code
posted publicly, even though the Register window accepted it. The shim works
around that permanently: on every launch it changes two instructions in
1.2.5's code so the check still runs but its verdict is dropped
(`src/patch.c`). The Register window's check is unchanged.

Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`

### Replays (prototype, Loony Labyrinth)

Every game of Loony Labyrinth is recorded into a file of its own: what
reached the game from outside while you played (the clock and date, keys and
focus, when sound callbacks came, and the preferences it started from), a
few KB a minute, in `~/Library/Application Support/littlewing-shim/recordings`
(`recordings` in `LOONY_DATA_DIR` when that is set). A file is named for the
game's start, `loony-labyrinth-<date>-<time>.lwreplay`, and ends at game
over (after a high score's initials), when the game is thrown away from the
pause menu, when another starts, or at quit. No file is written outside a
game, and Cmd-S in the menus or the self-playing demo says "no game is being
recorded".

A replay can only start at launch, so a game's file also carries what came
before the game in that launch: the opening, the menus and any earlier
games, without their marks. Watching replays that part first (below).

At launch, the last three games are kept, and so is every game where you
pressed Cmd-S (renamed `...-marked.lwreplay` at the first press), every
game with clips (below) and every game that set a high score still in the
table: a game whose score is pushed off the table goes with it. A game is
matched to a score by time: the score's timestamp falls between the start
in the file's name and its last write. The others are deleted then, so the
games of a session stay until the next launch.
`LOONY_RECORD=game.lwreplay` records the whole session into a file of your
choosing instead, and `LOONY_RECORD=0` records nothing. Only format 5, the
one with a file per game, plays: recordings from the prototype's earlier
builds are refused. The date a replay sees is the recording's:
a high score keeps its timestamp. `LOONY_REPLAY` plays a recording back
headless, frame for frame and faster than real time, from those preferences
in a folder of its own:

```bash
LOONY_GAME=loony-labyrinth LOONY_RECORD=game.lwreplay ./build-release/loony
LOONY_REPLAY=game.lwreplay LOONY_SCREENSHOT=last.png SDL_VIDEO_DRIVER=dummy ./build-release/loony
```

The replay logs where its saves went (set `LOONY_DATA_DIR` to choose; it
refuses a folder that already has preferences, so it can't write over yours) and,
at the end, whether it reached the end of the recording with every frame
check matching. Every 30 presents the recording keeps a hash of the screen;
at the first one that differs, the replay stops with status 3 and logs the
step and tick (a desync). A replay plays no sound unless it is watched
(below; `LOONY_WAV` still records it) and needs the same game program; a
replay made by another version of the shim may desync. HD mode can be on or
off in either. `LOONY_SCRIPT` screenshots work in a replay; the game never
sees its keys (a watched replay takes them), and its quit (like closing the
window) stops the replay.

A replay can also go straight to a short video, with its sound:

```bash
LOONY_REPLAY=game.lwreplay LOONY_CLIP=9:40-10:15 LOONY_CLIP_OUT=ramps.mp4 ./build-release/loony
```

Up to the clip's start the replay runs at full speed and draws nothing; from
there each tick of the game's clock is a frame (60 fps), handed with the
sound straight to AVFoundation's H.264 and AAC encoders, so no frame is
written to disk and no ffmpeg is needed. It exits once the clip is written;
no window opens. Times are play time (below), `m:ss` or `h:mm:ss` from the
game's start (`1:02:03.5`), or ticks. In HD mode the clip is the HD picture at 2x, scaled down to 1280x960;
`LOONY_CLIP_WIDTH` sets another width (1600 for the whole 2x picture; a
clip is never scaled up). Without HD it is 800x600. `LOONY_CLIP_OUT` is
`clip.mp4` by default (`.mov` makes a QuickTime movie). The video's quality
is `LOONY_CLIP_QUALITY`, above 0 and at most 1 (0.65 by default, which matched x264 at CRF
33 on a test clip); `LOONY_CLIP_KBPS` sets an average bitrate instead. A
30-second clip from minute 10 of a 15-minute game takes about 30 seconds and
comes to 3.1 MB in HD, or 16 seconds and 2.1 MB at 800x600. A clip
that runs past the end of the recording keeps what it has; one that starts
past it writes nothing and exits with status 4. A clip cut short before
the recording's end (by a desync, an error, `LOONY_EXIT_AFTER`) is thrown
away, with the desync's or the error's status, or else 1. A file already at
`LOONY_CLIP_OUT` is replaced, but the replay itself is refused, as is a
folder that can't be written, before the seek.

While you play, Cmd-S marks the moment, the way a console saves the last
30 seconds. The window's title says "clip 1 marked" for a couple of seconds,
and the game never sees the key. A mark is one record in the file, and a
replay only logs it. Afterwards, `LOONY_CLIP=marks` makes a clip of every
mark in a single pass:

```bash
LOONY_REPLAY=game.lwreplay LOONY_CLIP=marks ./build-release/loony
```

Each clip runs from 30 seconds before its mark to 3 seconds after it, since
the key comes a moment late; `LOONY_CLIP_BEFORE` and `LOONY_CLIP_AFTER` set
those, in seconds. The clips go next to the replay, each named for its
span: `game-2m58-3m20.mp4` and so on, for `game.lwreplay` or
`game-marked.lwreplay` (with `LOONY_CLIP_OUT=ramps.mp4`,
`ramps-2m58-3m20.mp4`), and the other `LOONY_CLIP_` settings apply to each.
A clip whose file is there already is skipped, so exporting again renders
only new clips, and when there are none it exits at once. The skip goes by
the name alone: to render a clip again, with other settings say, delete
its file, and give each replay a `LOONY_CLIP_OUT` of its own. Each clip is
written to a hidden file and renamed once it is finished, so an export
that is stopped never leaves a clip that looks whole. Ctrl-C, SIGTERM or
SIGHUP removes that hidden file too, and the export exits with 128 plus
the signal (130 for Ctrl-C, 143 for SIGTERM). Marks whose
clips overlap, or come within a second of each other, share one clip. A
replay with no marks is refused. When nothing is being recorded
(outside a game when each game gets a file, with `LOONY_RECORD=0`, with
`LOONY_FIXED_CLOCK=1`, or in a Loony Labyrinth other than 3.0.1, whose games
can't be told apart), Cmd-S marks nothing and the title says so.

For a program running an export, `LOONY_CLIP_PROGRESS=1` reports it on
stdout, a whole line for each event (the log on stderr is unchanged, and
without the setting nothing goes to stdout):

| Line | When |
| --- | --- |
| `skipped <file>` | a clip whose file is there already |
| `seek <tick> <target>` | replaying up to the next clip's start, on the game's clock in ticks, at most 4 a second |
| `clip <k> <n> <frame> <frames>` | making clip k of the n left to render, frame of its frames written: at 0 and each time the percentage changes |
| `wrote <file>` | a clip finished and in place |
| `error <message>` | the export couldn't start (no `.clips` file, no marks, a bad setting) |

`<file>` is the clip's file as the replay or `LOONY_CLIP_OUT` names it.

To find a moment you didn't mark, watch the replay in a window, at the speed
it was played and with its sound:

```bash
LOONY_REPLAY=game.lwreplay LOONY_WATCH=1 ./build-release/loony
LOONY_REPLAY=game.lwreplay LOONY_CLIP=clips ./build-release/loony
```

| Key | While watching |
| --- | --- |
| Space, K | Pause, or go on |
| Right, Left | Faster or slower: 1x, 2x, 4x, 8x, as fast as it can |
| I, O | Where a clip starts, and where it ends (O saves it) |
| J, L | Back or ahead 10 seconds (with Shift, a minute) |
| E | Export the clips, as `LOONY_CLIP=clips` does |
| Esc | Cancel the export |

Watching starts where the game does. First it replays what came before the
game, silent and as fast as it can, about 70 times real time, drawing a few
frames a second; the title counts through it ("replaying what came before
the game, 0:40 of 2:10"). Starting from a saved state instead would skip that wait
([#46](https://github.com/gregjkal/littlewing-shim/issues/46)). The same goes
for what isn't play inside the game, the pause menu and time the window
was in the background: it is skipped, and every time is play time, which
counts only play from the game's start. That holds for the title, J and L,
I and O, the `.clips` file, `LOONY_CLIP` and marks. A clip that runs
across a pause goes straight from before it to after it.

The window's title shows the play time and the speed, and a clip's start
once I has marked it. Only 1x has sound. Cut scenes draw up to twice as many
frames as play, more than the window can show; when watching falls behind,
it skips frames to keep time, and shows the latest once it has caught up,
or a few times a second when it can't catch up (HD at 8x). Each clip goes
into `game.clips` next to `game.lwreplay`, one `<start>-<end>` a line in `LOONY_CLIP`'s
format, so the file can be edited by hand too; `LOONY_CLIP=clips` then
renders each into `game-0m01-0m03.mp4` and so on, in one pass, as
`LOONY_CLIP=marks` does (clips that overlap become one). Taking a line out
of the file, or adding one, renames none of the other clips' files, though
clips that come to overlap, or stop overlapping, get new files. A recording in
`recordings` with a `.clips` file is kept, like a marked one.

Going ahead replays without pacing to the time. A replay only runs
forward, so going back restarts the program, which replays from the start
to the time wanted: the further into the game, the longer it takes (longer
still in HD). The speed, a pause
and a clip's start carry over. At the end of the replay, or when the game
quits, the last frame stays up until you close the window. At 1x each
minute of play logs how long it took on the wall clock: within a few
milliseconds. The keys of a `LOONY_SCRIPT` go to the viewer too (`60 down
i`), until a restart.

#### Exporting from the viewer

E exports the clips in `game.clips` without leaving the viewer: the
replay plays on while a second copy of LittleWing renders them, as
`LOONY_CLIP=clips` does, and the window's title says how far it has got
("exporting clip 2 of 3, 40%", or "exporting, finding clip 2 of 3, 15%"
while it replays up to that clip). When it is done, Finder opens with the
new files selected, next to the recording, where the clips always go.
With every clip exported already, the title says so and Finder shows
those. Esc cancels, which leaves no file behind. One export runs at a
time; going back (J) doesn't stop it, and quitting does. While watching,
the **Replay** menu (next to Sound) has the same: **Export Clips**,
**Export Marks** (as `LOONY_CLIP=marks`, from the marks made with Cmd-S
while recording) and **Cancel Export**.

#### The Replays menu

The **Replays** menu lists the recordings kept in the recordings folder,
newest first, read again each time it opens, so a game that just ended is
there: when the game started, how long it was played (the menus and pauses
left out, as the viewer counts), and how many marks it has and whether it
has clips, as in "Oct 15, 8:15 PM, 14:32, 2 marks, clips". Choosing one
watches it from 0:00, as opening the file does (below). Under a recording
with marks, an indented **Play from 3:12** for each mark watches it from that
mark, and J goes back from there. **Show Recordings in Finder**, at the
bottom, opens the folder. The menu is there in the picker, while Loony
Labyrinth plays and while watching; not in the other games, which aren't
recorded yet. A file in the folder that isn't a replay this version plays
is left out.

#### Sharing a high score

While Loony Labyrinth plays, the **High Scores** menu (next to Sound)
lists the game's four high scores, read from the game each time the menu
opens, so a score set a minute ago is there. Each score has a submenu with
the date it was set and two items. **Share Replay…** opens the share sheet
(AirDrop, Mail, Messages and so on) with the replay of the game that set
the score. **Save Replay…** saves the same file wherever you choose. It is
named like `Loony Labyrinth 449,655,220 GREG.lwreplay`; an hour of play comes
to about 400 KB. The replay is the recording kept for that score (see
above), found by the score's date. A score set before the app recorded
games has none, and neither does a score without a date, and its submenu
says so. The game starts with undated scores, and it loses dates too: when
a new score pushes the others down, their names move with them but their
dates stay in place, so a lower score can show another's date, or none. A
score recorded by the prototype's earlier builds can't be shared, since
this version can't play its recording. The replay holds the one game that
set the score, and watching it starts at that game (above).

The shared file leaves your registration out. The app writes a copy with
the e-mail address and key code emptied, and the original recording stays
as it is. The high-score names and anything typed during play are in the
copy. A game played before Loony Labyrinth was registered can't be shared:
the replay would start differently.

#### Opening a replay

Double-click a `.lwreplay` file, or drop one on the window, and
LittleWing.app watches it from 0:00, with the keys above. Whatever was
playing stops first, and a game's preferences and recording are saved as
on quit. Opening another file switches to it, and stops an export. When the replay ends, the
last frame stays up until you close the window, which quits the app.
**File > Open Recent** lists the recordings watched lately, however they
were opened, and **Clear Menu** forgets them.

To watch a shared replay you need Loony Labyrinth installed and registered
in LittleWing: the replay uses your registration in place of the one left
out. The game only checks the registration and never draws it, so the
frames still match the recording. Without one, the app says so and doesn't
play it. The replay also needs the same version of LittleWing, or it may
desync. From a terminal, `LOONY_REPLAY_LICENSE` names the preferences to
take the registration from; by default they're your own copy's, in
`littlewing-shim/loony-labyrinth/prefs.plist`. Shared files are format 5,
like recordings, with a flag that says the registration is out.

While recording, the game's clock moves in whole milliseconds. When the
game spins on `TickCount` waiting for the next tick, it sleeps instead, and
when it spins on `Microseconds` and `Delay(0)` (Loony Labyrinth's menus do,
the pause menu among them), it sleeps until the next millisecond after three
polls in the same one. The
file holds none of the game itself, but its preferences hold the
registration (e-mail address and key code) and the high-score names, and the
keys record anything typed. Share a recording through the High Scores menu,
which takes the registration out, or only as you would those.

## License

littlewing-shim is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 2 of the License, or (at your option) any
later version. See `LICENSE`.

The app bundles Unicorn (GPLv2) and SDL3 (zlib license). Their licenses are in
`LittleWing.app/Contents/Resources/Licenses`. The games, and the art in
`hd-art/` made from them, belong to LittleWing and aren't covered by this
license.
