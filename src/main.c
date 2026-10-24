#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cf.h"
#include "cgdisplay.h"
#include "cgimage.h"
#include "clip.h"
#include "coreaudio.h"
#include "cpu.h"
#include "cxxrt.h"
#include "dialogs.h"
#include "display.h"
#include "events.h"
#include "files.h"
#include "game.h"
#include "gamestart.h"
#include "guest_mem.h"
#include "hd.h"
#include "libc.h"
#include "loader.h"
#include "measure.h"
#include "memmgr.h"
#include "menu.h"
#include "misc.h"
#include "patch.h"
#include "picker.h"
#include "play.h"
#include "qd.h"
#include "recordings.h"
#include "replay.h"
#include "rsrc.h"
#include "script.h"
#include "seek.h"
#include "settings.h"
#include "sound.h"
#include "trap.h"
#include "util.h"
#include "watch.h"

/* A local build can define this, in a file of its own, to change a game's
   code after it loads. Releases have only this one, which does nothing. */
__attribute__((weak)) void local_patch_hook(const char *game_id) { (void)game_id; }

/* Launched as the app (from Finder or `open`), there is no terminal: the
   log goes to ~/Library/Logs/littlewing-shim/loony.log (the one before it is kept
   as loony.previous.log; after a restart for the picker, the same log
   carries on), and failures are shown in a message box. */
static char log_path[PATH_MAX];

static void show_failure(const char *msg) {
    const char *video = getenv("SDL_VIDEO_DRIVER");
    const char *progress = getenv("LOONY_CLIP_PROGRESS");
    if (video && strcmp(video, "dummy") == 0) /* headless: nobody to click it */
        return;
    if (progress && strcmp(progress, "1") == 0) /* an export: the program running it says so */
        return;
    char text[2048];
    if (log_path[0])
        snprintf(text, sizeof text, "%s\n\nThe log is in %s", msg, log_path);
    else
        snprintf(text, sizeof text, "%s", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LittleWing", text, NULL);
}

static void log_to_file_if_app(const char *argv0) {
    const char *home = getenv("HOME");
    if (!strstr(argv0, ".app/Contents/MacOS/") || isatty(STDERR_FILENO) || !home)
        return;
    char dir[PATH_MAX], prev[PATH_MAX + 32];
    snprintf(dir, sizeof dir, "%s/Library/Logs/littlewing-shim", home);
    snprintf(log_path, sizeof log_path, "%s/loony.log", dir);
    snprintf(prev, sizeof prev, "%s/loony.previous.log", dir);
    util_set_failure_hook(show_failure);
    const char *inherited = getenv("LOONY_LOG_INHERITED");
    if (inherited && strcmp(inherited, "1") == 0) /* restarted for the picker: stderr is already the log */
        return;
    if (!make_dirs(dir)) {
        log_path[0] = '\0';
        return;
    }
    rename(log_path, prev);
    if (freopen(log_path, "w", stderr))
        setvbuf(stderr, NULL, _IOLBF, 0);
    else
        log_path[0] = '\0';
}

/* At exit, save what the game put in its preferences, unless it crashed:
   macOS would have kept those values even if the game never called
   CFPreferencesAppSynchronize (say, quit during a sequence that outlasted
   the quit grace period). */
static void save_prefs_at_exit(void) {
    if (!util_failed())
        cf_save_prefs();
}

/* The event pump's present: when a frame check is due (a recording or a
   replay), the screen's hash goes to replay_frame first. A clip, or a
   watched replay, takes the frame in place of the window, and a take
   (gamestart.h) has none. */
static void present_frame(void) {
    bool dirty = qd_take_dirty();
    if (dirty && replay_frame_due()) {
        qd_pixels px;
        qd_palette pal;
        qd_screen(&px, &pal);
        uint32_t h = fnv1a32(px.base, (size_t)px.row_bytes * (size_t)rect_h(px.bounds));
        if (px.depth <= 8)
            h ^= fnv1a32(pal.c, sizeof pal.c[0] * (size_t)pal.n);
        replay_frame(h);
    }
    if (clip_on())
        clip_present(dirty);
    else if (watch_on())
        watch_present(dirty);
    else if (dirty && !gamestart_taking())
        display_present();
    measure_present();
}

/* The event pump's poll: the window's events, and the takes of the
   games recorded, in the background (gamestart.h). */
static void poll_host(void) {
    display_poll();
    gamestart_tend();
}

/* A game's recording has ended (replay_set_game_recorded): its snapshot
   of the game's start is taken in the background. */
static void take_later(const char *path, uint64_t play_on_us) {
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) == 0)
        gamestart_take_later(self, path, play_on_us);
}

/* An error before the game starts: printed, and shown when running as the
   app or reported to the program running an export (clip_progress_error). */
static int startup_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int startup_error(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "loony: %s\n", msg);
    clip_progress_error(msg);
    util_report_failure(msg);
    return 1;
}

/* The games' folders, for the message when none is installed. */
static int no_game_error(void) {
    char msg[2048] = "no LittleWing game found: put ";
    for (size_t i = 0; i < game_count(); i++) {
        char dir[PATH_MAX];
        game_folder(game_at(i), dir, sizeof dir);
        size_t used = strlen(msg);
        snprintf(msg + used, sizeof msg - used, "%s%s in %s", i ? " or " : "", game_at(i)->title, dir);
    }
    return startup_error("%s", msg);
}

/* The loader's resolver for a Mach-O game's data imports. */
static uint32_t macho_data_symbol(const char *name) {
    uint32_t a = libc_data_symbol(name);
    if (!a)
        a = cxxrt_data_symbol(name);
    if (!a)
        a = cf_data_symbol(name);
    return a;
}

/* How long the app waits at launch for macOS to say a recording was opened
   to launch it. */
#define LAUNCH_EVENTS_MS 150

/* Set when the picker chose the game: its own quit goes back to the picker. */
static bool return_to_picker;

/* Restarts this program in place, in the same window mode and log, for
   why (logged): the emulator's state can't be reset in place. Returns only
   if it can't. */
static void restart_self(const char *why) {
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) {
        log_msg("can't find this program to restart it");
        return;
    }
    setenv("LOONY_FULLSCREEN", display_fullscreen() ? "1" : "0", 1);
    if (log_path[0])
        setenv("LOONY_LOG_INHERITED", "1", 1);
    display_log_stats(); /* exec skips the atexit handlers */
    replay_finish();
    gamestart_take_end();
    log_msg("%s", why);
    fflush(stderr);
    execv(self, (char *const[]){self, NULL});
    log_msg("can't restart: %s", strerror(errno));
}

/* The game ended by itself (ExitToShell, or its main returned). A watched
   replay holds its last frame first, and a seek from there goes back into
   the game (seek.h) instead of returning. After a pick, and unless the host
   asked to quit or something failed, the preferences are saved and the
   process restarts itself to show the picker again. */
static void back_to_picker(void) {
    watch_hold();
    if (!return_to_picker || events_quit_requested() || util_failed())
        return;
    cf_save_prefs();
    unsetenv("LOONY_RECORD"); /* the next game doesn't record over this one */
    unsetenv("LOONY_REPLAY");
    unsetenv("LOONY_WATCH");
    restart_self("back to the picker");
}

/* A recording opened from Finder, dropped on the window or chosen from the
   Replays or Open Recent menu (display.h): the program restarts to watch
   it (watch.h) from from_ticks of play, in a save folder of its own,
   whatever is playing. A game's preferences are saved first, and its
   recording ends where it is (restart_self), so the recording being made
   can be opened too. */
static void open_replay_at(const char *path, uint32_t from_ticks) {
    size_t n = strlen(path);
    char full[PATH_MAX];
    if (n < 9 || strcmp(path + n - 9, ".lwreplay") != 0 || clip_on()) {
        log_msg("not opening %s: not a replay", path);
        return;
    }
    char why[512];
    if (!realpath(path, full)) {
        log_msg("can't open %s: %s", path, strerror(errno));
        return;
    }
    if (!replay_check(full, why, sizeof why)) { /* said here: the game goes on */
        const char *video = getenv("SDL_VIDEO_DRIVER");
        char msg[PATH_MAX + 600];
        log_msg("can't open %s: %s", full, why);
        snprintf(msg, sizeof msg, "LittleWing can't play %s: %s.", strrchr(full, '/') + 1, why);
        if (!video || strcmp(video, "dummy") != 0)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "LittleWing", msg, NULL);
        return;
    }
    if (!replay_playing())
        cf_save_prefs();
    watch_stop_export(); /* it would go on unseen */
    recordings_watch_env(full, from_ticks);
    restart_self("watch: opening a replay");
}

/* The recording being watched was deleted (recordings_delete): the
   program restarts as if launched, to the picker when there is one, and
   says why there. */
static void close_replay(const char *why) {
    watch_stop_export(); /* it would go on unseen */
    recordings_leave_env();
    setenv("LOONY_FLASH", why, 1);
    restart_self("watch: closing the replay");
}

int main(int argc, char **argv) {
    log_to_file_if_app(argv[0]);
    char err[256];
    if (!replay_open(err, sizeof err)) /* sets the game, save folder and alerts it needs */
        return startup_error("%s", err);
    if (!clip_open(err, sizeof err))
        return startup_error("%s", err);
    if (!watch_open(err, sizeof err))
        return startup_error("%s", err);
    display_set_open(open_replay_at);
    display_set_close(close_replay);
    /* Launched by opening a recording in Finder: watch it, before the
       picker shows or a game starts recording (which deletes old ones). */
    const char *replaying = getenv("LOONY_REPLAY");
    if (strstr(argv[0], ".app/Contents/MacOS/") && (!replaying || !*replaying))
        display_take_launch_events(LAUNCH_EVENTS_MS);
    files_move_old_data_root(); /* before the picker reads its file there */
    int64_t volume;
    if (settings_get_int(SETTINGS_VOLUME, &volume)) /* before the Sound menu shows it */
        sound_set_volume((int)volume);
    const char *dir_arg = NULL;
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-psn_", 5) == 0) /* older macOS adds this when launching an app */
            continue;
        dir_arg = argv[i];
        nargs++;
    }
    if (nargs > 1)
        return startup_error("usage: loony [game-folder]");

    /* The game: the one in the folder given; else LOONY_GAME's; else the
       one installed, or the picker's choice when there are more. */
    const game_info *game;
    char dir[PATH_MAX];
    const char *forced = getenv("LOONY_GAME");
    if (dir_arg) {
        snprintf(dir, sizeof dir, "%s", dir_arg);
        game = game_in_folder(dir);
        if (!game)
            game = game_at(0); /* names the program expected, in the message below */
    } else if (forced && *forced) {
        game = game_by_id(forced);
        if (!game)
            return startup_error("LOONY_GAME names no known game: %s", forced);
        game_folder(game, dir, sizeof dir);
    } else {
        const game_info *installed[8];
        int n = game_installed(installed, 8);
        if (n == 0)
            return no_game_error();
        if (n == 1) {
            game = installed[0];
        } else {
            menu_add_replays(); /* the picker's menu bar has them */
            game = picker_run(installed, n);
            return_to_picker = true;
        }
        game_folder(game, dir, sizeof dir);
    }

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, game->exe);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf)
        return startup_error("can't read %s: %s. %s needs the original game in %s.", path,
                             strerror(errno), game->title, dir);
    log_msg("playing %s from %s", game->title, dir);
    const bool macho = game->kind == GAME_MACHO_BUNDLE;

    loaded_image img;
    if (macho) {
        /* The loader asks for the data objects the program imports, so the
           heap and the libraries that own them come first. */
        gm_init_layout(GM_LAYOUT_MACHO);
        cpu_init();
        mm_init();
        cf_init();
        cf_set_bundle(dir);
        libc_init(path);
        cxxrt_init();
        image_set_data_resolver(macho_data_symbol);
        if (!image_load_macho(buf, len, &img, err, sizeof err))
            return startup_error("can't load %s: %s", path, err);
        /* Only MONSTER FAIR 1.2.5 is supported, and its second license check
           is always off (see patch.h). */
        if (strcmp(game->id, "monster-fair") == 0) {
            if (!patch_mf_skip_license_recheck(err, sizeof err))
                return startup_error("MONSTER FAIR 1.2.5 is the only version supported: %s", err);
            log_msg("MONSTER FAIR 1.2.5: its second license check is off");
        }
        local_patch_hook(game->id);
        rsrc_open_empty();
        misc_init();
        misc_set_system_version(MISC_SYSTEM_VERSION_MACHO);
        misc_set_exit_hook(back_to_picker);
    } else {
        char fork_path[PATH_MAX + 32];
        snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", path);
        size_t fork_len = 0;
        uint8_t *fork = read_file(fork_path, &fork_len);
        if (!fork)
            return startup_error("can't read %s: %s", fork_path, strerror(errno));

        gm_init();
        cpu_init();
        if (!image_load(buf, len, &img, err, sizeof err))
            return startup_error("can't load %s: %s", path, err);
        /* Code runs from the code section alone, which the game's files
           fill and a snapshot from a file never changes (gamestart.h). */
        uint32_t code_end = (img.code_base + img.code_len + 0xFFFu) & ~0xFFFu;
        cpu_no_exec(GUEST_IMAGE_BASE, img.code_base);
        cpu_no_exec(code_end, GUEST_IMAGE_LIMIT);
        local_patch_hook(game->id);
        if (!rsrc_open(fork, fork_len, err, sizeof err))
            return startup_error("can't load the resources of %s: %s", path, err);
        mm_init();
        misc_init();
        misc_set_exit_hook(back_to_picker);
        cf_init();
    }
    char data_root[PATH_MAX], data_dir[PATH_MAX];
    if (files_data_root(data_root, sizeof data_root))
        game_move_legacy_data(data_root);
    bool have_data = files_data_dir(game->id, data_dir, sizeof data_dir);
    if (!have_data)
        log_msg("neither LOONY_DATA_DIR nor HOME is set: nothing will be saved");
    bool loony = strcmp(game->id, "loony-labyrinth") == 0; /* replays are its alone, for now */
    if (loony && !clip_on())
        menu_add_replays();
    else
        menu_remove_replays(); /* the picker had them */
    if (loony) {
        char rec_dir[PATH_MAX];
        if (!recordings_dir(rec_dir, sizeof rec_dir))
            rec_dir[0] = '\0';
        time_t scores[16];
        size_t nscores = 0;
        if (have_data) {
            char prefs[PATH_MAX + 16];
            snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
            nscores = game_high_score_times(prefs, scores, 16);
        }
        if (rec_dir[0]) {
            replay_record_into(rec_dir, game->id, scores, nscores);
            replay_set_game_recorded(take_later);
        }
        const char *replay = getenv("LOONY_REPLAY");
        if (rec_dir[0] && (!replay || !*replay) && !clip_on())
            menu_add_high_scores(game->id, game->title, rec_dir);
    }
    if (!replay_start(game->id, buf, len, have_data ? data_dir : NULL, err, sizeof err))
        return startup_error("%s", err);
    bool saving = files_init(dir, have_data ? data_dir : NULL);
    if (macho) { /* the folder the program is in, where a Carbon program's relative paths start */
        char exe_dir[PATH_MAX];
        snprintf(exe_dir, sizeof exe_dir, "%s", game->exe);
        char *slash = strrchr(exe_dir, '/');
        *(slash ? slash : exe_dir) = '\0';
        files_set_default_dir(exe_dir);
    }
    if (saving) {
        char prefs[PATH_MAX + 16];
        snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
        cf_load_prefs(prefs);
        if (!clip_on()) /* a clip's replay saves into a folder nobody reads */
            atexit(save_prefs_at_exit);
    }
    qd_init(game->screen_width, game->screen_height, macho ? 32 : 8);
    dialogs_init();
    cgimage_init();
    cgdisplay_init();
    events_init();
    sound_init();
    coreaudio_init();
    display_init();
    char self[PATH_MAX];
    uint32_t self_size = sizeof self;
    hd_init(game->id, _NSGetExecutablePath(self, &self_size) == 0 ? self : argv[0]);
    measure_init();
    display_set_title(game->title);
    events_set_present(present_frame);
    events_set_poll(poll_host);
    events_set_screenshot(display_write_png);
    events_set_cursor(display_set_cursor);
    static const display_input input = {events_host_key, events_host_activation,
                                        events_host_quit, events_host_mouse, events_host_text,
                                        replay_mark};
    display_set_input(&input);
    display_set_vsync(!misc_fixed_clock() && !replay_playing());
    sound_start_output();
    seek_init();
    if (watch_on()) {
        events_set_replay_key(watch_key);
        watch_start(game->title);
        menu_add_replay();
        menu_note_recent(getenv("LOONY_REPLAY")); /* for File > Open Recent */
    } else if (replay_playing() && seek_verifying()) {
        seek_start(NULL); /* snapshots to verify, and no seeks */
    }
    gamestart_init(); /* after seek_start: its hook runs seek's */
    const char *script = getenv("LOONY_SCRIPT");
    if (script && *script && !script_load(script, err, sizeof err)) {
        fprintf(stderr, "loony: can't load the script %s: %s\n", script, err);
        return 1;
    }
    misc_set_idle(events_pump);

    trap_init(img.nnames, img.names, img.code_base, img.code_len);
    if (macho) {
        trap_set_direct_calls(true);
        libc_register();
        cxxrt_register();
    }
    mm_register();
    rsrc_register();
    misc_register();
    cf_register();
    qd_register();
    dialogs_register();
    cgimage_register();
    cgdisplay_register();
    events_register();
    files_register();
    sound_register();
    coreaudio_register();

    if (macho) {
        log_msg("loaded %s: %u imports, main at 0x%x, %u initializers", path, img.nnames,
                img.main_addr, img.ninit);
        /* What dyld does before the entry point: the static constructors.
           Then main, as crt1's _start would call it, and exit with what it
           returns. */
        for (uint32_t i = 0; i < img.ninit; i++)
            guest_call(img.init_addrs[i], 0, NULL);
        uint32_t args[4] = {1};
        libc_main_args(&args[1], &args[2], &args[3]);
        volatile uint32_t status = 0;
        if (setjmp(seek_unwound) == 0) /* where a seek unwinds to from inside a trap */
            status = guest_call(img.main_addr, 4, args);
        else /* replays are Loony Labyrinth's, so this game never seeks: status stays 0 */
            seek_reenter();
        misc_main_returned((int32_t)status); /* runs back_to_picker, the exit hook */
    }

    int32_t app_id = image_find_import(&img, "kCFPreferencesCurrentApplication");
    if (app_id >= 0)
        gm_w32(img.import_addr[app_id], cf_current_app());

    if (!img.main_tvector)
        fatal("%s has no main entry point", path);
    log_msg("loaded %s: %u imports, main at code+0x%05x", path, img.pef.nimports,
            gm_r32(img.main_tvector) - img.code_base);

    if (img.init_tvector)
        guest_call(img.init_tvector, 0, NULL);
    play_init(game->id, img.code_base, gm_r32(img.main_tvector + 4));
    if (setjmp(seek_unwound) == 0) /* where a seek unwinds to from inside a trap */
        guest_call(img.main_tvector, 0, NULL);
    else
        seek_reenter();
    misc_exit("main returned", 0); /* logged before back_to_picker, the exit hook */
}
