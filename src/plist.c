#include "plist.h"

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* A malloc'd Mac Roman copy of s; characters Mac Roman lacks become '?'. */
static char *mac_roman(CFStringRef s) {
    CFRange all = CFRangeMake(0, CFStringGetLength(s));
    CFIndex n = 0;
    CFStringGetBytes(s, all, kCFStringEncodingMacRoman, '?', false, NULL, 0, &n);
    char *out = malloc((size_t)n + 1);
    if (!out)
        fatal("out of memory");
    CFStringGetBytes(s, all, kCFStringEncodingMacRoman, '?', false, (UInt8 *)out, n, NULL);
    out[n] = '\0';
    return out;
}

static CFStringRef cf_str(const char *mac) {
    CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)mac, (CFIndex)strlen(mac),
                                            kCFStringEncodingMacRoman, false);
    if (!s)
        fatal("out of memory");
    return s;
}

plist_status plist_read(const char *path, plist_entry **out, uint32_t *n, char *err, size_t errlen) {
    *out = NULL;
    *n = 0;
    err[0] = '\0';
    size_t len;
    uint8_t *buf = read_file(path, &len);
    if (!buf) {
        if (errno == ENOENT || errno == ENOTDIR)
            return PLIST_MISSING;
        snprintf(err, errlen, "can't read %s: %s", path, strerror(errno));
        return PLIST_UNREADABLE;
    }
    CFDataRef data = CFDataCreate(NULL, buf, (CFIndex)len);
    free(buf);
    if (!data)
        fatal("out of memory");
    CFPropertyListRef root = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (!root || CFGetTypeID(root) != CFDictionaryGetTypeID()) {
        if (root)
            CFRelease(root);
        snprintf(err, errlen, "%s is not a property list dictionary", path);
        return PLIST_BAD;
    }
    CFIndex count = CFDictionaryGetCount(root);
    const void **keys = calloc((size_t)count + 1, sizeof *keys);
    const void **values = calloc((size_t)count + 1, sizeof *values);
    plist_entry *e = calloc((size_t)count + 1, sizeof *e);
    if (!keys || !values || !e)
        fatal("out of memory");
    CFDictionaryGetKeysAndValues(root, keys, values);
    uint32_t m = 0;
    for (CFIndex i = 0; i < count; i++) {
        bool is_str = CFGetTypeID(values[i]) == CFStringGetTypeID();
        bool is_num = CFGetTypeID(values[i]) == CFNumberGetTypeID() && !CFNumberIsFloatType(values[i]);
        if (CFGetTypeID(keys[i]) != CFStringGetTypeID() || (!is_str && !is_num)) {
            char *k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? mac_roman(keys[i]) : NULL;
            size_t used = strlen(err);
            snprintf(err + used, errlen - used, "%sskipped \"%s\" (not a string or an integer)",
                     used ? "; " : "", k ? k : "?");
            free(k);
            continue;
        }
        e[m].key = mac_roman(keys[i]);
        e[m].is_number = is_num;
        if (is_num) {
            SInt64 v;
            CFNumberGetValue(values[i], kCFNumberSInt64Type, &v);
            e[m].num = v;
        } else {
            e[m].str = mac_roman(values[i]);
        }
        m++;
    }
    free(keys);
    free(values);
    CFRelease(root);
    *out = e;
    *n = m;
    return PLIST_OK;
}

bool plist_write(const char *path, const plist_entry *e, uint32_t n, char *err, size_t errlen) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    if (!make_dirs(dirname(dir))) {
        snprintf(err, errlen, "can't create the folder for %s: %s", path, strerror(errno));
        return false;
    }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, n, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    for (uint32_t i = 0; i < n; i++) {
        CFStringRef k = cf_str(e[i].key);
        CFTypeRef v = e[i].is_number ? (CFTypeRef)CFNumberCreate(NULL, kCFNumberSInt64Type, &e[i].num)
                                     : (CFTypeRef)cf_str(e[i].str);
        CFDictionarySetValue(d, k, v);
        CFRelease(k);
        CFRelease(v);
    }
    CFDataRef data = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    CFRelease(d);
    if (!data) {
        snprintf(err, errlen, "can't encode %s", path);
        return false;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    size_t len = (size_t)CFDataGetLength(data);
    bool ok = f && fwrite(CFDataGetBytePtr(data), 1, len, f) == len;
    if (f && fclose(f) != 0)
        ok = false;
    CFRelease(data);
    if (!ok || rename(tmp, path) != 0) {
        snprintf(err, errlen, "can't write %s: %s", path, strerror(errno));
        remove(tmp);
        return false;
    }
    return true;
}

void plist_free(plist_entry *e, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        free(e[i].key);
        free(e[i].str);
    }
    free(e);
}
