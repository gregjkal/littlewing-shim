#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Core Foundation subset: CFString and CFNumber objects and CFPreferences.
   Objects live in host memory; the guest sees opaque IDs in tag space
   (CF_TAG_BASE + 16 * index), which it never dereferences. Preferences are
   kept in memory only (saved to disk in a later milestone). */

#define CF_TAG_BASE       0x08000000u
#define CF_TAG_LIMIT      0x09000000u
#define CF_STRING_TYPE_ID 7u
#define CF_NUMBER_TYPE_ID 22u

/* Empties the object table and preferences, then creates the string that
   kCFPreferencesCurrentApplication refers to. */
void cf_init(void);

/* The CFStringRef stored in the kCFPreferencesCurrentApplication data import. */
uint32_t cf_current_app(void);

/* Creates a CFString (retain count 1) from a C string. */
uint32_t cf_string(const char *s);

/* Retain count of a live object, or 0 if ref isn't one. */
int cf_retain_count(uint32_t ref);

/* Number of live objects. */
uint32_t cf_live_objects(void);

/* Registers the CFString, CFNumber, CFRelease, CFGetTypeID and CFPreferences imports. */
void cf_register(void);
