# Replays, clips and practice

These work in Loony Labyrinth only, for now. Back to the [README](../README.md).

## Recording

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

## Making clips

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

## Marking moments with Cmd-S

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

## Watching a replay

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

## Checking snapshots

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

## Exporting from the viewer

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

## The Replays menu

The **Replays** menu lists the recordings kept in the recordings folder,
newest first, read again each time it opens, so a game that just ended is
there: when the game started, how long it was played (the menus and pauses
left out, as the viewer counts), and how many marks it has and whether it
has clips, as in "Oct 15, 8:15 PM, 14:32, 2 marks, clips". Choosing one
watches it from 0:00, as opening the file does (below). Under a recording
with marks, an indented **Play from 3:12** for each mark watches it from that
mark, and J goes back from there, and under it **Practice from 3:12.50**
practices from the mark (below). **Delete**, last under each recording,
moves it and its list of clips to the Trash, so Put Back in Finder undoes
it; the clips exported from it stay where they are. The game being recorded
has no Delete. Deleting the recording being watched closes the viewer, as
if the app had just opened, and says why in the window's title. **Show
Recordings in Finder**, at the bottom, opens the folder. The menu, like File > Open Recent (below), is
there in the picker, while Loony Labyrinth plays and while watching; not in
the other games, which aren't recorded yet. A file in the folder that isn't a replay this version plays
is left out.

## Practice

Practice plays on from a moment of a recorded game, as many times as you
like, with nothing kept. Choose **Practice from 3:12.50** under a mark in the
Replays menu, or a moment the app ships in the **Practice** menu (next to
Replays): **2000 BC** starts as the score panel first shows LANDED AT 2000
BC. The replay runs out of sight to that moment, with the window's title
counting up ("Getting to 32:16.05 of play: 40%"), and then the game is
yours: the flippers, the sound, the rest of the ball. Nothing of it is kept:
it isn't recorded, and its high scores and settings go to a temporary
folder of its own, never your own preferences. The title says
"(practice from 32:16.05)". Choose the item again to start over.

The first practice from a moment of a long game takes a while (about 50
seconds for one half an hour in), so it keeps a snapshot of a second before
the moment (the replay checks that second against the recording) in a
`.practice` file beside the recording, and each practice after starts at
once. One file a recording: practicing from another of its moments
replaces it, and deleting the recording deletes it. The Practice menu's
moments come with their snapshots, and are copied to
`littlewing-shim/practice` when chosen. From a terminal, `LOONY_PRACTICE`
with `LOONY_REPLAY` practices from a time of play, in ticks or `[h:]m:ss`
with hundredths, as the viewer counts:

```bash
LOONY_REPLAY=game.lwreplay LOONY_PRACTICE=31:47.98 ./build-release/loony
```

Not with `LOONY_WATCH` or `LOONY_CLIP`. From the menus, a shared recording
practices with the registration every game starts with (from a terminal,
add `LOONY_REPLAY_LICENSE=default` for the same):
the game keeps its high scores only under the registration they were saved
with, so yours would start it differently, and the snapshot wouldn't fit.

## Sharing a high score

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

## Opening a replay

Double-click a `.lwreplay` file, or drop one on the window, and
LittleWing.app watches it from 0:00, with the keys above. Whatever was
playing stops first, and a game's preferences and recording are saved as
on quit. Opening another file switches to it, and stops an export. When the replay ends, the
last frame stays up until you close the window, which quits the app.
**File > Open Recent** lists the recordings watched lately, however they
were opened, and **Clear Menu** forgets them.

To watch a shared replay you need Loony Labyrinth installed: the replay
uses your registration in place of the one left out, or, if you have none
saved yet, the one every game starts with (see [Registration](development.md#registration)). The game
only checks the registration and never draws it, so the frames match the
recording, but for one thing: the game keeps its high scores only under the
registration they were saved with, so when yours isn't the recorder's it
starts with its first high-score table, and a replay that shows the table
desyncs there. The replay also needs the same version of LittleWing, or it
may desync. From a terminal, `LOONY_REPLAY_LICENSE` names the preferences to
take the registration from; by default they're your own copy's, in
`littlewing-shim/loony-labyrinth/prefs.plist`, and `default` takes the one
every game starts with. Shared files are format 6,
like recordings, with a flag that says the registration is out. One that
came without a snapshot, or with one that doesn't fit, keeps a snapshot of
the game's start in it the first time you watch it, as for your own.

## What a recording holds

While recording, the game's clock moves in whole milliseconds. When the
game spins on `TickCount` waiting for the next tick, it sleeps instead, and
when it spins on `Microseconds` and `Delay(0)` (Loony Labyrinth's menus do,
the pause menu among them), it sleeps until the next millisecond after three
polls in the same one. The
file holds none of the game itself, but its preferences hold the
registration (e-mail address and key code) and the high-score names, and the
keys record anything typed. Share a recording through the High Scores menu,
which takes the registration out, or only as you would those.
