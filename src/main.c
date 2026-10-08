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
#include "cgimage.h"
#include "cpu.h"
#include "cxxrt.h"
#include "dialogs.h"
#include "display.h"
#include "events.h"
#include "files.h"
#include "game.h"
#include "guest_mem.h"
#include "hd.h"
#include "libc.h"
#include "loader.h"
#include "memmgr.h"
#include "misc.h"
#include "picker.h"
#include "qd.h"
#include "rsrc.h"
#include "script.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

/* Launched as the app (from Finder or `open`), there is no terminal: the
   log goes to ~/Library/Logs/loony-shim/loony.log (the one before it is kept
   as loony.previous.log; after a restart for the picker, the same log
   carries on), and failures are shown in a message box. */
static char log_path[PATH_MAX];

static void show_failure(const char *msg) {
    const char *video = getenv("SDL_VIDEO_DRIVER");
    if (video && strcmp(video, "dummy") == 0) /* headless: nobody to click it */
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
    snprintf(dir, sizeof dir, "%s/Library/Logs/loony-shim", home);
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

/* An error before the game starts: printed, and shown when running as the app. */
static int startup_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int startup_error(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "loony: %s\n", msg);
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

/* Set when the picker chose the game: its own quit goes back to the picker. */
static bool return_to_picker;

/* The game ended by itself (ExitToShell, or its main returned). After a
   pick, and unless the host asked to quit or something failed, the
   preferences are saved and the process restarts itself to show the picker
   again: the emulator's state can't be reset in place. */
static void back_to_picker(void) {
    if (!return_to_picker || events_quit_requested() || util_failed())
        return;
    cf_save_prefs();
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) {
        log_msg("can't find this program to go back to the picker");
        return;
    }
    setenv("LOONY_FULLSCREEN", display_fullscreen() ? "1" : "0", 1);
    if (log_path[0])
        setenv("LOONY_LOG_INHERITED", "1", 1);
    log_msg("back to the picker");
    fflush(stderr);
    execv(self, (char *const[]){self, NULL});
    log_msg("can't restart for the picker: %s", strerror(errno));
}

int main(int argc, char **argv) {
    log_to_file_if_app(argv[0]);
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
    char err[256];
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
    if (files_init(dir, have_data ? data_dir : NULL)) {
        char prefs[PATH_MAX + 16];
        snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
        cf_load_prefs(prefs);
        atexit(save_prefs_at_exit);
    }
    qd_init(800, 600, macho ? 32 : 8);
    dialogs_init();
    cgimage_init();
    events_init();
    sound_init();
    display_init();
    hd_init();
    display_set_title(game->title);
    events_set_present(display_present_if_dirty);
    events_set_poll(display_poll);
    events_set_screenshot(display_write_png);
    events_set_cursor(display_set_cursor);
    static const display_input input = {events_post_key, events_post_activation,
                                        events_request_quit, events_post_mouse, events_post_text};
    display_set_input(&input);
    display_set_vsync(!misc_fixed_clock());
    sound_start_output();
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
    events_register();
    files_register();
    sound_register();

    if (macho) {
        log_msg("loaded %s: %u imports, main at 0x%x, %u initializers", path, img.nnames,
                img.main_addr, img.ninit);
        /* What dyld does before the entry point: the static constructors.
           Then main, as crt1's _start would call it. */
        for (uint32_t i = 0; i < img.ninit; i++)
            guest_call(img.init_addrs[i], 0, NULL);
        uint32_t args[4] = {1};
        libc_main_args(&args[1], &args[2], &args[3]);
        uint32_t status = guest_call(img.main_addr, 4, args);
        back_to_picker();
        log_msg("main returned %d", (int32_t)status);
        return 0;
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
    guest_call(img.main_tvector, 0, NULL);
    back_to_picker();
    log_msg("main returned");
    return 0;
}
