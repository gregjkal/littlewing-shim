#include "cf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
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
} C;

static uint32_t ref_of(uint32_t index) { return CF_TAG_BASE + 16u * index; }

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
    free(C.objs);
    free(C.prefs);
    free(C.prefs_path);
    memset(&C, 0, sizeof C);
    C.current_app = cf_string("com.littlewing.loonylabyrinth");
}

uint32_t cf_current_app(void) { return C.current_app; }

uint32_t cf_string(const char *s) {
    char *copy = strdup(s);
    if (!copy)
        fatal("out of memory");
    return new_obj(CF_STRING_TYPE_ID, copy, 0);
}

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
    cf_obj *o = need("CFStringGetCString", trap_arg(0));
    if (o->type_id != CF_STRING_TYPE_ID)
        trap_crash("CFStringGetCString: 0x%08x is not a CFString", trap_arg(0));
    need_encoding("CFStringGetCString", trap_arg(3));
    int32_t size = (int32_t)trap_arg(2);
    size_t n = strlen(o->str) + 1;
    if (size < 0 || n > (size_t)size) {
        trap_return(0);
        return;
    }
    memcpy(gm_ptr(trap_arg(1), (uint32_t)n), o->str, n);
    trap_return(1);
}

static void h_string_get_type_id(void) { trap_return(CF_STRING_TYPE_ID); }

static void h_get_type_id(void) { trap_return(need("CFGetTypeID", trap_arg(0))->type_id); }

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

static void h_release(void) { release(need("CFRelease", trap_arg(0))); }

/* ---- CFPreferences ---- */

static void need_current_app(const char *call, uint32_t app) {
    if (app != C.current_app)
        trap_crash("%s: unsupported application ID 0x%08x", call, app);
}

static const char *key_string(const char *call, uint32_t key) {
    cf_obj *o = need(call, key);
    if (o->type_id != CF_STRING_TYPE_ID)
        trap_crash("%s: key 0x%08x is not a CFString", call, key);
    return o->str;
}

static pref *find_pref(const char *key) {
    for (uint32_t i = 0; i < C.nprefs; i++)
        if (strcmp(C.prefs[i].key, key) == 0)
            return &C.prefs[i];
    return NULL;
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

/* CFPreferencesSetAppValue(key, value, appID). A NULL value removes the key. */
static void h_prefs_set_app_value(void) {
    const char *key = key_string("CFPreferencesSetAppValue", trap_arg(0));
    uint32_t value = trap_arg(1);
    need_current_app("CFPreferencesSetAppValue", trap_arg(2));
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
    }
    plist_free(e, n);
}

bool cf_save_prefs(void) {
    if (!C.prefs_path)
        return true;
    plist_entry *e = calloc(C.nprefs + 1, sizeof *e);
    if (!e)
        fatal("out of memory");
    for (uint32_t i = 0; i < C.nprefs; i++) {
        cf_obj *o = need("CFPreferencesAppSynchronize", C.prefs[i].value);
        e[i].key = C.prefs[i].key;
        e[i].is_number = o->type_id == CF_NUMBER_TYPE_ID;
        e[i].num = o->num;
        e[i].str = o->str;
    }
    char err[512];
    bool ok = plist_write(C.prefs_path, e, C.nprefs, err, sizeof err);
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
}
