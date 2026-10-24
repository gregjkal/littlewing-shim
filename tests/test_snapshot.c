#include "test.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asm.h"
#include "cf.h"
#include "cgimage.h"
#include "events.h"
#include "files.h"
#include "gamestart.h"
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
    "CFRelease", "CGDataProviderCreateWithURL", "CGDataProviderRelease", "CFStringGetCString",
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
   that isn't there, or failing a check of what its walk loads: each is
   filled, saved, filled again and loaded, and the state is then what was
   saved. The rest are the ones tested through their modules below and in
   the replays that go back. A block added to the inventory is covered
   here unless it is added to that list. */
static const char *const owners[] = {"cf.c C",      "files.c F",  "mixer.c X",  "dialogs.c G",
                                     "cgimage.c G", "rsrc.c R",   "events.c E", "replay.c R",
                                     "misc.c M",    "qd.c Q",     "cgdisplay.c G", "libc.c L",
                                     "coreaudio.c CA", "ae.c A"};

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
    CHECK(plain >= 7);
    CHECK_EQ(restored, plain);
    CHECK(differed);
    CHECK_CONTAINS(named, "play.c P at byte ");
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
    cf_set_bundle(game); /* where a URL can be */
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

/* ---- on disk (#46) ---- */

#define PART (AREA + 0x400) /* a range left out of the encoding, then poked back */
#define PART_LEN 0x100u

/* What the codec's hook saw. */
static struct {
    snapshot *base, *s, *decoded;
    uint8_t *enc;
    size_t enc_len;
    uint32_t string;
    bool done, missing_part, same, fits, late, early;
    bool plain_ok, code_ok, pc_ok; /* snapshot_brings_no_code */
    char code_why[256], pc_why[256];
    int calls;
    char before_poke[2048], after[2048], err[256];
} D;

/* Decodes enc (len bytes) against the emulator as it is and says whether
   it brings no code, and if not why. */
static bool brings_no_code(const uint8_t *enc, size_t len, char *why, size_t cap) {
    snapshot *s = snapshot_decode(enc, len, why, cap);
    bool ok = s && snapshot_brings_no_code(s, why, cap);
    snapshot_free(s);
    return ok;
}

/* At the second SnapTest: the baseline. At the third: fills guest memory,
   makes a CF string and takes the snapshot, encoded against the baseline
   with PART left out. At the fourth: puts the baseline back, decodes the
   encoding against it, pokes PART back and puts the decoded snapshot back
   in place, which must be the third again. */
static uint32_t codec_hook(uint32_t index) {
    if (trap_depth() != 2 || strcmp(trap_import_name(index), "SnapTest") != 0)
        return index;
    D.calls++;
    uint32_t n = gm_r32(COUNTER);
    if (n == 2 && !D.base) {
        fill(0x11);
        gm_write_cstr(AREA + 0x2100, "early@example.com"); /* in a page that changes */
        D.base = snapshot_take(index);
    } else if (n == 3 && !D.s) {
        fill(0x33);
        gm_write_cstr(AREA + 0x1800, "late@example.com");
        D.string = cf_string("made between");
        D.s = snapshot_take(index);
        D.late = snapshot_changed_holds(D.s, D.base, "LATE@example.com");
        D.early = snapshot_changed_holds(D.s, D.base, "EARLY@EXAMPLE.COM");
        snapshot_range part = {PART, PART_LEN};
        D.enc = snapshot_encode(D.s, D.base, &part, 1, &D.enc_len);
    } else if (n == 4 && !D.done) {
        D.done = true;
        snapshot_restore(D.base);
        /* The harness's code section is the first 64 KB of the image,
           where the hook fills memory: the baseline against itself brings
           none, the snapshot changes the code, and a program counter in
           data is refused. */
        size_t plain_len;
        uint8_t *plain = snapshot_encode(D.base, D.base, NULL, 0, &plain_len);
        char why[256];
        D.plain_ok = brings_no_code(plain, plain_len, why, sizeof why);
        D.code_ok = brings_no_code(D.enc, D.enc_len, D.code_why, sizeof D.code_why);
        size_t pc_at = 4 + 4 + 4 + 8 + 8 + 64 * 8; /* the CPU's registers: the pc after r0-31, f0-31 */
        for (int b = 0; b < 4; b++)
            plain[pc_at + (size_t)b] = (uint8_t)(0x00500000u >> (8 * b));
        D.pc_ok = brings_no_code(plain, plain_len, D.pc_why, sizeof D.pc_why);
        free(plain);
        D.decoded = snapshot_decode(D.enc, D.enc_len, D.err, sizeof D.err);
        if (!D.decoded)
            return index;
        D.fits = snapshot_fits(D.decoded);
        index = snapshot_restore(D.decoded);
        D.missing_part = !snapshot_verify(D.s, D.before_poke, sizeof D.before_poke);
        uint8_t bytes[PART_LEN];
        for (uint32_t i = 0; i < PART_LEN; i++)
            bytes[i] = (uint8_t)(0x33 + (PART - AREA + i) * 7);
        snapshot_poke(D.decoded, PART, bytes, PART_LEN);
        index = snapshot_restore(D.decoded);
        D.same = snapshot_verify(D.s, D.after, sizeof D.after);
    }
    return index;
}

/* A snapshot encoded against an earlier one (snapshot_encode) and decoded
   against the emulator put back to that one is the snapshot again, once
   the range left out is poked back; and the encoding holds the pages that
   changed, not the whole mapping. */
TEST(snapshot_encodes_as_the_pages_that_differ_from_a_baseline) {
    setup();
    emit_proc();
    memset(&D, 0, sizeof D);
    cpu_set_fpr(1, 0.0);
    cpu_set_fpr(2, 0.001);
    uint32_t out = scratch(4);
    uint32_t upp = call_import("NewEventLoopTimerUPP", 1, TV_PROC);
    CHECK_EQ(call_import("InstallEventLoopTimer", 8, call_import("GetMainEventLoop", 0), 0u, 0u, 0u,
                         0u, upp, 0u, out),
             0);
    trap_on_entry(codec_hook);
    call_import("RunApplicationEventLoop", 0);
    trap_on_entry(NULL);
    CHECK(D.base && D.s && D.enc);
    CHECK_STR(D.err, "");
    CHECK(D.late);   /* written since the baseline, in any case */
    CHECK(!D.early); /* there at the baseline, unchanged: XORed out of the file */
    CHECK(D.decoded != NULL);
    CHECK(D.plain_ok);
    CHECK(!D.code_ok);
    CHECK_CONTAINS(D.code_why, "it changes the game's code, at 0x");
    CHECK(!D.pc_ok);
    CHECK_STR(D.pc_why, "the CPU's pc is 0x00500000, outside the game's code");
    CHECK(D.fits);
    CHECK(D.missing_part);
    CHECK_CONTAINS(D.before_poke, "guest memory at 0x");
    CHECK(D.same);
    CHECK_STR(D.after, "");
    size_t state_len;
    uint8_t *state = snapshot_state_save(&state_len);
    free(state);
    /* Past the module state, a few pages, of a mapping of megabytes. */
    CHECK(D.enc_len > state_len);
    CHECK(D.enc_len - state_len < 64 * 1024);
    /* Fires 1 to 4, then 4 and 5 again from the third. */
    CHECK_EQ(D.calls, 4 + FIRES - 3);
    CHECK_STR(cf_string_text("test", D.string), "made between");
    /* A damaged encoding is refused, not put back. */
    char err[256] = "";
    D.enc[0] ^= 1;
    CHECK(snapshot_decode(D.enc, D.enc_len, err, sizeof err) == NULL);
    CHECK_CONTAINS(err, "isn't a snapshot");
    D.enc[0] ^= 1;
    CHECK(snapshot_decode(D.enc, D.enc_len - 1, err, sizeof err) == NULL);
    snapshot_free(D.base);
    snapshot_free(D.s);
    snapshot_free(D.decoded);
    free(D.enc);
    teardown();
}

/* Guest memory as a game registered with email leaves it at launch:
   copied into a buffer of the game's (LICENSE_FIELD) by CFStringGetCString,
   and somewhere else in capitals. */
#define LICENSE_FIELD (GUEST_IMAGE_BASE + 0xA000)
#define LICENSE_COPY  (GUEST_IMAGE_BASE + 0xA3FE) /* across a 1 KB line, for the search */
static void registered_as(const char *email) {
    static const char *const keys[] = {"user email"};
    cf_keep_out_of_snapshots(keys, 1);
    char path[1100];
    snprintf(path, sizeof path, "%s/prefs.plist", game);
    FILE *f = fopen(path, "w");
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
               "<key>user email</key><string>%s</string></dict></plist>\n",
            email);
    fclose(f);
    cf_load_prefs(path);
    memset(gm_ptr(LICENSE_FIELD, 0x800), 0, 0x800);
    CHECK_EQ(call_import("CFStringGetCString", 4, cf_string(email), LICENSE_FIELD, 256u, 0u), 1);
    gm_ptr(LICENSE_FIELD, 1)[0] ^= 0x5A; /* changed in place, as Loony decrypts its key code */
    char upper[64];
    for (size_t i = 0; i <= strlen(email); i++)
        upper[i] = (char)toupper((unsigned char)email[i]);
    gm_write_cstr(LICENSE_COPY, upper);
}

/* A baseline's hash is the same under any license (gamestart.h): the
   buffers the license was copied into, and its text anywhere else, are
   blanked. Any other byte counts. */
TEST(snapshot_baseline_hash_leaves_out_the_license) {
    setup();
    uint8_t mine[32], theirs[32], other[32];
    registered_as("me@example.com");
    gamestart_memory_hash(mine);
    registered_as("someone.else@example.org");
    gamestart_memory_hash(theirs);
    gm_ptr(LICENSE_FIELD + 0x400, 1)[0] ^= 1;
    gamestart_memory_hash(other);
    CHECK(memcmp(mine, theirs, sizeof mine) == 0);
    CHECK(memcmp(mine, other, sizeof mine) != 0);
    teardown();
}

/* A string the preferences hold as a license is never in the module
   state (cf_keep_out_of_snapshots): it is walked as which key it is, and
   loading gives it the value this process loaded. */
TEST(snapshot_state_leaves_out_the_license) {
    setup();
    char path[1100];
    snprintf(path, sizeof path, "%s/prefs.plist", game);
    FILE *f = fopen(path, "w");
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
          "<key>user email</key><string>me@example.com</string>"
          "<key>user id</key><string>123456789012</string>"
          "<key>highscore name 1</key><string>GREG</string></dict></plist>\n",
          f);
    fclose(f);
    static const char *const keys[] = {"user email", "user id"};
    cf_keep_out_of_snapshots(keys, 2);
    cf_load_prefs(path);
    CHECK_STR(cf_kept_out(0), "me@example.com");
    CHECK_STR(cf_kept_out(1), "123456789012");
    uint32_t copy = cf_string("me@example.com"); /* as the game copies one */
    size_t len;
    uint8_t *state = snapshot_state_save(&len);
    CHECK(!memmem(state, len, "me@example.com", 14));
    CHECK(!memmem(state, len, "123456789012", 12));
    CHECK(memmem(state, len, "GREG", 4) != NULL);
    call_import("CFRelease", 1, copy);
    snapshot_state_load(state, len);
    char what[1024];
    CHECK(snapshot_state_verify(state, len, what, sizeof what));
    CHECK_STR(cf_string_text("test", copy), "me@example.com");
    CHECK_STR(cf_pref_string("user email"), "me@example.com");
    CHECK_STR(cf_pref_string("user id"), "123456789012");
    free(state);
    teardown();
}
