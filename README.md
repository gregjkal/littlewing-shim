# littlewing-shim

Runs all nine of LittleWing's PowerPC Mac pinball games natively on Apple Silicon:
*Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1* (2003, Carbon programs for
Mac OS 9 and X), *Jinni Zeala 1.2.2* (2004, an older build of the same engine),
*MONSTER FAIR 1.2.5* and *Mad Daedalus 1.1.9* (2010, Mac OS X applications),
*Angel Egg 3.0.1* (the 1996 table, as a 2005 Mac OS X application),
*Golden Logres 3.0.4* (2005, a Mac OS X application), *FAIRY TOWER 1.1.7*
(2008, a Mac OS X application) and *Tristan 2.5.1* (the Internet Edition of
the 1992 table, a classic Mac OS program from before Carbon). It
emulates their CPU (Unicorn) and reimplements the Mac OS calls they make in C.

This repo contains no game files: you need your own copies of the games.
You can find them on Littlewing's
[download page](http://www.littlewingpinball.com/doc/en/downloads/index.html).

This is an unofficial project, not made or endorsed by LittleWing.

## Play

A release needs an Apple Silicon Mac with macOS 14 (Sonoma) or later. An app you build
yourself needs the macOS you built it on, or later, because Homebrew's Unicorn
and SDL3 do. It plays the games from these places, which must hold the game's
file:

| Game | Where | File | Download |
|---|---|---|---|
| Loony Labyrinth | `/Applications/Loony Labyrinth` | `LOONY LABYRINTH 3.0.1` | `loony_labyrinth_301a.dmg` |
| Crystal Caliburn | `/Applications/Crystal Caliburn` | `CRYSTAL CALIBURN 3.0.1` | `crystal_caliburn_301a.dmg` |
| Jinni Zeala | `/Applications/Jinni Zeala` | `JINNI ZEALA 1.2.2` | the Mac version, 1.2.2 |
| MONSTER FAIR | `/Applications/MONSTER FAIR.app` | `Contents/MacOS/MONSTER FAIR` | the Mac OS X version, 1.2.5 (the only version supported) |
| Mad Daedalus | `/Applications/Mad Daedalus.app` | `Contents/MacOS/Mad Daedalus` | the Mac OS X version, 1.1.9 |
| Angel Egg | `/Applications/Angel Egg.app` | `Contents/MacOS/Angel Egg` | the Mac OS X version, 3.0.1 |
| Golden Logres | `/Applications/Golden Logres.app` | `Contents/MacOS/Golden Logres` | the Mac OS X version, 3.0.4 |
| FAIRY TOWER | `/Applications/FAIRY TOWER.app` | `Contents/MacOS/FAIRY TOWER` | the Mac OS X version, 1.1.7 |
| Tristan | `/Applications/TRISTAN IE` | `TRISTAN IE 2.5.1` | the Mac version, 2.5.1 |

To get a game, download it from LittleWing's [download
page](http://www.littlewingpinball.com/doc/en/downloads/index.html), open the
disk image, and drag the game onto the Applications shortcut next to it: the
game's folder for Loony Labyrinth, Crystal Caliburn, Jinni Zeala and Tristan, the app
itself for MONSTER FAIR, Mad Daedalus, Angel Egg, Golden Logres and FAIRY TOWER. Keep its name. To keep
the games together, put them in a folder of their own in Applications instead,
such as `/Applications/LittleWing Games/Loony Labyrinth`; any folder name
works, one level down. A game directly in Applications is played over a copy
in a folder. None of these apps runs on Apple Silicon by itself;
LittleWing.app runs them.

Download `LittleWing-<version>.zip` from
[Releases](https://github.com/gregjkal/littlewing-shim/releases), unzip it,
and move `LittleWing.app` to `/Applications` (or anywhere); or build it as
above and copy `build-release/LittleWing.app`. Double-click it. With two to
all nine games installed, it opens on a picker showing each game's thumbnail
from LittleWing's download page (`./build-release/loony`, outside the app,
shows each game's title picture or icon instead): the arrow keys (or
the mouse) choose, and Return (or a click) plays. It starts on the last game
played. With one game installed, it plays that game. Choosing QUIT in a game's
own menu goes back to the picker; Cmd-Q or closing the window quits. The app
carries its own copies of Unicorn and SDL3, so Homebrew upgrades don't affect
it. A release is notarized and opens without a warning, and updates itself: it
checks for a new release once a day, and LittleWing → Check for Updates…
checks now. A new version found at launch, or after the Mac has sat idle, is
offered in a window; one found while you play only retitles that menu item
("Update to LittleWing 0.5.0…"), so play isn't interrupted. Installing
replaces the app where it is and reopens it. Built as above, it is signed ad
hoc, which is enough on the Mac that built it. On another Mac, Gatekeeper
blocks the first launch; open System Settings, then Privacy & Security, and
click Open Anyway near the bottom. (On macOS 15 and later, right-click and
Open no longer gets past it.) To give the app to someone without that step,
sign and notarize it (below). When the app can't start or the game crashes, it
says so in a message box; its log is
`~/Library/Logs/littlewing-shim/loony.log` (the run before is kept as
`loony.previous.log`).

From a terminal:

```bash
./build-release/loony                       # like the app: the picker, or the one game installed
./build-release/loony "/path/to/game folder"   # the game in that folder, no picker
./build-release/loony "/Applications/MONSTER FAIR.app"   # MONSTER FAIR, no picker
LOONY_GAME=crystal-caliburn ./build-release/loony   # that game, from its usual place
```

`LOONY_GAME` takes `loony-labyrinth`, `crystal-caliburn`, `monster-fair`,
`jinni-zeala`, `mad-daedalus`, `angel-egg`, `golden-logres`, `fairy-tower` or `tristan`.

Until it is registered, each game first shows two windows: the shareware screen
(Play Demo, Buy Now, Enter Key-Code, Quit) and the key list. Click a button, or
press Return for the outlined one. To register, click **Enter Key-Code**, type
or paste (Cmd-V) your e-mail address and key code, using Tab to move between
the fields, and click **Register** (or press Return). The license is saved with
the preferences, so later launches go straight to the game. LittleWing's store
is gone, so no key can be bought. Every build drops each game's verdict on the
key (`src/unlock.c`, with the patches in `src/games.inc`): any e-mail address and
a made-up key code of 24 or more digits register (24 or more letters and digits
for MONSTER FAIR). For example, `tester@example.com` and `123456789012345678901234`
work in every game. The log says `UNLOCK ACTIVE for <game id>` at launch; if it
says `nothing patched`, the game's code isn't the version the patches expect.

The game then plays its opening and a self-playing demo. All the games use
the same keys by default (MONSTER FAIR and Jinni Zeala add X and . to nudge
the table's left and right sides, and in Jinni Zeala, which has no demo to
stop, and in Mad Daedalus and FAIRY TOWER, the first Esc opens the menu). Angel Egg keeps
its menus in a menu bar it hides, so Cmd-N (its New Game) starts a game, and
Esc pauses it. So does Golden Logres, whose Esc first skips its opening story,
then the title; its Cmd-P pauses and Cmd-R resumes too. Tristan starts a game
with Cmd-N as well; its shareware screen offers Start Demo instead of Play
Demo:

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

Angel Egg, Golden Logres and Tristan have menus of their own (Golden Logres's
Operation menu, with New Game, Resume, the practice tables, Save Game and
Restore Game). The game hides them, so they appear in the Mac's menu bar
instead, after the app menu; in full screen, move the pointer to the top of
the screen to show it. Each menu shows what the game has enabled and
checked when it opens, and its keyboard shortcuts, and choosing an item does
what it does in the game. Display Options stays greyed out: it only sets the
screen size, which LittleWing chooses itself.

The **Sound** menu in the menu bar has a volume slider, from silent at the left
to the game's own level at the right. It starts at 80%, 6 dB below the game's
level. The slider follows how loudness is heard, not the signal, so 50% is
18 dB down. The volume is remembered, and is the same for every game.

Each game's preferences (options, keys, the high-score table and the license)
are saved when it quits (and at any exit but a crash), in
`~/Library/Application Support/littlewing-shim/<game>/prefs.plist`, where `<game>`
is `loony-labyrinth`, `crystal-caliburn`, `monster-fair`, `jinni-zeala`,
`mad-daedalus`, `angel-egg`, `golden-logres`, `fairy-tower` or `tristan`. Any
file a game writes goes to its own folder there, never into the game's folder
or app. Tristan saves its settings and high scores into its own program
file, so those saves go to a copy of that file there, which it plays from
after that.
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
  (`crystal-caliburn`, `monster-fair`, `jinni-zeala`, `mad-daedalus`, `angel-egg`,
  `golden-logres`, `fairy-tower`, `tristan`), and the app uses it
  from then on.

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
and double-clicks it. They register the same way, with any e-mail address and a made-up key code (see [Play](#play)).

### Releases

To publish a version on the
[Releases](https://github.com/gregjkal/littlewing-shim/releases) page, from a
clean `main` that matches GitHub:

```bash
LOONY_SIGN_ID="Developer ID Application: Your Name (TEAMID)" \
  LOONY_RELEASE_SUMMARY="One sentence about what's new." tools/release.sh 0.1.0
```

This builds Unicorn and SDL3 from their sources (`tools/build_deps.sh`) and a
fresh Release build against them in `build-dist`, all for macOS 14, and runs
the tests. Unicorn gets two fixes from its development branch
(`tools/unicorn-patches`) that its next release will have: without them, every
store the game makes takes Unicorn's slow path, even to memory it can't run
code from, and going ahead in a replay takes about 2.5 times as long. A build
against Homebrew's Unicorn plays the same, only slower there. It then builds
the app as version 0.1.0, checks that it runs on macOS 14, and notarizes it.
It tags the commit `v0.1.0`, pushes the tag, and makes a draft release with
`LittleWing-0.1.0.zip` and `appcast.xml`, the feed that released apps read to
find updates. `LOONY_RELEASE_SUMMARY` goes under "What's Changed", above the
list of pull requests, in both the release's notes and the feed's, which are
what the app's update dialog shows. Check the draft on GitHub, then publish
it: apps see the update once it's the latest release. Other builds of the app
say version 0.0.0 (set `LOONY_VERSION` to change it), and don't update
themselves.

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
(`70 mark`, as Cmd-S does), choose an item from one of the game's own menus
(`80 menu 131 6`: the menu's resource ID, then the item, counting from 1,
separators included), and may be any length. `tools/soak_script.py` writes one that keeps playing for an hour (game
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
is the size each game expects: 800x600 for the classic games, 1024x768 at
16 bits for MONSTER FAIR, which takes over the whole (emulated) display, and
1024x768 at 32 bits for Mad Daedalus and FAIRY TOWER, which play at the display's own size
(their DESKTOP SCREEN option) and whose tables are drawn for 768 lines, as is
Angel Egg's, which sizes its window to the 1024x768 screen, and Golden
Logres's, which takes the display over at 1024x768 and 16 bits. Tristan,
the oldest, gets a 640x480 screen. The host
window keeps its size and scales it to fit.

Tests that need a game's files skip when it isn't installed (`SKIP_UNLESS_GAME`,
`SKIP_UNLESS_CC`, `SKIP_UNLESS_MF`, `SKIP_UNLESS_JZ`, `SKIP_UNLESS_MD`,
`SKIP_UNLESS_AE`, `SKIP_UNLESS_GL`, `SKIP_UNLESS_FT` and `SKIP_UNLESS_TR` in
`tests/test.h`). `LOONY_GAME_DIR`, `LOONY_CC_DIR`, `LOONY_MF_APP`,
`LOONY_JZ_DIR`, `LOONY_MD_APP`, `LOONY_AE_APP`, `LOONY_GL_APP`,
`LOONY_FT_APP` and `LOONY_TR_DIR` point them at copies elsewhere. The tests
that register type a made-up address and a 24-digit key code (`TEST_EMAIL` and
`TEST_KEY` in `tests/test_run.c`), which the unlock accepts, so they need no
secrets.
The slow tests, the three-minute regression runs (`run_three_minutes_*`) and
MONSTER FAIR's play past its second license check, take a minute or more each,
so they skip unless `LOONY_SLOW_TESTS=1` is set (`SLOW_TEST`). `--slow` runs
them alone, and narrows them by any filters given: a test runs if one filter
matches it, once however many do. `--list` prints what a command would run.
Run them before merging anything that could change emulation, drawing or sound.

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
games, without their marks. A replay that goes past that part keeps a
snapshot of the game's start in the file, about 150 KB, and every replay
after that goes to the game's start from it a quarter of a second after
launch, however late in the session the game came. The app takes that
snapshot as soon as a game ends: a second copy of LittleWing replays the
file out of sight, at a lower priority than the game, one game after
another, while you play on. Quit the app before it is done, or open a
replay first, and it stops; then the first replay of that game (watching
it or exporting from it) replays the part before the game itself,
unseen, and keeps the snapshot. The snapshot holds what changed since a
point shortly after launch that every replay of the file reaches by
itself, less the game's own sounds and art, which come from your copy of
the game, and never your registration. A snapshot works on any Mac with
the same copy of the game, whoever's registration it has, so a shared
replay carries its snapshot (below), and it keeps working after an update
to LittleWing unless that update changed what a snapshot holds. One taken
by a version whose snapshots differ, or that doesn't fit (another copy of
the game, a damaged file), is ignored with a line in the log saying why:
the replay plays from launch and takes it again. If a replay that started
from a snapshot goes out of step with the recording, the snapshot is
dropped and the replay starts again from launch, by itself, which takes
it again (an export fails instead, and the next export plays from
launch). A snapshot is the
state of the whole emulator, so one that would change the game's code, or
run anything but the game's own code, is refused the same way. A game that
starts within 20 seconds of launch gets none, and neither does a recording
still being written. Taking it rewrites the file in place, keeping its
last write's time, which is how a game is matched to its high score
(below).
`LOONY_GAMESTART=0` neither uses a snapshot nor takes one.
`LOONY_GAMESTART=take`, with `LOONY_REPLAY`, replays only until the
snapshot is kept in the file (or found there already, or known to be
impossible) and exits: it is what sharing runs, headless, and it draws
nothing, which halves its time.

At launch, the last three games are kept, and so is every game where you
pressed Cmd-S (renamed `...-marked.lwreplay` at the first press), every
game with clips (below) and every game that set a high score still in the
table: a game whose score is pushed off the table goes with it. A game is
matched to a score by time: the score's timestamp falls between the start
in the file's name and its last write. The others are deleted then, so the
games of a session stay until the next launch.
`LOONY_RECORD=game.lwreplay` records the whole session into a file of your
choosing instead, and `LOONY_RECORD=0` records nothing. Formats 5 and 6
play (6 is 5 with room for the snapshot of the game's start, and is what
this version writes): recordings from the prototype's earlier builds are
refused. The date a replay sees is the recording's:
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
| J, L | Back or ahead 10 seconds (with Shift, a minute) |
| Left, Right | Back or ahead 5 seconds |
| `,` and `.` (comma, period) | While paused, back or ahead a frame (1/60 s); comma at the end too, which pauses there |
| 0 to 9 (on the keypad too) | Go to that tenth of the recording: 0 is its start, 5 its middle |
| `[`, `]` | Go to the mark before or after (Cmd-S while recording) |
| `<`, `>` (Shift-comma, Shift-period) | Slower or faster: 1x, 2x, 4x, 8x, as fast as it can |
| I, O | Where a clip starts, and where it ends (O saves it); inside a saved clip, move its start or end there instead |
| Option-`,` and Option-`.` | Move the selected clip's nearer edge back or ahead a frame (with Shift, a second) |
| Backspace (Delete) | Delete the selected clip |
| E | Export the clips, as `LOONY_CLIP=clips` does |
| Esc | Cancel the export |

The same controls are on a strip below the game: play or pause, back and
ahead 10 seconds, slower and faster with the speed between them, the time
and the recording's length, and a bar of how far it has got. Resting the
pointer on a button says its key. The bar is the recording's timeline:
marks are diamonds on it, saved clips yellow spans, and a clip started
with I a bracket. The pointer over the bar says the time under it (or a
mark's or clip's number), and a click goes there; a click on a mark or a
clip's start goes to it exactly. Drag along the bar to find a time: the
replay pauses while the button is down (unless it is at its end), only the
caret moves, and letting go goes there and plays on if it was playing and
neither Space nor a frame step was pressed in the meantime. The strip comes up when the pointer moves
or a key is pressed and fades a couple of seconds later, unless the pointer
is on it; a thin line along the bottom still shows the progress, and the
pointer hides with the strip. In full screen the strip goes over the
bottom edge of the game. What the viewer has to say ("clip 1 saved", "no
marks") shows above the strip for a couple of seconds, with the strip up,
rather than in the window's title, so it shows in full screen too. Pausing
or going on shows a pause or play sign over the middle of the game that
fades away, and the speed lights up for a moment when it changes. It is
all drawn on the window, never into the game's
screen, so screenshots, clips and a replay's frame checks leave it out.

These are YouTube's keys. A key with Cmd or Ctrl held goes to the menus,
not the viewer, and so does one with Option but for the edge's. The
**Replay** menu (next to Sound) lists every one of these with its key
but 0 to 9, so it is also where to find them, and Help's search finds
them by name.

Watching starts where the game does. First it replays what came before the
game, silent and as fast as it can, about 9 times real time in HD and 18
without, drawing a few frames a second; the title counts through it ("replaying what came before
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
or a few times a second when it can't catch up (HD at 8x); the strip then
shows the speed it reaches, in amber ("5.2x" for 8x), and at "as fast as
it can" it shows how fast that is. If a replay goes out of step with its
recording (a desync), a message box says so before the window closes. Each clip goes
into `game.clips` next to `game.lwreplay`, one `<start>-<end>` a line in `LOONY_CLIP`'s
format, so the file can be edited by hand too; `LOONY_CLIP=clips` then
renders each into `game-0m01-0m03.mp4` and so on, in one pass, as
`LOONY_CLIP=marks` does (clips that overlap become one).

To fix a clip after saving it, put the playhead in it: that clip is the
selected one, outlined on the timeline, with a taller bar on the edge
nearer the playhead (its start and end count as in it). Where clips
overlap, the shortest under the playhead is selected. I or O there moves
its start or end to the playhead rather than starting a new clip, but I
at a clip's very end starts the next one, and O after an I that started
a clip still saves it. Option-comma and Option-period move the edge with
the bar a frame, Shift-Option-comma and Shift-Option-period a second,
and the viewer pauses and goes with it so that the edge's frame is on
screen; a clip keeps at least a frame, and an edge stops where another,
shorter clip would be selected instead. (Shift-comma and Shift-period
are the speed, so the edge takes Option.) Backspace deletes the selected
clip at once; its exported video, if any, stays. With the playhead in no
clip, I starts a new one, O says how to make one, and Backspace and the
nudges do nothing. Each edit changes that clip's line in `.clips` and
leaves the rest of the file, comments and all, as it was; a file left
with nothing in it is removed. Marks aren't edited. Taking a line out of
the file, or adding one, renames none of the other clips' files, though
clips that come to overlap, or stop overlapping, get new files. A
recording in `recordings` with a `.clips` file is kept, like a marked
one.

Going ahead replays without pacing to the time. Going back is near
instant: while you watch, the viewer keeps a snapshot of the emulator every
5 seconds of the game's clock, puts back the last one before the time
wanted, and replays the few seconds from there without pacing. Going ahead
after going back uses them too. The window stays open, the speed, a pause
and a clip's start stay as they were, and an export goes on. It works from
the end of the replay too, after the game has quit. Snapshots take about a
megabyte each and stay in memory while the viewer runs; past 120 the
oldest are thinned out, so early in a long game they end up 10 or 20
seconds apart. At the end of the replay, or when the game quits, the last
frame stays up until you close the window or go back. At 1x each minute of
play logs how long it took on the wall clock: within a few milliseconds.
The keys of a `LOONY_SCRIPT` go to the viewer too (`60 down i`), at the
replay's ticks, so after going back the next one comes when the replay
gets there again; headless, the ones left come at once at the end or
while paused, and paused with none left, the viewer goes on.

`LOONY_SNAPSHOT_VERIFY=1` checks the snapshots as they are used, for
debugging a seek that desyncs. Each one is put back the moment it is taken
and compared with what was there, and after going back the replay is
compared with every snapshot it passed before, at the same trap, as it
comes to it again. A difference stops the program and names what differs:
a range of guest memory, a register, or a module's state and the byte in
it. A replay that isn't watched takes snapshots too while verifying, so
its frames and sound can be compared with one that takes none. The log's
last `seek: verify:` line counts the snapshots checked each way. It costs
little time but more memory, about 15 MB a snapshot instead of 1.
Verifying, a replay of a file with a snapshot of the game's start doesn't
use it: it decodes it shortly after launch, plays from launch, and compares
it with the emulator where it was taken, the game's sounds and art put back
from the game included (`gamestart: verify:` in the log).

#### Exporting from the viewer

E exports the clips in `game.clips` without leaving the viewer: the
replay plays on while a second copy of LittleWing renders them, as
`LOONY_CLIP=clips` does, and the window's title says how far it has got
("exporting clip 2 of 3, 40%", or "exporting, finding clip 2 of 3, 15%"
while it replays up to that clip). When it is done, Finder opens with the
new files selected, next to the recording, where the clips always go.
With every clip exported already, the viewer says so and Finder shows
those. Esc cancels, which leaves no file behind. One export runs at a
time; going back (J) doesn't stop it, and quitting does. The **Replay**
menu has the same: **Export Clips**, **Export Marks** (as
`LOONY_CLIP=marks`, from the marks made with Cmd-S while recording, with
no key) and **Cancel Export**.

#### The Replays menu

The **Replays** menu lists the recordings kept in the recordings folder,
newest first, read again each time it opens, so a game that just ended is
there: when the game started, how long it was played (the menus and pauses
left out, as the viewer counts), and how many marks it has and whether it
has clips, as in "Oct 15, 8:15 PM, 14:32, 2 marks, clips". Choosing one
watches it from 0:00, as opening the file does (below). Under a recording
with marks, an indented **Play from 3:12** for each mark watches it from that
mark, and J goes back from there. **Delete**, last under each recording,
moves it and its list of clips to the Trash, so Put Back in Finder undoes
it; the clips exported from it stay where they are. The game being recorded
has no Delete. Deleting the recording being watched closes the viewer, as
if the app had just opened, and says why in the window's title. **Show
Recordings in Finder**, at the bottom, opens the folder. The menu, like File > Open Recent (below), is
there in the picker, while Loony Labyrinth plays and while watching; not in
the other games, which aren't recorded yet. A file in the folder that isn't a replay this version plays
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
as it is. The copy carries the recording's snapshot of the game's start
(above), so it starts at the game on the other Mac too. When the
recording has none yet, the app takes one into the copy before the share
sheet or the save panel shows, by replaying it out of sight: a panel says
so, and Cancel stops the share. That takes a while for a game late in a
session (about half a minute for a game 20 minutes in). The snapshot is the
state of the game's memory when the game started, so whoever opens the
file runs the game from that state, not only from the inputs recorded.
The high-score names and anything typed during play are in the copy. A game played before Loony Labyrinth was registered can't be shared:
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
`littlewing-shim/loony-labyrinth/prefs.plist`. Shared files are format 6,
like recordings, with a flag that says the registration is out. One that
came without a snapshot, or with one that doesn't fit, keeps a snapshot of
the game's start in it the first time you watch it, as for your own.

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

The app bundles Unicorn (GPLv2), with the changes in `tools/unicorn-patches`,
and SDL3 (zlib license). Their licenses are in
`LittleWing.app/Contents/Resources/Licenses`. The games, and the art in
`hd-art/` made from them, belong to LittleWing and aren't covered by this
license.
