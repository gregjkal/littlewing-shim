#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Core Foundation subset: CFString and CFNumber objects and CFPreferences.
   Objects live in host memory; the guest sees opaque IDs in tag space
   (CF_TAG_BASE + 16 * index), which it never dereferences. Preferences live
   in memory; with a preferences file (cf_load_prefs) they are read from it
   at startup and written back by CFPreferencesAppSynchronize, which the game
   calls as it quits. main also saves them at any exit but a crash, as macOS
   keeps values an application set without synchronizing. */

#define CF_TAG_BASE       0x08000000u
#define CF_TAG_LIMIT      0x09000000u
#define CF_STRING_TYPE_ID 7u
#define CF_NUMBER_TYPE_ID 22u
#define CF_URL_TYPE_ID    29u /* the shim's own numbers from here on */
#define CF_BUNDLE_TYPE_ID 31u

/* Empties the object table and preferences, then creates the string that
   kCFPreferencesCurrentApplication refers to. */
void cf_init(void);

/* Loads the preferences in path (a property list of strings and integers)
   and remembers path for CFPreferencesAppSynchronize. A missing file means
   no preferences yet. A file that isn't a property list is logged and
   renamed to path.bad, so the next synchronize doesn't destroy it, and the
   game starts with no preferences. A file that can't be read (permissions,
   an I/O error) is left alone and the preferences aren't saved. */
void cf_load_prefs(const char *path);

/* Writes the preferences to the file named by cf_load_prefs. False (logged)
   if that fails. True, doing nothing, if there's no file. A key whose value
   is no longer a live object is left out (logged). */
bool cf_save_prefs(void);

/* The CFStringRef stored in the kCFPreferencesCurrentApplication data import. */
uint32_t cf_current_app(void);

/* The loader's resolver for a Mach-O game's Core Foundation data (see
   image_data_fn): kCFPreferencesCurrentApplication, a word holding
   cf_current_app(), and __CFConstantStringClassReference, the isa of the
   program's constant strings. Allocated on first use (requires mm_init());
   0 for anything else. */
uint32_t cf_data_symbol(const char *name);

/* Creates a CFString (retain count 1) from a C string. */
uint32_t cf_string(const char *s);

/* The text of a CFString: a live CFString object, or one of the program's
   constant strings (a guest struct whose isa is
   __CFConstantStringClassReference, then flags, a pointer to the bytes and
   a length). Crashes, naming call, for anything else. The text stays valid
   until cf_init(). */
const char *cf_string_text(const char *call, uint32_t ref);

/* A Mach-O game's bundle: CFBundleGetMainBundle's, whose resources are in
   <path>/Contents/Resources. Kept across cf_init(). */
void cf_set_bundle(const char *path);

/* The path cf_set_bundle was given, or NULL. */
const char *cf_bundle_path(void);

/* Creates a CFURL (retain count 1) for a host path. */
uint32_t cf_url(const char *path);

/* The host path a CFURL names. Crashes, naming call, if ref isn't a CFURL. */
const char *cf_url_path(const char *call, uint32_t ref);

/* Retain count of a live object, or 0 if ref isn't one. */
int cf_retain_count(uint32_t ref);

/* Number of live objects. */
uint32_t cf_live_objects(void);

/* Registers the CFString, CFNumber, CFRelease, CFGetTypeID, CFPreferences,
   CFBundle and CFURL imports. */
void cf_register(void);
