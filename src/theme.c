#include "theme.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread/spawn.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "export.h"
#include "files.h"
#include "misc.h"
#include "sound.h"
#include "util.h"
#include "wav.h"

static bool loud(const int16_t *s, uint32_t i) {
    return abs(s[2 * i]) > THEME_QUIET || abs(s[2 * i + 1]) > THEME_QUIET;
}

uint32_t theme_clip(const int16_t *samples, uint32_t frames, uint32_t from, int16_t **out) {
    *out = NULL;
    uint32_t start = from;
    while (start < frames && !loud(samples, start))
        start++;
    if (start >= frames)
        return 0;
    const uint32_t max = THEME_MAX_SECONDS * THEME_RATE, gap = THEME_GAP_MS * THEME_RATE / 1000;
    uint32_t len = 0, quiet = 0;
    bool ended = false; /* by a gap: nothing to fade */
    for (uint32_t i = start; i < frames && i - start < max; i++) {
        if (loud(samples, i)) {
            quiet = 0;
            len = i - start + 1;
        } else if (++quiet >= gap) {
            ended = true;
            break;
        }
    }
    if (!ended)
        len = frames - start < max ? frames - start : max;
    int16_t *clip = malloc((size_t)len * 4);
    if (!clip)
        return 0;
    memcpy(clip, samples + 2 * (size_t)start, (size_t)len * 4);
    uint32_t fade = THEME_FADE_MS * THEME_RATE / 1000;
    if (!ended && fade > len)
        fade = len;
    for (uint32_t i = ended ? len : len - fade; i < len; i++)
        for (int c = 0; c < 2; c++)
            clip[2 * i + c] = (int16_t)((int64_t)clip[2 * i + c] * (len - i) / fade);
    *out = clip;
    return len;
}

/* ---- the clips ---- */

#define THEME_GAMES 16

typedef struct {
    const game_info *game;
    char path[PATH_MAX]; /* its clip, THEME_FILE in its save folder */
    bool have;           /* the clip file is there (maybe empty) */
    bool loaded;         /* clip and frames hold it */
    bool failed;         /* its render failed: not again this time */
    int16_t *clip;
    uint32_t frames;
} entry;

static struct {
    bool on;
    int n, selected;
    entry e[THEME_GAMES];
    char root[PATH_MAX], self[PATH_MAX];
    SDL_AudioStream *stream;
    int volume;
    pid_t pid;  /* the render running, 0 for none */
    int render; /* its entry */
    char temp[PATH_MAX];
} T;

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st, (void)flag, (void)ftw;
    remove(path);
    return 0;
}

static void remove_tree(const char *path) { nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS); }

/* Removes the folders of renders whose program has ended without
   removing them (a crash): THEME_TEMP_PREFIX<pid>-XXXXXX, where pid is the
   program that made it. Another copy's, still running, is left alone,
   unless the folder is older than any render (its pid since reused). */
#define LEFTOVER_AGE (60 * 60) /* seconds */
static void remove_leftovers(void) {
    DIR *d = opendir(T.root);
    struct dirent *de;
    const size_t n = strlen(THEME_TEMP_PREFIX);
    while (d && (de = readdir(d))) {
        if (strncmp(de->d_name, THEME_TEMP_PREFIX, n) != 0)
            continue;
        char path[PATH_MAX * 2];
        snprintf(path, sizeof path, "%s/%s", T.root, de->d_name);
        pid_t owner = (pid_t)strtol(de->d_name + n, NULL, 10);
        struct stat st;
        bool recent = stat(path, &st) == 0 && time(NULL) - st.st_mtime < LEFTOVER_AGE;
        if (recent && owner > 0 && owner != getpid() && (kill(owner, 0) == 0 || errno == EPERM))
            continue;
        remove_tree(path);
    }
    if (d)
        closedir(d);
}

static void load(entry *e) {
    if (e->loaded || !e->have)
        return;
    e->loaded = true;
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(e->path, &spec, &buf, &len)) {
        log_msg("picker music: can't read %s: %s", e->path, SDL_GetError());
        return;
    }
    if (spec.format != SDL_AUDIO_S16 || spec.channels != 2 || spec.freq != THEME_RATE) {
        log_msg("picker music: %s isn't 16-bit stereo at %d Hz", e->path, THEME_RATE);
    } else if (len >= 4 && (e->clip = malloc(len))) {
        memcpy(e->clip, buf, len);
        e->frames = len / 4;
    }
    SDL_free(buf);
}

static void play(int i) {
    if (!T.stream || i < 0 || i >= T.n)
        return;
    SDL_ClearAudioStream(T.stream);
    entry *e = &T.e[i];
    load(e);
    if (e->frames)
        SDL_PutAudioStreamData(T.stream, e->clip, (int)(e->frames * 4));
}

/* ---- rendering ---- */

extern char **environ;

static bool write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    bool ok = f && fputs(text, f) >= 0;
    return f ? fclose(f) == 0 && ok : false;
}

/* Starts the render of entry i, or returns false (logged). */
static bool spawn(int i) {
    entry *e = &T.e[i];
    const game_info *g = e->game;
    snprintf(T.temp, sizeof T.temp, "%s/" THEME_TEMP_PREFIX "%d-XXXXXX", T.root, (int)getpid());
    if (!make_dirs(T.root) || !mkdtemp(T.temp)) {
        log_msg("picker music: can't make a folder in %s: %s", T.root, strerror(errno));
        return false;
    }
    char dir[PATH_MAX], data[PATH_MAX + 8], script[PATH_MAX + 16];
    char set_data[PATH_MAX + 32], set_wav[PATH_MAX + 32], set_script[PATH_MAX + 32], set_exit[48];
    game_folder(g, dir, sizeof dir);
    snprintf(data, sizeof data, "%s/data", T.temp);
    snprintf(script, sizeof script, "%s/script.txt", T.temp);
    snprintf(set_data, sizeof set_data, "LOONY_DATA_DIR=%s", data);
    snprintf(set_wav, sizeof set_wav, "LOONY_WAV=%s/out.wav", T.temp);
    snprintf(set_script, sizeof set_script, "LOONY_SCRIPT=%s", script);
    snprintf(set_exit, sizeof set_exit, "LOONY_EXIT_AFTER=%d",
             g->theme_from + (THEME_MAX_SECONDS + 2) * 60);
    if (mkdir(data, 0755) != 0 || (g->theme_script && !write_text(script, g->theme_script))) {
        log_msg("picker music: can't write in %s: %s", T.temp, strerror(errno));
        remove_tree(T.temp);
        return false;
    }
    /* Registered, as the app's games are, and none of the debugging that
       would change what it plays or fill the log. The script last: without
       one, its NULL ends the list there. */
    const char *set[] = {set_data, set_wav, set_exit, "LOONY_FIXED_CLOCK=1", "LOONY_AUTO_ALERTS=1",
                         "LOONY_UNREGISTERED=0", "LOONY_TRACE=", "LOONY_STUB=", "LOONY_HD=off",
                         "SDL_VIDEO_DRIVER=dummy", "SDL_AUDIO_DRIVER=dummy",
                         "LOONY_LOG_INHERITED=1", g->theme_script ? set_script : NULL, NULL};
    char **env = export_env_with(environ, set);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    /* Only stdin, and stderr (this program's log), reach it. */
    posix_spawn_file_actions_addinherit_np(&actions, STDIN_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_UTILITY); /* below the picker */
    pid_t pid = 0;
    char *const argv[] = {T.self, dir, NULL};
    int rc = env ? posix_spawn(&pid, T.self, &actions, &attr, argv, env) : ENOMEM;
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    export_free_env(env);
    if (rc != 0) {
        log_msg("picker music: can't start %s: %s", T.self, strerror(rc));
        remove_tree(T.temp);
        return false;
    }
    T.pid = pid;
    T.render = i;
    log_msg("picker music: rendering %s's (process %d)", g->title, (int)pid);
    return true;
}

/* Keeps the clip in the finished render's recording as entry i's. */
static void keep(int i) {
    entry *e = &T.e[i];
    char wav[PATH_MAX + 16], tmp[PATH_MAX + 16];
    snprintf(wav, sizeof wav, "%s/out.wav", T.temp);
    snprintf(tmp, sizeof tmp, "%s/clip.wav", T.temp);
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(wav, &spec, &buf, &len)) {
        log_msg("picker music: can't read %s's recording: %s", e->game->title, SDL_GetError());
        e->failed = true;
        return;
    }
    int16_t *clip = NULL;
    uint32_t frames = theme_clip((const int16_t *)buf, len / 4,
                                 (uint32_t)e->game->theme_from * THEME_RATE / 60, &clip);
    SDL_free(buf);
    char dir[PATH_MAX];
    wav_file w;
    bool ok = files_data_dir(e->game->id, dir, sizeof dir) && make_dirs(dir) &&
              wav_open(&w, tmp, THEME_RATE);
    const char *why = ok ? NULL : "can't create it";
    if (ok) {
        wav_write(&w, clip, frames);
        if (!(ok = wav_close(&w)))
            why = "a write failed";
        else if (!(ok = rename(tmp, e->path) == 0))
            why = strerror(errno);
    }
    free(clip);
    if (!ok) {
        log_msg("picker music: can't write %s: %s", e->path, why);
        e->failed = true;
        return;
    }
    e->have = true;
    if (frames)
        log_msg("picker music: %s's is %.1f s, in %s", e->game->title, (double)frames / THEME_RATE,
                e->path);
    else
        log_msg("picker music: %s played nothing; %s is empty", e->game->title, e->path);
}

/* The next entry to render: the selected one if it needs it, else the first. */
static int next_render(void) {
    entry *s = &T.e[T.selected];
    if (!s->have && !s->failed)
        return T.selected;
    for (int i = 0; i < T.n; i++)
        if (!T.e[i].have && !T.e[i].failed)
            return i;
    return -1;
}

/* ---- the picker's calls ---- */

void theme_start(const game_info *const *games, int n, int selected, const char *self) {
    memset(&T, 0, sizeof T);
    const char *off = getenv("LOONY_PICKER_MUSIC");
    if ((off && strcmp(off, "0") == 0) || misc_fixed_clock_asked() || !self ||
        !files_data_root(T.root, sizeof T.root))
        return;
    T.n = n < THEME_GAMES ? n : THEME_GAMES;
    T.selected = selected;
    snprintf(T.self, sizeof T.self, "%s", self);
    for (int i = 0; i < T.n; i++) {
        entry *e = &T.e[i];
        e->game = games[i];
        char dir[PATH_MAX];
        if (files_data_dir(e->game->id, dir, sizeof dir))
            snprintf(e->path, sizeof e->path, "%s/" THEME_FILE, dir);
        else
            e->failed = true;
        e->have = !e->failed && access(e->path, F_OK) == 0;
    }
    remove_leftovers();
    T.on = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log_msg("picker music: no audio output: %s", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, THEME_RATE};
    T.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!T.stream) {
        log_msg("picker music: no audio output: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    T.volume = sound_volume();
    SDL_SetAudioStreamGain(T.stream, sound_gain(T.volume));
    SDL_ResumeAudioStreamDevice(T.stream);
}

void theme_select(int selected) {
    if (!T.on || selected < 0 || selected >= T.n)
        return;
    T.selected = selected;
    play(selected);
}

void theme_tend(void) {
    if (!T.on)
        return;
    if (T.stream && sound_volume() != T.volume) {
        T.volume = sound_volume();
        SDL_SetAudioStreamGain(T.stream, sound_gain(T.volume));
    }
    if (T.pid) {
        int st = 0;
        pid_t r = waitpid(T.pid, &st, WNOHANG);
        if (r == 0 || (r < 0 && errno == EINTR))
            return;
        T.pid = 0;
        entry *e = &T.e[T.render];
        if (r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            keep(T.render);
        } else {
            log_msg("picker music: rendering %s's failed (status 0x%x)", e->game->title, st);
            e->failed = true;
        }
        remove_tree(T.temp);
        if (T.render == T.selected && e->have)
            play(T.selected);
    }
    int i = next_render();
    if (i >= 0 && !spawn(i))
        T.e[i].failed = true;
}

void theme_stop(void) {
    if (T.pid) {
        kill(T.pid, SIGKILL);
        while (waitpid(T.pid, NULL, 0) < 0 && errno == EINTR)
            ;
        remove_tree(T.temp);
        log_msg("picker music: stopped rendering %s's", T.e[T.render].game->title);
    }
    if (T.stream) {
        SDL_DestroyAudioStream(T.stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    for (int i = 0; i < T.n; i++)
        free(T.e[i].clip);
    memset(&T, 0, sizeof T);
}
