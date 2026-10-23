#include "cf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgimage.h"
#include "files.h"
#include "guest_mem.h"
#include "snapshot.h"
#include "memmgr.h"
#include "plist.h"
#include "trap.h"
#include "util.h"

#define ENC_MAC_ROMAN 0x00000000u
#define ENC_ASCII     0x00000600u
#define ENC_UTF8      0x08000100u

typedef struct {
    uint32_t type_id; /* 0 = free slot */
    int refs;
    char *str;
    int64_t num;
} cf_obj;

typedef struct {
    char *key;
    uint32_t value;
} pref;

static struct {
    cf_obj *objs;
    uint32_t nobjs, cap;
    pref *prefs;
    uint32_t nprefs, prefs_cap;
    uint32_t current_app;
    char *prefs_path; /* NULL: preferences are never saved */
    uint32_t app_var;   /* Mach-O: the kCFPreferencesCurrentApplication variable */
    uint32_t class_ref; /* Mach-O: __CFConstantStringClassReference */
    struct {
        char *text;
        uint32_t addr;
    } *consts; /* constant strings read so far */
    uint32_t nconsts, consts_cap;
    uint32_t main_bundle;
} C;

/* A snapshot's walk (snapshot.h): C with its arrays and every string in
   them, since releasing an object frees its string and a new one can move
   an array. An array's slots past its count are zeros in a snapshot. The
   preferences file's path is the host's (another process, putting back a
   snapshot kept in a recording, saves into its own folder), and so are the
   kept-out values below: a string holding one is walked as which it is. */
static void free_owned(void) {
    for (uint32_t i = 0; i < C.nobjs; i++)
        free(C.objs[i].str);
    for (uint32_t i = 0; i < C.nprefs; i++)
        free(C.prefs[i].key);
    for (uint32_t i = 0; i < C.nconsts; i++)
        free(C.consts[i].text);
    free(C.objs);
    free(C.prefs);
    free(C.consts);
}

/* The preferences whose values a snapshot leaves out
   (cf_keep_out_of_snapshots), and their values as cf_load_prefs read
   them: the host's, kept across cf_init. */
#define MAX_KEPT_OUT 4
#define MAX_KEPT_FIELDS 8
#define MAX_KEPT_FIELD 4096
static struct {
    char *keys[MAX_KEPT_OUT];
    char *values[MAX_KEPT_OUT]; /* NULL: no such string preference */
    size_t n;
    cf_field fields[MAX_KEPT_FIELDS]; /* cf_kept_out_field */
    size_t nfields;
} kept_out;

/* Whether s is one of the values kept out. */
static bool is_kept_out(const char *s) {
    for (size_t k = 0; k < kept_out.n; k++)
        if (kept_out.values[k] && kept_out.values[k][0] && strcmp(s, kept_out.values[k]) == 0)
            return true;
    return false;
}

/* A string the block owns, walked as which kept-out value it is (1 for
   the first, 0 for none) and, if it is none, as itself. Loading one that
   was kept out gives it this process's value. */
static void walk_text(snapshot_walker *w, char **field) {
    uint8_t which = 0;
    if (!snapshot_loading(w) && *field)
        for (size_t k = 0; k < kept_out.n; k++)
            if (kept_out.values[k] && kept_out.values[k][0] && strcmp(*field, kept_out.values[k]) == 0)
                which = (uint8_t)(k + 1);
    snapshot_bytes(w, &which, sizeof which);
    if (!snapshot_loading(w) && which) {
        char *text = *field;
        *field = NULL; /* written as no string */
        snapshot_string(w, field);
        *field = text;
        return;
    }
    snapshot_string(w, field);
    if (snapshot_loading(w) && which) {
        const char *v = which <= kept_out.n && kept_out.values[which - 1] ? kept_out.values[which - 1] : "";
        free(*field);
        if (!(*field = strdup(v)))
            fatal("out of memory");
    }
}

static char *bundle_path; /* cf_set_bundle's; survives cf_init */

static uint32_t ref_of(uint32_t index) { return CF_TAG_BASE + 16u * index; }

bool cf_in_bundle(const char *path) {
    size_t n = bundle_path ? strlen(bundle_path) : 0;
    return path && n && strncmp(path, bundle_path, n) == 0 && (!path[n] || path[n] == '/') &&
           !strstr(path + n, "/../") && !(strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0);
}

static void snap_walk(snapshot_walker *w) {
    if (snapshot_loading(w))
        free_owned();
    snapshot_bytes(w, &C, sizeof C);
    snapshot_check(w, C.nobjs <= C.cap && C.nprefs <= C.prefs_cap && C.nconsts <= C.consts_cap &&
                          ref_of(C.nobjs) <= CF_TAG_LIMIT,
                   "%u objects, %u preferences and %u constants", C.nobjs, C.nprefs, C.nconsts);
    SNAPSHOT_OWNED(w, C.objs, C.cap);
    snapshot_check(w, !C.cap || C.objs, "no objects");
    for (uint32_t i = 0; i < C.cap; i++) {
        if (i < C.nobjs)
            walk_text(w, &C.objs[i].str);
        else
            SNAPSHOT_REBUILT(w, C.objs[i]);
    }
    for (uint32_t i = 0; i < C.nobjs; i++) {
        uint32_t t = C.objs[i].type_id;
        if (t == CF_URL_TYPE_ID || t == CF_BUNDLE_TYPE_ID)
            snapshot_check(w, cf_in_bundle(C.objs[i].str), "object %u's path", i);
        else if (t == CF_STRING_TYPE_ID)
            snapshot_check(w, C.objs[i].str != NULL, "string %u", i);
    }
    SNAPSHOT_OWNED(w, C.prefs, C.prefs_cap);
    snapshot_check(w, !C.prefs_cap || C.prefs, "no preferences");
    for (uint32_t i = 0; i < C.prefs_cap; i++) {
        if (i < C.nprefs) {
            snapshot_string(w, &C.prefs[i].key);
            SNAPSHOT_PADDING(w, C.prefs[i], value);
        } else {
            SNAPSHOT_REBUILT(w, C.prefs[i]);
        }
    }
    for (uint32_t i = 0; i < C.nprefs; i++) {
        uint32_t v = C.prefs[i].value;
        snapshot_check(w, C.prefs[i].key && v >= CF_TAG_BASE && !(v & 15u) && (v - CF_TAG_BASE) / 16u < C.nobjs,
                       "preference %u", i);
    }
    SNAPSHOT_HOST(w, C.prefs_path);
    SNAPSHOT_OWNED(w, C.consts, C.consts_cap);
    snapshot_check(w, !C.consts_cap || C.consts, "no constants");
    for (uint32_t i = 0; i < C.consts_cap; i++) {
        if (i < C.nconsts) {
            snapshot_string(w, &C.consts[i].text);
            SNAPSHOT_PADDING(w, C.consts[i], addr);
            snapshot_check(w, C.consts[i].text != NULL, "constant %u", i);
        } else {
            SNAPSHOT_REBUILT(w, C.consts[i]);
        }
    }
}
SNAPSHOT_STATE_WALK(C, snap_walk)


static cf_obj *lookup(uint32_t ref) {
    if (ref < CF_TAG_BASE || ref >= CF_TAG_LIMIT || (ref & 15u) != 0)
        return NULL;
    uint32_t i = (ref - CF_TAG_BASE) / 16u;
    if (i >= C.nobjs || C.objs[i].type_id == 0)
        return NULL;
    return &C.objs[i];
}

static cf_obj *need(const char *call, uint32_t ref) {
    cf_obj *o = lookup(ref);
    if (!o)
        trap_crash("%s: 0x%08x is not a live CF object", call, ref);
    return o;
}

static uint32_t new_obj(uint32_t type_id, char *str, int64_t num) {
    uint32_t i = 0;
    while (i < C.nobjs && C.objs[i].type_id != 0)
        i++;
    if (i == C.nobjs) {
        if (ref_of(C.nobjs) >= CF_TAG_LIMIT)
            fatal("too many CF objects");
        if (C.nobjs == C.cap) {
            C.cap = C.cap ? C.cap * 2 : 64;
            C.objs = realloc(C.objs, C.cap * sizeof *C.objs);
            if (!C.objs)
                fatal("out of memory");
        }
        C.nobjs++;
    }
    C.objs[i] = (cf_obj){type_id, 1, str, num};
    return ref_of(i);
}

static void release(cf_obj *o) {
    if (--o->refs > 0)
        return;
    free(o->str);
    memset(o, 0, sizeof *o);
}

void cf_init(void) {
    for (uint32_t i = 0; i < C.nobjs; i++)
        free(C.objs[i].str);
    for (uint32_t i = 0; i < C.nprefs; i++)
        free(C.prefs[i].key);
    for (uint32_t i = 0; i < C.nconsts; i++)
        free(C.consts[i].text);
    free(C.objs);
    free(C.prefs);
    free(C.prefs_path);
    free(C.consts);
    memset(&C, 0, sizeof C);
    C.current_app = cf_string("com.littlewing.loonylabyrinth");
}

uint32_t cf_current_app(void) { return C.current_app; }

static uint32_t new_data(uint32_t size) {
    uint32_t p = mm_new_ptr(size, true);
    if (!p)
        fatal("can't allocate Core Foundation's data");
    return p;
}

uint32_t cf_data_symbol(const char *name) {
    if (strcmp(name, "kCFPreferencesCurrentApplication") == 0) {
        if (!C.app_var) {
            C.app_var = new_data(4);
            gm_w32(C.app_var, C.current_app);
        }
        return C.app_var;
    }
    if (strcmp(name, "__CFConstantStringClassReference") == 0) {
        if (!C.class_ref)
            C.class_ref = new_data(16);
        return C.class_ref;
    }
    return 0;
}

uint32_t cf_string(const char *s) {
    char *copy = strdup(s);
    if (!copy)
        fatal("out of memory");
    return new_obj(CF_STRING_TYPE_ID, copy, 0);
}

/* One of the program's constant strings: {isa, flags, bytes, length} in
   guest memory, with isa the class reference the loader bound. */
static bool is_constant_string(uint32_t ref) {
    return C.class_ref && gm_is_backed(ref, 16) && gm_r32(ref) == C.class_ref;
}

/* The flags word's bit for text in UTF-16 (0x7D0, where 8-bit text is
   0x7C8): the compiler's form for a CFSTR with a non-ASCII character, its
   length in UniChars, big-endian like the rest of guest memory. */
#define CONST_UNICODE 0x10u

/* UTF-16 text as Mac Roman, the strings' encoding here, with '?' for a
   character Mac Roman doesn't have. */
static void utf16_to_mac(const uint8_t *src, uint32_t len, char *out) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t c = rd_be16(src + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && (rd_be16(src + 2 * i + 2) & 0xFC00) == 0xDC00)
            i++; /* a surrogate pair: outside Mac Roman */
        int m = c >= 0xD800 && c < 0xE000 ? -1 : files_unicode_to_mac(c);
        out[o++] = m < 0 ? '?' : (char)m;
    }
    out[o] = '\0';
}

static const char *constant_text(uint32_t ref) {
    for (uint32_t i = 0; i < C.nconsts; i++)
        if (C.consts[i].addr == ref)
            return C.consts[i].text;
    uint32_t flags = gm_r32(ref + 4), bytes = gm_r32(ref + 8), len = gm_r32(ref + 12);
    bool unicode = flags & CONST_UNICODE;
    if (unicode && len > UINT32_MAX / 2)
        trap_crash("constant string at 0x%08x: %u UniChars", ref, len);
    const uint8_t *src = gm_ptr(bytes, len ? (unicode ? 2 * len : len) : 1);
    char *text = malloc((size_t)len + 1);
    if (!text)
        fatal("out of memory");
    if (unicode) {
        utf16_to_mac(src, len, text);
    } else {
        memcpy(text, src, len);
        text[len] = '\0';
    }
    if (C.nconsts == C.consts_cap) {
        C.consts_cap = C.consts_cap ? C.consts_cap * 2 : 16;
        C.consts = realloc(C.consts, C.consts_cap * sizeof *C.consts);
        if (!C.consts)
            fatal("out of memory");
    }
    C.consts[C.nconsts].addr = ref;
    C.consts[C.nconsts++].text = text;
    return text;
}

const char *cf_string_text(const char *call, uint32_t ref) {
    if (is_constant_string(ref))
        return constant_text(ref);
    cf_obj *o = need(call, ref);
    if (o->type_id != CF_STRING_TYPE_ID)
        trap_crash("%s: 0x%08x is not a CFString", call, ref);
    return o->str;
}

uint32_t cf_url(const char *path) {
    char *copy = strdup(path);
    if (!copy)
        fatal("out of memory");
    return new_obj(CF_URL_TYPE_ID, copy, 0);
}

const char *cf_url_path(const char *call, uint32_t ref) {
    cf_obj *o = need(call, ref);
    if (o->type_id != CF_URL_TYPE_ID)
        trap_crash("%s: 0x%08x is not a CFURL", call, ref);
    return o->str;
}

void cf_set_bundle(const char *path) {
    free(bundle_path);
    bundle_path = strdup(path);
    if (!bundle_path)
        fatal("out of memory");
}

const char *cf_bundle_path(void) { return bundle_path; }

int cf_retain_count(uint32_t ref) {
    cf_obj *o = lookup(ref);
    return o ? o->refs : 0;
}

uint32_t cf_live_objects(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < C.nobjs; i++)
        n += C.objs[i].type_id != 0;
    return n;
}

/* ---- CFString, CFNumber, CFRelease ---- */

static void need_encoding(const char *call, uint32_t enc) {
    if (enc != ENC_MAC_ROMAN && enc != ENC_ASCII && enc != ENC_UTF8)
        trap_crash("%s: unsupported string encoding 0x%08x", call, enc);
}

/* CFStringCreateWithCString(alloc, cStr, encoding). Bytes are kept as given. */
static void h_string_create_with_cstring(void) {
    uint32_t cstr = trap_arg(1);
    need_encoding("CFStringCreateWithCString", trap_arg(2));
    if (!cstr)
        trap_crash("CFStringCreateWithCString: NULL string");
    char buf[1024];
    if (!gm_read_cstr(cstr, buf, sizeof buf))
        trap_crash("CFStringCreateWithCString: string at 0x%08x is longer than %zu bytes", cstr,
                   sizeof buf - 1);
    trap_return(cf_string(buf));
}

/* CFStringGetCString(str, buffer, bufferSize, encoding) -> Boolean. Writes
   nothing and returns false if the string and its NUL don't fit. */
static void h_string_get_cstring(void) {
    const char *s = cf_string_text("CFStringGetCString", trap_arg(0));
    need_encoding("CFStringGetCString", trap_arg(3));
    int32_t size = (int32_t)trap_arg(2);
    size_t n = strlen(s) + 1;
    if (size < 0 || n > (size_t)size) {
        trap_return(0);
        return;
    }
    memcpy(gm_ptr(trap_arg(1), (uint32_t)n), s, n);
    if (is_kept_out(s)) {
        cf_field f = {trap_arg(1), (uint32_t)size < MAX_KEPT_FIELD ? (uint32_t)size : MAX_KEPT_FIELD};
        size_t i = 0;
        while (i < kept_out.nfields && (kept_out.fields[i].addr != f.addr || kept_out.fields[i].len != f.len))
            i++;
        if (i == kept_out.nfields && i < MAX_KEPT_FIELDS)
            kept_out.fields[kept_out.nfields++] = f;
    }
    trap_return(1);
}

static void h_string_get_type_id(void) { trap_return(CF_STRING_TYPE_ID); }

static void h_get_type_id(void) {
    uint32_t ref = trap_arg(0);
    uint32_t cg = cgimage_type_id(ref);
    trap_return(is_constant_string(ref) ? CF_STRING_TYPE_ID : cg ? cg : need("CFGetTypeID", ref)->type_id);
}

/* CFNumberCreate(alloc, theType, valuePtr): integer types only. */
static void h_number_create(void) {
    uint32_t type = trap_arg(1), p = trap_arg(2);
    int64_t v;
    switch (type) {
    case 1: case 7: /* SInt8, Char */
        v = (int8_t)gm_r8(p);
        break;
    case 2: case 8: /* SInt16, Short */
        v = (int16_t)gm_r16(p);
        break;
    case 3: case 9: case 10: case 14: /* SInt32, Int, Long, CFIndex */
        v = (int32_t)gm_r32(p);
        break;
    case 4: case 11: /* SInt64, LongLong */
        v = (int64_t)(((uint64_t)gm_r32(p) << 32) | gm_r32(p + 4));
        break;
    default:
        trap_crash("CFNumberCreate: unsupported number type %u", type);
    }
    trap_return(new_obj(CF_NUMBER_TYPE_ID, NULL, v));
}

/* A constant string lives as long as the program: releasing it does
   nothing. A CGImage or data provider is cgimage's to release. */
static void h_release(void) {
    uint32_t ref = trap_arg(0);
    if (cgimage_type_id(ref))
        cgimage_release(ref);
    else if (!is_constant_string(ref))
        release(need("CFRelease", ref));
}

/* ---- CFBundle and CFURL: host paths ---- */

static uint32_t main_bundle(void) {
    if (!bundle_path)
        trap_crash("CFBundleGetMainBundle: this game has no bundle");
    if (!C.main_bundle) {
        char *copy = strdup(bundle_path);
        if (!copy)
            fatal("out of memory");
        C.main_bundle = new_obj(CF_BUNDLE_TYPE_ID, copy, 0); /* never released */
    }
    return C.main_bundle;
}

static const char *bundle_of(const char *call, uint32_t ref) {
    cf_obj *o = need(call, ref);
    if (o->type_id != CF_BUNDLE_TYPE_ID)
        trap_crash("%s: 0x%08x is not a CFBundle", call, ref);
    return o->str;
}

/* CFBundleGetMainBundle(): not retained. */
static void h_bundle_get_main_bundle(void) { trap_return(main_bundle()); }

/* CFBundleCopyResourcesDirectoryURL(bundle). Directory URLs end in '/'. */
static void h_bundle_copy_resources_directory_url(void) {
    char path[2048];
    snprintf(path, sizeof path, "%s/Contents/Resources/",
             bundle_of("CFBundleCopyResourcesDirectoryURL", trap_arg(0)));
    trap_return(cf_url(path));
}

/* CFBundleCopyResourceURL(bundle, name, type, subdir): the resource in
   Resources (or Resources/English.lproj), or NULL if there's none. */
static void h_bundle_copy_resource_url(void) {
    const char *call = "CFBundleCopyResourceURL";
    const char *bundle = bundle_of(call, trap_arg(0));
    const char *name = cf_string_text(call, trap_arg(1));
    const char *type = trap_arg(2) ? cf_string_text(call, trap_arg(2)) : "";
    const char *subdir = trap_arg(3) ? cf_string_text(call, trap_arg(3)) : "";
    static const char *const places[] = {"", "English.lproj/"};
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s/Contents/Resources/%s%s%s%s%s%s", bundle, places[i], subdir,
                 *subdir ? "/" : "", name, *type ? "." : "", type);
        if (access(path, F_OK) == 0) {
            trap_return(cf_url(path));
            return;
        }
    }
    log_msg("%s: no resource %s%s%s", call, name, *type ? "." : "", type);
    trap_return(0);
}

/* CFURLCreateWithString(alloc, CFString, baseURL): a URL the game names in
   full, such as its web page. */
static void h_url_create_with_string(void) {
    const char *call = "CFURLCreateWithString";
    if (trap_arg(2))
        trap_crash("%s: a URL relative to a base is not supported", call);
    trap_return(cf_url(cf_string_text(call, trap_arg(1))));
}

/* CFURLCreateCopyAppendingPathComponent(alloc, url, component, isDirectory). */
static void h_url_create_copy_appending_path_component(void) {
    const char *call = "CFURLCreateCopyAppendingPathComponent";
    const char *base = cf_url_path(call, trap_arg(1));
    const char *comp = cf_string_text(call, trap_arg(2));
    size_t n = strlen(base);
    char path[2048];
    snprintf(path, sizeof path, "%s%s%s%s", base, n && base[n - 1] == '/' ? "" : "/", comp,
             trap_arg(3) & 0xFF ? "/" : "");
    trap_return(cf_url(path));
}

/* CFURLGetFileSystemRepresentation(url, resolveAgainstBase, buffer, maxBufLen)
   -> Boolean: the path in UTF-8, false if it and its NUL don't fit. A
   directory's trailing '/' is left off, as CF does. */
static void h_url_get_file_system_representation(void) {
    const char *path = cf_url_path("CFURLGetFileSystemRepresentation", trap_arg(0));
    uint32_t buf = trap_arg(2);
    int32_t max = (int32_t)trap_arg(3);
    size_t n = strlen(path);
    if (n > 1 && path[n - 1] == '/')
        n--;
    if (max < 0 || n + 1 > (size_t)max) {
        trap_return(0);
        return;
    }
    uint8_t *dst = gm_ptr(buf, (uint32_t)n + 1);
    memcpy(dst, path, n);
    dst[n] = '\0';
    trap_return(1);
}

/* ---- CFPreferences ---- */

static void need_current_app(const char *call, uint32_t app) {
    if (app != C.current_app)
        trap_crash("%s: unsupported application ID 0x%08x", call, app);
}

static const char *key_string(const char *call, uint32_t key) {
    return cf_string_text(call, key);
}

static pref *find_pref(const char *key) {
    for (uint32_t i = 0; i < C.nprefs; i++)
        if (strcmp(C.prefs[i].key, key) == 0)
            return &C.prefs[i];
    return NULL;
}

const char *cf_pref_string(const char *key) {
    pref *p = find_pref(key);
    cf_obj *o = p ? lookup(p->value) : NULL;
    return o && o->type_id == CF_STRING_TYPE_ID ? o->str : NULL;
}

bool cf_pref_int(const char *key, int64_t *out) {
    pref *p = find_pref(key);
    cf_obj *o = p ? lookup(p->value) : NULL;
    if (!o || o->type_id != CF_NUMBER_TYPE_ID)
        return false;
    *out = o->num;
    return true;
}

/* Sets key to value, which the preferences now hold a reference to (the
   caller's reference is unchanged). A value of 0 removes the key. */
static void set_pref(const char *key, uint32_t value) {
    if (value)
        lookup(value)->refs++;
    pref *p = find_pref(key);
    if (p) {
        release(&C.objs[(p->value - CF_TAG_BASE) / 16u]);
        if (value) {
            p->value = value;
        } else {
            free(p->key);
            *p = C.prefs[--C.nprefs];
        }
        return;
    }
    if (!value)
        return;
    if (C.nprefs == C.prefs_cap) {
        C.prefs_cap = C.prefs_cap ? C.prefs_cap * 2 : 32;
        C.prefs = realloc(C.prefs, C.prefs_cap * sizeof *C.prefs);
        if (!C.prefs)
            fatal("out of memory");
    }
    char *copy = strdup(key);
    if (!copy)
        fatal("out of memory");
    C.prefs[C.nprefs++] = (pref){copy, value};
}

/* CFPreferencesSetAppValue(key, value, appID). A NULL value removes the
   key. A constant string is stored as a copy, since the preferences hold
   only objects. */
static void h_prefs_set_app_value(void) {
    const char *key = key_string("CFPreferencesSetAppValue", trap_arg(0));
    uint32_t value = trap_arg(1);
    need_current_app("CFPreferencesSetAppValue", trap_arg(2));
    if (value && is_constant_string(value)) {
        uint32_t copy = cf_string(constant_text(value));
        set_pref(key, copy);
        release(lookup(copy));
        return;
    }
    if (value)
        need("CFPreferencesSetAppValue", value);
    set_pref(key, value);
}

/* CFPreferencesCopyAppValue(key, appID): a retained value, or NULL. */
static void h_prefs_copy_app_value(void) {
    const char *key = key_string("CFPreferencesCopyAppValue", trap_arg(0));
    need_current_app("CFPreferencesCopyAppValue", trap_arg(1));
    pref *p = find_pref(key);
    if (!p) {
        trap_return(0);
        return;
    }
    need("CFPreferencesCopyAppValue", p->value)->refs++;
    trap_return(p->value);
}

/* CFPreferencesGetAppIntegerValue(key, appID, Boolean *keyExistsAndHasValidFormat).
   Numbers, and strings that are whole decimal integers, are valid. */
static void h_prefs_get_app_integer_value(void) {
    const char *key = key_string("CFPreferencesGetAppIntegerValue", trap_arg(0));
    need_current_app("CFPreferencesGetAppIntegerValue", trap_arg(1));
    uint32_t valid_out = trap_arg(2);
    pref *p = find_pref(key);
    bool valid = false;
    int64_t v = 0;
    if (p) {
        cf_obj *o = need("CFPreferencesGetAppIntegerValue", p->value);
        if (o->type_id == CF_NUMBER_TYPE_ID) {
            v = o->num;
            valid = true;
        } else {
            char *end;
            errno = 0;
            long long parsed = strtoll(o->str, &end, 10);
            if (*o->str && !*end && errno == 0) {
                v = parsed;
                valid = true;
            }
        }
    }
    if (valid_out)
        gm_w8(valid_out, valid);
    trap_return(valid ? (uint32_t)(int32_t)v : 0);
}

/* CFPreferencesAppSynchronize(appID) -> Boolean: writes the preferences file. */
static void h_prefs_app_synchronize(void) {
    need_current_app("CFPreferencesAppSynchronize", trap_arg(0));
    trap_return(cf_save_prefs());
}

/* ---- the preferences file ---- */

void cf_load_prefs(const char *path) {
    free(C.prefs_path);
    C.prefs_path = strdup(path);
    if (!C.prefs_path)
        fatal("out of memory");
    plist_entry *e;
    uint32_t n;
    char err[512];
    plist_status st = plist_read(path, &e, &n, err, sizeof err);
    if (st == PLIST_UNREADABLE) { /* leave it alone, and don't overwrite it */
        log_msg("preferences: %s; they won't be saved", err);
        free(C.prefs_path);
        C.prefs_path = NULL;
        return;
    }
    if (st == PLIST_BAD) {
        char bad[1100];
        snprintf(bad, sizeof bad, "%s.bad", path);
        rename(path, bad);
        log_msg("preferences: %s; moved it to %s and starting with none", err, bad);
        return;
    }
    if (err[0])
        log_msg("preferences: %s: %s", path, err);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = e[i].is_number ? new_obj(CF_NUMBER_TYPE_ID, NULL, e[i].num) : cf_string(e[i].str);
        set_pref(e[i].key, v);
        release(lookup(v));
        for (size_t k = 0; k < kept_out.n; k++)
            if (!e[i].is_number && strcmp(e[i].key, kept_out.keys[k]) == 0) {
                free(kept_out.values[k]);
                if (!(kept_out.values[k] = strdup(e[i].str)))
                    fatal("out of memory");
            }
    }
    plist_free(e, n);
}

void cf_keep_out_of_snapshots(const char *const *keys, size_t n) {
    if (n > MAX_KEPT_OUT)
        fatal("cf: more than %d preferences kept out of snapshots", MAX_KEPT_OUT);
    for (size_t k = 0; k < kept_out.n; k++) {
        free(kept_out.keys[k]);
        free(kept_out.values[k]);
    }
    memset(&kept_out, 0, sizeof kept_out);
    for (size_t k = 0; k < n; k++)
        if (!(kept_out.keys[k] = strdup(keys[k])))
            fatal("out of memory");
    kept_out.n = n;
}

const char *cf_kept_out(size_t i) { return i < kept_out.n ? kept_out.values[i] : NULL; }

size_t cf_kept_out_count(void) { return kept_out.n; }

size_t cf_kept_out_fields(const cf_field **fields) {
    *fields = kept_out.fields;
    return kept_out.nfields;
}

bool cf_save_prefs(void) {
    if (!C.prefs_path)
        return true;
    plist_entry *e = calloc(C.nprefs + 1, sizeof *e);
    if (!e)
        fatal("out of memory");
    uint32_t n = 0;
    for (uint32_t i = 0; i < C.nprefs; i++) {
        cf_obj *o = lookup(C.prefs[i].value);
        if (!o) { /* the game released a stored value too often; leave the key out */
            log_msg("preferences: \"%s\" no longer has a value; not saving it", C.prefs[i].key);
            continue;
        }
        e[n].key = C.prefs[i].key;
        e[n].is_number = o->type_id == CF_NUMBER_TYPE_ID;
        e[n].num = o->num;
        e[n].str = o->str;
        n++;
    }
    char err[512];
    bool ok = plist_write(C.prefs_path, e, n, err, sizeof err);
    free(e); /* the strings belong to the preferences */
    if (!ok)
        log_msg("preferences: %s", err);
    return ok;
}

void cf_register(void) {
    trap_register("CFStringCreateWithCString", h_string_create_with_cstring);
    trap_register("CFStringGetCString", h_string_get_cstring);
    trap_register("CFStringGetTypeID", h_string_get_type_id);
    trap_register("CFGetTypeID", h_get_type_id);
    trap_register("CFNumberCreate", h_number_create);
    trap_register("CFRelease", h_release);
    trap_register("CFPreferencesSetAppValue", h_prefs_set_app_value);
    trap_register("CFPreferencesCopyAppValue", h_prefs_copy_app_value);
    trap_register("CFPreferencesGetAppIntegerValue", h_prefs_get_app_integer_value);
    trap_register("CFPreferencesAppSynchronize", h_prefs_app_synchronize);
    trap_register("CFBundleGetMainBundle", h_bundle_get_main_bundle);
    trap_register("CFBundleCopyResourcesDirectoryURL", h_bundle_copy_resources_directory_url);
    trap_register("CFBundleCopyResourceURL", h_bundle_copy_resource_url);
    trap_register("CFURLCreateWithString", h_url_create_with_string);
    trap_register("CFURLCreateCopyAppendingPathComponent",
                  h_url_create_copy_appending_path_component);
    trap_register("CFURLGetFileSystemRepresentation", h_url_get_file_system_representation);
}
