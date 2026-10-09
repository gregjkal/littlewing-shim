# loony-shim

Runs three PowerPC Mac pinball games by LittleWing natively on Apple Silicon:
*Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1* (2003, Carbon programs for
Mac OS 9 and X) and *MONSTER FAIR 1.2.5* (2010, a Mac OS X application). It
emulates their CPU (Unicorn) and reimplements the Mac OS calls they make in C.
Personal use only. This repo contains no game files; point it at your own
copies. The one exception is `hd-art/`, enlarged art made from the game's
pictures (see HD art below).

## Build

```bash
brew install unicorn sdl3 cmake pkg-config
cmake -S . -B build                                     # Debug (sanitizers): for development
cmake --build build
./build/loony_tests          # all tests; ./build/loony_tests <substring> to filter

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release  # for playing
cmake --build build-release
cmake --build build-release --target app                # build-release/LittleWing.app
```

## Play

The app needs an Apple Silicon Mac with macOS 26 or later. It plays the games
from these places, which must hold the game's file:

| Game | Where | File | Download |
|---|---|---|---|
| Loony Labyrinth | `/Applications/Loony Labyrinth` | `LOONY LABYRINTH 3.0.1` | `loony_labyrinth_301a.dmg` |
| Crystal Caliburn | `/Applications/Crystal Caliburn` | `CRYSTAL CALIBURN 3.0.1` | `crystal_caliburn_301a.dmg` |
| MONSTER FAIR | `/Applications/MONSTER FAIR.app` | `Contents/MacOS/MONSTER FAIR` | the Mac OS X version |

To get a game, download it from LittleWing's
[download page](http://www.littlewingpinball.com/doc/en/downloads/index.html),
open the disk image, and drag the game onto the Applications shortcut next to
it: the game's folder for Loony Labyrinth and Crystal Caliburn, the app itself
for MONSTER FAIR. Keep its name. MONSTER FAIR's own app doesn't run on Apple
Silicon; LittleWing.app runs it.

Copy `build-release/LittleWing.app` to `/Applications` (or anywhere) and
double-click it. With two or three games installed, it opens on a picker
showing each game's title picture (MONSTER FAIR's card shows its icon): Left
and Right (or the mouse) choose, and Return (or a click) plays. It starts on
the last game played. With one game installed, it plays that game. Choosing
QUIT in a game's own menu goes back to the picker; Cmd-Q or closing the window
quits. The app carries its own copies of Unicorn and SDL3, so Homebrew
upgrades don't affect it. Built as above, it is signed ad hoc, which is enough
on the Mac that built it. On another Mac, Gatekeeper blocks the first launch;
open System Settings, then Privacy & Security, and click Open Anyway near the
bottom. (Right-click and Open no longer gets past it on recent macOS.) To give
the app to someone without that step, sign and notarize it (below). When the app can't
start or the game crashes, it says so in a message box; its log is
`~/Library/Logs/loony-shim/loony.log` (the run before is kept as
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
| Cmd-Q or closing the window | Quit |

The keys can be changed from the game's OPTIONS menu. Unregistered, games are
time-limited.

Each game's preferences (options, keys, the high-score table and the license)
are saved when it quits (and at any exit but a crash), in
`~/Library/Application Support/loony-shim/<game>/prefs.plist`, where `<game>`
is `loony-labyrinth`, `crystal-caliburn` or `monster-fair`. Any file a game
writes goes to its own folder there, never into the game's folder or app. Delete a game's folder to start
it over. Saves from before there were two games (`loony-shim/prefs.plist`)
move into `loony-labyrinth/` on the next launch. The picker remembers the last
game in `loony-shim/picker.plist`.

## HD art (a prototype)

HD mode draws the game at 4 times its size (3200×2400), with replacement art
for the game's pictures. The game itself still runs at 800×600. Art for Loony
Labyrinth is in `hd-art/loony-labyrinth`:

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
- **Where it works:** only Loony Labyrinth has been tried, and only from a
  terminal; the app doesn't pass these settings on.

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
and give it back (`60 blur`, `90 focus`), and may be any length. `tools/soak_script.py` writes one that keeps playing for an hour (game
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
`docs/test_key.txt` is one the game refuses in play, which that test checks).

MONSTER FAIR checks its license a second time during play, a couple of
minutes in, and erases a license it refuses there, such as a key code posted
publicly, even though the Register window accepted it. For testing with such
a key, `LOONY_MF_SKIP_LICENSE_RECHECK=1` keeps the license: the check still
runs, but the shim drops its verdict (it changes two instructions in MONSTER
FAIR 1.2.5's code after loading, and refuses to start with any other
version). The Register window's check is unchanged.

```bash
LOONY_MF_SKIP_LICENSE_RECHECK=1 ./build-release/loony "/Applications/MONSTER FAIR.app"
open --env LOONY_MF_SKIP_LICENSE_RECHECK=1 build-release/LittleWing.app
```

Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
