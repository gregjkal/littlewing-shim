#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cf.h"
#include "cpu.h"
#include "dialogs.h"
#include "display.h"
#include "events.h"
#include "files.h"
#include "game.h"
#include "guest_mem.h"
#include "loader.h"
#include "memmgr.h"
#include "misc.h"
#include "qd.h"
#include "rsrc.h"
#include "script.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

#define DEFAULT_GAME_DIR "/Applications/Loony Labyrinth"
#define GAME_EXE_NAME "LOONY LABYRINTH 3.0.1"

/* Launched as the app (from Finder or `open`), there is no terminal: the
   log goes to ~/Library/Logs/loony-shim/loony.log (the one before it is kept
   as loony.previous.log), and failures are shown in a message box. */
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
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Loony Labyrinth", text, NULL);
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

int main(int argc, char **argv) {
    log_to_file_if_app(argv[0]);
    const char *dir = DEFAULT_GAME_DIR;
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-psn_", 5) == 0) /* older macOS adds this when launching an app */
            continue;
        dir = argv[i];
        nargs++;
    }
    if (nargs > 1)
        return startup_error("usage: loony [game-folder]");

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, GAME_EXE_NAME);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf)
        return startup_error("can't read %s: %s. Loony Labyrinth needs the original game in %s.", path,
                             strerror(errno), dir);

    char fork_path[PATH_MAX + 32];
    snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", path);
    size_t fork_len = 0;
    uint8_t *fork = read_file(fork_path, &fork_len);
    if (!fork)
        return startup_error("can't read %s: %s", fork_path, strerror(errno));

    gm_init();
    cpu_init();
    loaded_image img;
    char err[256];
    if (!image_load(buf, len, &img, err, sizeof err))
        return startup_error("can't load %s: %s", path, err);
    if (!rsrc_open(fork, fork_len, err, sizeof err))
        return startup_error("can't load the resources of %s: %s", path, err);
    mm_init();
    misc_init();
    cf_init();
    char data_dir[PATH_MAX];
    bool have_data = files_data_dir(game_at(0)->id, data_dir, sizeof data_dir);
    if (!have_data)
        log_msg("neither LOONY_DATA_DIR nor HOME is set: nothing will be saved");
    if (files_init(dir, have_data ? data_dir : NULL)) {
        char prefs[PATH_MAX + 16];
        snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
        cf_load_prefs(prefs);
        atexit(save_prefs_at_exit);
    }
    qd_init(800, 600, 8);
    dialogs_init();
    events_init();
    sound_init();
    display_init();
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

    const char **names = calloc(img.pef.nimports ? img.pef.nimports : 1, sizeof *names);
    if (!names)
        fatal("out of memory");
    for (uint32_t i = 0; i < img.pef.nimports; i++)
        names[i] = img.pef.imports[i].name;
    trap_init(img.pef.nimports, names, img.code_base, img.code_len);
    mm_register();
    rsrc_register();
    misc_register();
    cf_register();
    qd_register();
    dialogs_register();
    events_register();
    files_register();
    sound_register();
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
    log_msg("main returned");
    return 0;
}
