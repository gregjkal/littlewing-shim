#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "guest_mem.h"
#include "loader.h"
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

    gm_init();
    cpu_init();
    loaded_image img;
    char err[256];
    if (!image_load(buf, len, &img, err, sizeof err)) {
        fprintf(stderr, "loony: can't load %s: %s\n", path, err);
        return 1;
    }

    const char **names = calloc(img.pef.nimports ? img.pef.nimports : 1, sizeof *names);
    if (!names)
        fatal("out of memory");
    for (uint32_t i = 0; i < img.pef.nimports; i++)
        names[i] = img.pef.imports[i].name;
    trap_init(img.pef.nimports, names, img.code_base, img.code_len);

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
