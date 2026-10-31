# Development

How to build LittleWing, run it from a terminal, debug it, test it and
release it. For playing the games, see the [README](../README.md). The
design, with the memory map and the reasons behind the layout, is
[`docs/superpowers/specs/2026-09-30-loony-shim-design.md`](superpowers/specs/2026-09-30-loony-shim-design.md).

## Building

You need Xcode's command line tools, CMake, and Unicorn and SDL3 from
Homebrew:

```bash
brew install cmake unicorn sdl3 pkg-config
cmake -S . -B build && cmake --build build -j4      # Debug: ASan + UBSan, warnings are errors
./build/loony_tests                                 # the tests
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release --target app
```

The last line makes `build-release/LittleWing.app`. It carries its own
copies of Unicorn and SDL3, so Homebrew upgrades don't affect it. It is
signed ad hoc, which is enough on the Mac that built it. On another Mac,
Gatekeeper blocks the first launch; open System Settings, then Privacy &
Security, and click Open Anyway near the bottom. (On macOS 15 and later,
right-click and Open no longer gets past it.) To give the app to someone
without that step, sign and notarize it ([below](#giving-it-to-someone)).
`tools/make_app.sh` refuses a sanitizer build, so the app always comes from
`build-release`.

## Running from a terminal

```bash
./build-release/loony                       # like the app: the picker, or the one game installed
./build-release/loony "/path/to/game folder"   # the game in that folder, no picker
./build-release/loony "/Applications/MONSTER FAIR.app"   # MONSTER FAIR, no picker
LOONY_GAME=crystal-caliburn ./build-release/loony   # that game, from its usual place
```

`LOONY_GAME` takes `loony-labyrinth`, `crystal-caliburn`, `monster-fair`,
`jinni-zeala`, `mad-daedalus`, `angel-egg`, `golden-logres`, `fairy-tower`,
`tristan` or `psychedelic-o`.

Outside the app, the picker shows each game's title picture or icon in
place of the thumbnails from LittleWing's download page.
`LOONY_PICKER_MUSIC=0` turns off the picker's music. The log is
`~/Library/Logs/littlewing-shim/loony.log` (the run before is kept as
`loony.previous.log`).

## Registration

Every game starts registered. LittleWing's store is gone, so no key can be
bought, and every build drops each game's verdict on the key (`src/unlock.c`,
with the patches in `src/games.inc`): any e-mail address and a made-up key
code of 24 or more digits register (24 or more letters and digits for MONSTER
FAIR; Psychedelic "O"'s key is its disc's 12-digit serial, and almost any
such number passes, such as 1234-5678-9012). So before a game first looks,
the app saves `tester@example.com` and
`123456789012345678901234` where the game keeps its license (`src/license.h`),
and the game goes straight to its opening, as if you had typed them into its
Register window. A license already saved, such as one you typed yourself, is
kept. The log says `license: registered in <file>` when it saves one, and
`UNLOCK ACTIVE for <game id>` at launch; if it says `nothing patched`, the
game's code isn't the version the patches expect.

`LOONY_UNREGISTERED=1` saves no license, so in a save folder that has none yet
(a fresh `LOONY_DATA_DIR`) the game starts unregistered; one already saved
stays. Unregistered, each game first shows two windows: the shareware
screen (Play Demo, Buy Now, Enter Key-Code, Quit) and the key list.
Click a button, or press Return for the outlined one. To register, click
**Enter Key-Code**, type or paste (Cmd-V) your e-mail address and key code,
using Tab to move between the fields, and click **Register** (or press
Return). The license is saved with the preferences, so later launches go
straight to the game. Unregistered, games are time-limited.
Psychedelic "O" has no demo: unregistered, it asks for its disc's serial
number in a window of its own, until one is accepted (Cancel quits). The
window's prompt is in Japanese, which the shim draws as unreadable
characters; type the serial in the box and click OK.

## Saved files

Each game's preferences (options, keys, the high-score table and the license)
are saved when it quits (and at any exit but a crash), in
`~/Library/Application Support/littlewing-shim/<game>/prefs.plist`, where `<game>`
is `loony-labyrinth`, `crystal-caliburn`, `monster-fair`, `jinni-zeala`,
`mad-daedalus`, `angel-egg`, `golden-logres`, `fairy-tower`, `tristan` or
`psychedelic-o`. Any
file a game writes goes to its own folder there, never into the game's folder
or app. Tristan saves its settings and high scores into its own program
file, so those saves go to a copy of that file there, which it plays from
after that.
Delete a game's folder to start it over. Its picker music is kept there as
`picker-music.wav`; delete that to hear it again from the game. The picker's last game and the volume
are kept in `littlewing-shim/picker.plist`. Saves from before the project was
renamed (`loony-shim/`) move to `littlewing-shim/` on the next launch, and
saves from before there were two games (`prefs.plist` at the top) move into
`loony-labyrinth/`.

## Screen sizes

The original game files are only ever read, never modified. The emulated screen
is the size each game expects: 800x600 for the classic games, 1024x768 at
16 bits for MONSTER FAIR, which takes over the whole (emulated) display, and
1024x768 at 32 bits for Mad Daedalus and FAIRY TOWER, which play at the display's own size
(their DESKTOP SCREEN option) and whose tables are drawn for 768 lines, as is
Angel Egg's, which sizes its window to the 1024x768 screen, and Golden
Logres's, which takes the display over at 1024x768 and 16 bits. Tristan,
the oldest, gets a 640x480 screen. The host
window keeps its size and scales it to fit.

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
  `golden-logres`, `fairy-tower`, `tristan`, `psychedelic-o`), and the app uses it
  from then on.

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
LOONY_DATA_DIR=$(mktemp -d) LOONY_UNREGISTERED=1 ./build/loony   # no license: the shareware screens show
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
LOONY_DATA_DIR=$(mktemp -d) LOONY_UNREGISTERED=1 LOONY_AUTO_ALERTS=1 LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=/tmp/soak.txt \
  SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ./build-release/loony
```

For MONSTER FAIR, add `monster-fair` after the screenshot prefix (its first
game starts later, and it nudges with X and . too), and pass
`"/Applications/MONSTER FAIR.app"` to `loony`.

Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
`lshift`, `rshift`, ...).

## Tests

Tests that need a game's files skip when it isn't installed (`SKIP_UNLESS_GAME`,
`SKIP_UNLESS_CC`, `SKIP_UNLESS_MF`, `SKIP_UNLESS_JZ`, `SKIP_UNLESS_MD`,
`SKIP_UNLESS_AE`, `SKIP_UNLESS_GL`, `SKIP_UNLESS_FT`, `SKIP_UNLESS_TR` and
`SKIP_UNLESS_PO` in `tests/test.h`). `LOONY_GAME_DIR`, `LOONY_CC_DIR`,
`LOONY_MF_APP`, `LOONY_JZ_DIR`, `LOONY_MD_APP`, `LOONY_AE_APP`,
`LOONY_GL_APP`, `LOONY_FT_APP`, `LOONY_TR_DIR` and `LOONY_PO_DIR` point
them at copies elsewhere. The tests run with `LOONY_UNREGISTERED=1`, so the
games start unregistered, as in the frames they check; the tests of the
license saved by default unset it, as do Psychedelic "O"'s, since it has no
demo to play. The
tests that register type a made-up address and a 24-digit key code
(`TEST_EMAIL` and `TEST_KEY` in `tests/test_run.c`), which the unlock accepts,
so they need no secrets.
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

## MONSTER FAIR's second license check

Only MONSTER FAIR 1.2.5 is supported; the shim refuses to start any other
version. MONSTER FAIR checks its license a second time during play, a couple
of minutes in, and erases a license it refuses there, such as a key code
posted publicly, even though the Register window accepted it. The shim works
around that permanently: on every launch it changes two instructions in
1.2.5's code so the check still runs but its verdict is dropped
(`src/patch.c`). The Register window's check is unchanged.

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
downloads the games as in the [README](../README.md#quick-start), unzips the app into Applications,
and double-clicks it. Every game starts registered (see the [README](../README.md#quick-start)).

## Releases

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
