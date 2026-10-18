#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asm.h"
#include "cf.h"
#include "cgimage.h"
#include "events.h"
#include "files.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "mixer.h"
#include "snapshot.h"
#include "util.h"

/* Snapshots of a machine with no game (snapshot.h): the only checks of
   the walker that run where the games aren't installed. */

static const char *const names[] = {
    "NewEventLoopTimerUPP", "GetMainEventLoop", "InstallEventLoopTimer", "RunApplicationEventLoop",
    "QuitApplicationEventLoop", "SnapTest", "FSMakeFSSpec", "FSpOpenDF", "PBReadSync", "FSClose",
    "CFRelease", "CGDataProviderCreateWithURL", "CGDataProviderRelease",
};

#define PROC    (GUEST_IMAGE_BASE + 0x200)
#define TV_PROC (GUEST_IMAGE_BASE + 0x8100)
#define COUNTER (GUEST_IMAGE_BASE + 0x8200)
#define AREA    (GUEST_IMAGE_BASE + 0x9000) /* guest memory the hook fills */
#define AREA_LEN 0x2000u
#define FIRES   5

static char game[1024];

static uint32_t tv_of(const char *name) {
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(names[i], name) == 0)
            return HARNESS_TV_BASE + 8 * i;
    return 0;
}

static void h_snap_test(void) { trap_return(0); }

/* A fresh machine with every module the tests touch, and a game folder
   holding "data.bin", "0123456789". */
static void setup(void) {
    char root[1024];
    test_tmp_dir(root, sizeof root);
    snprintf(game, sizeof game, "%s/game", root);
    char data[1100];
    snprintf(data, sizeof data, "%s/data", root);
    if (!make_dirs(game))
        fatal("can't create %s", game);
    char p[1100];
    snprintf(p, sizeof p, "%s/data.bin", game);
    FILE *f = fopen(p, "wb");
    fputs("0123456789", f);
    fclose(f);
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    misc_register();
    events_init();
    events_register();
    cf_init();
    cf_register();
    files_init(game, data);
    files_register();
    mix_init();
    cgimage_init();
    cgimage_register();
    trap_register("SnapTest", h_snap_test);
}

static void teardown(void) {
    char root[1024];
    snprintf(root, sizeof root, "%s", game);
    *strrchr(root, '/') = '\0';
    test_remove_tree(root);
}

/* A timer proc that adds 1 to COUNTER, calls SnapTest, and calls
   QuitApplicationEventLoop once COUNTER reaches FIRES. */
static void emit_proc(void) {
    uint32_t tvs = tv_of("SnapTest"), tvq = tv_of("QuitApplicationEventLoop");
    uint32_t code[] = {
        PPC_MFLR_R0, PPC_SAVE_LR, PPC_PUSH64,
        ppc_lis(4, COUNTER >> 16), ppc_ori(4, 4, COUNTER & 0xFFFF),
        ppc_lwz(5, 0, 4), ppc_addi(5, 5, 1), ppc_stw(5, 0, 4),
        ppc_lis(12, tvs >> 16), ppc_ori(12, 12, tvs & 0xFFFF),
        ppc_lwz(0, 0, 12), ppc_lwz(2, 4, 12), PPC_MTCTR_R0, PPC_BCTRL,
        ppc_lis(4, COUNTER >> 16), ppc_ori(4, 4, COUNTER & 0xFFFF), ppc_lwz(5, 0, 4),
        ppc_cmpwi(5, FIRES), ppc_blt(7 * 4),
        ppc_lis(12, tvq >> 16), ppc_ori(12, 12, tvq & 0xFFFF),
        ppc_lwz(0, 0, 12), ppc_lwz(2, 4, 12), PPC_MTCTR_R0, PPC_BCTRL,
        PPC_POP64, PPC_LOAD_LR, PPC_MTLR_R0, PPC_BLR,
    };
    put_words(PROC, code, (int)(sizeof code / sizeof code[0]));
    gm_w32(TV_PROC, PROC);
    gm_w32(TV_PROC + 4, 0);
    gm_w32(COUNTER, 0);
}

static void fill(uint32_t seed) {
    for (uint32_t i = 0; i < AREA_LEN; i++)
        gm_w8(AREA + i, (uint8_t)(seed + i * 7));
}

static bool filled(uint32_t seed) {
    for (uint32_t i = 0; i < AREA_LEN; i++)
        if (gm_r8(AREA + i) != (uint8_t)(seed + i * 7))
            return false;
    return true;
}

/* What the entry hook saw. */
static struct {
    int calls;
    snapshot *s;
    uint32_t taken_at; /* COUNTER when it was taken */
    uint32_t string;   /* a CF string made just before */
    bool restored;
    bool differed, same, named_byte, named_register;
    char before[2048], after[2048], byte[2048];
} H;

/* At the second SnapTest, at depth 2 inside the timer's callback: fills
   guest memory and takes a snapshot. At the fourth: changes memory, the
   CPU and module state, which the snapshot is compared with, then puts it
   back in place, which makes the game see the second again. */
static uint32_t hook(uint32_t index) {
    if (trap_depth() != 2 || strcmp(trap_import_name(index), "SnapTest") != 0)
        return index;
    H.calls++;
    uint32_t n = gm_r32(COUNTER);
    if (n == 2 && !H.s) {
        fill(0x11);
        H.string = cf_string("kept");
        H.s = snapshot_take(index);
        H.taken_at = n;
    } else if (n == 4 && !H.restored) {
        H.restored = true;
        fill(0x55);
        cf_string("made after");
        misc_set_system_version(0x1234);
        H.differed = !snapshot_verify(H.s, H.before, sizeof H.before);
        index = snapshot_restore(H.s);
        H.same = snapshot_verify(H.s, H.after, sizeof H.after);
        gm_w8(AREA + 0x1000, gm_r8(AREA + 0x1000) ^ 1);
        H.named_byte = !snapshot_verify(H.s, H.byte, sizeof H.byte) &&
                       strstr(H.byte, "guest memory at 0x") != NULL;
        gm_w8(AREA + 0x1000, gm_r8(AREA + 0x1000) ^ 1);
        uint32_t r5 = cpu_gpr(5);
        cpu_set_gpr(5, r5 + 1);
        char what[256];
        H.named_register = !snapshot_verify(H.s, what, sizeof what) && strstr(what, "the CPU: r5 ");
        cpu_set_gpr(5, r5);
    }
    return index;
}

TEST(snapshot_puts_a_guest_back_in_place_in_the_run_loop) {
    setup();
    emit_proc();
    memset(&H, 0, sizeof H);
    cpu_set_fpr(1, 0.0);
    cpu_set_fpr(2, 0.001);
    uint32_t out = scratch(4);
    uint32_t upp = call_import("NewEventLoopTimerUPP", 1, TV_PROC);
    CHECK_EQ(call_import("InstallEventLoopTimer", 8, call_import("GetMainEventLoop", 0), 0u, 0u, 0u,
                         0u, upp, 0u, out),
             0);
    uint32_t live = cf_live_objects();
    trap_on_entry(hook);
    call_import("RunApplicationEventLoop", 0);
    trap_on_entry(NULL);
    CHECK(H.s != NULL);
    CHECK_EQ(H.taken_at, 2);
    /* Fires 1 to 4, then 3 to 5 again from the snapshot. */
    CHECK_EQ(H.calls, 4 + FIRES - H.taken_at);
    CHECK_EQ(gm_r32(COUNTER), FIRES);
    CHECK(H.differed);
    CHECK_CONTAINS(H.before, "guest memory at 0x");
    CHECK_CONTAINS(H.before, "the CPU: ");
    CHECK_CONTAINS(H.before, "cf.c C");
    CHECK_CONTAINS(H.before, "misc.c system_version at byte 0 of 4");
    CHECK(H.same);
    CHECK_STR(H.after, "");
    CHECK(H.named_byte);
    CHECK(H.named_register);
    CHECK(filled(0x11)); /* the guest wrote nothing there since */
    CHECK_STR(cf_string_text("test", H.string), "kept");
    CHECK_EQ(cf_live_objects(), live + 1); /* "made after" is gone */
    snapshot_free(H.s);
    teardown();
}

/* Every block a pattern can be written into without following a pointer
   that isn't there: each is filled, saved, filled again and loaded, and
   the state is then what was saved. The rest are the ones tested through
   their modules below. A block added to the inventory is covered here
   unless it is added to that list. */
static const char *const owners[] = {"cf.c C", "files.c F", "mixer.c X", "dialogs.c G",
                                     "cgimage.c G", "rsrc.c R"};

static bool is_owner(const char *file, const char *name) {
    char tag[128];
    snprintf(tag, sizeof tag, "%s %s", file, name);
    for (size_t i = 0; i < sizeof owners / sizeof owners[0]; i++)
        if (strcmp(owners[i], tag) == 0)
            return true;
    return false;
}

/* Fills every plain block with pattern a (true) or b. */
static void fill_plain(bool a) {
    int n = snapshot_blocks(&(size_t){0});
    for (int i = 0; i < n; i++) {
        const char *file, *name;
        void *p;
        size_t len;
        snapshot_block(i, &file, &name, &p, &len);
        if (!is_owner(file, name))
            for (size_t k = 0; k < len; k++)
                ((uint8_t *)p)[k] = (uint8_t)(a ? i * 31 + (int)k : 0xA5 ^ (int)k);
    }
}

TEST(snapshot_state_puts_back_every_plain_block) {
    setup();
    int n = snapshot_blocks(&(size_t){0});
    uint8_t *orig[64] = {0};
    CHECK(n <= 64);
    for (int i = 0; i < n; i++) {
        const char *file, *name;
        void *p;
        size_t len;
        snapshot_block(i, &file, &name, &p, &len);
        orig[i] = malloc(len);
        memcpy(orig[i], p, len);
    }
    fill_plain(true);
    size_t alen;
    uint8_t *a = snapshot_state_save(&alen);
    char what[4096], named[4096];
    bool same_at_once = snapshot_state_verify(a, alen, what, sizeof what);
    fill_plain(false);
    bool differed = !snapshot_state_verify(a, alen, named, sizeof named);
    snapshot_state_load(a, alen);
    bool same = snapshot_state_verify(a, alen, what, sizeof what);
    /* Each block has pattern a back, but where it is the host's. */
    int plain = 0, restored = 0;
    for (int i = 0; i < n; i++) {
        const char *file, *name;
        void *p;
        size_t len;
        snapshot_block(i, &file, &name, &p, &len);
        if (!is_owner(file, name)) {
            plain++;
            const uint8_t *b = p;
            size_t k = 0;
            while (k < len && b[k] != (uint8_t)(i * 31 + (int)k))
                k++;
            restored += k < len;
        }
        memcpy(p, orig[i], len); /* before anything runs */
        free(orig[i]);
    }
    free(a);
    CHECK(same_at_once);
    CHECK(plain >= 10);
    CHECK_EQ(restored, plain);
    CHECK(differed);
    CHECK_CONTAINS(named, "misc.c M at byte ");
    CHECK_CONTAINS(named, "trap.c T at byte ");
    CHECK_CONTAINS(named, "memmgr.c last_error at byte ");
    CHECK(same);
    CHECK_STR(what, "");
    teardown();
}

/* PBReadSync of n bytes at the mark into text. */
static int16_t read_at_mark(uint16_t ref, uint32_t n, char *text) {
    uint32_t pb = scratch(80), buf = scratch(16);
    gm_w16(pb + 24, ref);
    gm_w32(pb + 32, buf);
    gm_w32(pb + 36, n);
    gm_w16(pb + 44, 0); /* fsAtMark */
    int16_t err = (int16_t)call_import("PBReadSync", 1, pb);
    memcpy(text, gm_ptr(buf, n), n);
    text[n] = '\0';
    return err;
}

/* The blocks that own heap objects or host handles, through their
   modules: a CF string released, a file read on and closed, a sound
   played to its end and a data provider released, after a save, all come
   back when it is loaded, and are the same bytes saved again. */
TEST(snapshot_state_puts_back_what_modules_own) {
    setup();
    uint32_t kept = cf_string("kept"), spec = scratch(FSSPEC_SIZE), ref = scratch(2), name = scratch(256);
    gm_write_pstr(name, "data.bin");
    CHECK_EQ(call_import("FSMakeFSSpec", 4, 0u, 0u, name, spec), 0);
    CHECK_EQ(call_import("FSpOpenDF", 3, spec, 1u, ref), 0);
    uint16_t file = gm_r16(ref);
    char text[16];
    CHECK_EQ(read_at_mark(file, 3, text), 0);
    mix_open(0);
    for (int i = 0; i < 2; i++) { /* one plays, one waits in the queue */
        mix_sound *snd = mix_sound_new(64, 1, 22050u << 16);
        CHECK(snd != NULL);
        for (int k = 0; k < 64; k++)
            snd->samples[k] = (int16_t)(1000 * (i + 1) + k);
        CHECK(mix_queue(0, (mix_cmd){.kind = MIX_BUFFER, .sound = snd}));
    }
    char png[1100];
    snprintf(png, sizeof png, "%s/art.png", game);
    uint32_t provider = call_import("CGDataProviderCreateWithURL", 1, cf_url(png));
    CHECK(provider != 0);
    size_t alen;
    uint8_t *a = snapshot_state_save(&alen);

    call_import("CFRelease", 1, kept);
    CHECK_EQ(read_at_mark(file, 4, text), 0);
    CHECK_STR(text, "3456");
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)file), 0);
    static int16_t played[2 * 4096], again[2 * 4096];
    mix_render(played, 4096);
    CHECK(!mix_busy(0));
    call_import("CGDataProviderRelease", 1, provider);
    char what[4096];
    CHECK(!snapshot_state_verify(a, alen, what, sizeof what));
    CHECK_CONTAINS(what, "cf.c C");
    CHECK_CONTAINS(what, "files.c F");
    CHECK_CONTAINS(what, "mixer.c X");
    CHECK_CONTAINS(what, "cgimage.c G");

    snapshot_state_load(a, alen);
    CHECK(snapshot_state_verify(a, alen, what, sizeof what));
    CHECK_STR(what, "");
    CHECK_STR(cf_string_text("test", kept), "kept");
    CHECK_EQ(read_at_mark(file, 4, text), 0); /* from the mark saved, 3 */
    CHECK_STR(text, "3456");
    CHECK(mix_busy(0));
    mix_render(again, 4096); /* both sounds, as they played before */
    CHECK(memcmp(played, again, sizeof played) == 0);
    int sounding = 0;
    for (size_t k = 0; k < sizeof again / sizeof again[0]; k++)
        sounding += again[k] != 0;
    CHECK(sounding > 2 * 2 * 64); /* more than the first sound, at twice the rate */
    size_t blen;
    uint8_t *b = snapshot_state_save(&blen);
    CHECK(blen != alen || memcmp(a, b, blen) != 0); /* the mark and the sounds have moved on */
    snapshot_state_load(a, alen); /* and back: the same bytes as saved */
    free(b);
    b = snapshot_state_save(&blen);
    CHECK_EQ(blen, alen);
    CHECK(memcmp(a, b, alen) == 0);
    /* Each owns its copy: releasing them again frees only those. */
    call_import("CFRelease", 1, kept);
    call_import("CGDataProviderRelease", 1, provider);
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)file), 0);
    free(a);
    free(b);
    teardown();
}

/* Loads state with one thing in it broken. */
static uint8_t *broken;
static size_t broken_len;

static void load_broken(void *unused) {
    (void)unused;
    snapshot_state_load(broken, broken_len);
    _exit(0);
}

/* The status of loading state saved here with the 32 bits at offset at
   replaced by v; out gets what it logged. */
static int load_with(size_t at, uint32_t v, char *out, size_t cap) {
    broken = snapshot_state_save(&broken_len);
    memcpy(broken + at, &v, 4);
    int status = test_run_child(load_broken, NULL, out, cap);
    free(broken);
    return status;
}

TEST(snapshot_state_fails_hard_on_a_header_tag_or_length_that_doesnt_match) {
    setup();
    char out[4][4096];
    int status[4];
    status[0] = load_with(8, 0x12345678u, out[0], sizeof out[0]);  /* the layout */
    status[1] = load_with(16, 0x12345678u, out[1], sizeof out[1]); /* the first block's tag */
    status[2] = load_with(20, 1, out[2], sizeof out[2]);           /* its size */
    size_t len;
    uint8_t *state = snapshot_state_save(&len);
    uint64_t body;
    memcpy(&body, state + 24, 8);
    free(state);
    status[3] = load_with(24, (uint32_t)body - 1, out[3], sizeof out[3]); /* its length */
    teardown();
    for (int i = 0; i < 4; i++)
        CHECK(status[i] != 0);
    CHECK_CONTAINS(out[0], "isn't this build's");
    CHECK_CONTAINS(out[1], "has tag 12345678, not this one's");
    CHECK_CONTAINS(out[2], "the snapshot's is 1 bytes, this build's");
    CHECK_CONTAINS(out[3], "loading: ");
}
