#include "play.h"

#include <stddef.h>
#include <string.h>

#include "cpu.h"
#include "guest_mem.h"
#include "measure.h"
#include "replay.h"
#include "util.h"

/* Loony Labyrinth 3.0.1's code and data (see play.h). */
#define MACH_START_NEW_GAME 0x1df90
#define EVENT_RESET_GAME 0x15400
#define IN_LOOP 0x4e36   /* Mach_ProcessingLoop is running */
#define DEMO 0x4dd1      /* and it is the self-playing demo */
#define SUSPENDED 0x4e35 /* a game waits under the pause menu */
#define GAME_OVER 0x4e34

/* Instructions that must be 3.0.1's: the entries hooked, and the ones that
   name each flag. */
static const struct {
    uint32_t offset, word;
} expected[] = {
    {0x1df90, 0x7c0802a6}, /* Mach_StartNewGame: mflr r0 */
    {0x1df9c, 0x3be24e36}, /*   addi r31, r2, IN_LOOP */
    {0x15400, 0x7c0802a6}, /* Event_ResetGame: mflr r0 */
    {0x15408, 0x3be24e35}, /*   addi r31, r2, SUSPENDED */
    {0x155c0, 0x38624dd1}, /* Event_StartDemo: addi r3, r2, DEMO */
    {0x15538, 0x38624e34}, /* Event_StartGame: addi r3, r2, GAME_OVER */
};

static struct {
    bool on;
    uint32_t toc;
    bool started; /* Mach_StartNewGame ran for a game since the last pump */
    bool reset;   /* Event_ResetGame threw a suspended game away */
    bool in_game, playing;
} P;
MEASURE_STATE(P)

void play_reset(void) { memset(&P, 0, sizeof P); }

static bool flag(uint32_t offset) { return gm_r8(P.toc + offset) != 0; }

static void on_start_new_game(void) {
    if (!flag(DEMO))
        P.started = true;
}

static void on_reset_game(void) {
    if (flag(SUSPENDED))
        P.reset = true;
}

bool play_init(const char *game_id, uint32_t code_base, uint32_t toc) {
    play_reset();
    if (strcmp(game_id, "loony-labyrinth") != 0 || replay_playing())
        return false;
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        if (gm_r32(code_base + expected[i].offset) != expected[i].word) {
            log_msg("play: this isn't Loony Labyrinth 3.0.1's code: games aren't told apart");
            replay_no_games();
            return false;
        }
    }
    P.on = true;
    P.toc = toc;
    cpu_on_exec(code_base + MACH_START_NEW_GAME, on_start_new_game);
    cpu_on_exec(code_base + EVENT_RESET_GAME, on_reset_game);
    return true;
}

static void set_playing(bool playing) {
    if (playing == P.playing)
        return;
    P.playing = playing;
    replay_play(playing);
}

static void end_game(const char *why) {
    set_playing(false);
    P.in_game = false;
    log_msg("play: the game ends (%s)", why);
    replay_game(false);
}

void play_pump(bool active) {
    if (!P.on)
        return;
    bool in_loop = flag(IN_LOOP) && !flag(DEMO);
    if (P.started) {
        P.started = P.reset = false;
        if (P.in_game)
            end_game("another starts");
        P.in_game = true;
        log_msg("play: a game starts");
        replay_game(true);
    } else if (P.in_game && P.reset) {
        P.reset = false;
        end_game("thrown away");
    } else if (P.in_game && !in_loop && flag(GAME_OVER) && !flag(SUSPENDED)) {
        end_game("game over");
    }
    set_playing(P.in_game && in_loop && active);
}

bool play_in_game(void) { return P.in_game; }
bool play_playing(void) { return P.playing; }
