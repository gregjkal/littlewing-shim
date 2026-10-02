#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf.h"
#include "cpu.h"
#include "dialogs.h"
#include "display.h"
#include "events.h"
#include "files.h"
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

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: loony [game-folder]\n");
        return 1;
    }
    const char *dir = argc == 2 ? argv[1] : DEFAULT_GAME_DIR;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, GAME_EXE_NAME);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "loony: can't read %s: %s\n", path, strerror(errno));
        return 1;
    }

    char fork_path[PATH_MAX + 32];
    snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", path);
    size_t fork_len = 0;
    uint8_t *fork = read_file(fork_path, &fork_len);
    if (!fork) {
        fprintf(stderr, "loony: can't read %s: %s\n", fork_path, strerror(errno));
        return 1;
    }

    gm_init();
    cpu_init();
    loaded_image img;
    char err[256];
    if (!image_load(buf, len, &img, err, sizeof err)) {
        fprintf(stderr, "loony: can't load %s: %s\n", path, err);
        return 1;
    }
    if (!rsrc_open(fork, fork_len, err, sizeof err)) {
        fprintf(stderr, "loony: can't load the resources of %s: %s\n", path, err);
        return 1;
    }
    mm_init();
    misc_init();
    cf_init();
    char data_dir[PATH_MAX];
    bool have_data = files_data_dir(data_dir, sizeof data_dir);
    if (have_data) {
        char prefs[PATH_MAX + 16];
        snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
        cf_load_prefs(prefs);
    } else {
        log_msg("neither LOONY_DATA_DIR nor HOME is set: nothing will be saved");
    }
    qd_init(800, 600, 8);
    dialogs_init();
    events_init();
    files_init(dir, have_data ? data_dir : NULL);
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
