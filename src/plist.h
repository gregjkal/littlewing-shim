#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Property list files holding one dictionary of string and integer values,
   read and written with the host's CoreFoundation. Keys and strings are Mac
   Roman bytes here and Unicode in the file, so the file reads normally in a
   text editor, `plutil` or `defaults`. */

typedef struct {
    char *key;
    bool is_number;
    int64_t num;
    char *str; /* when !is_number */
} plist_entry;

typedef enum { PLIST_OK, PLIST_MISSING, PLIST_UNREADABLE, PLIST_BAD } plist_status;

/* Reads path into a malloc'd array (free with plist_free). PLIST_MISSING if
   the file doesn't exist; PLIST_UNREADABLE, with err set, if it exists but
   can't be read; PLIST_BAD, with err set, if it isn't a property list
   dictionary. Values that aren't strings or integers are skipped and
   named in err (which is "" otherwise). */
plist_status plist_read(const char *path, plist_entry **out, uint32_t *n, char *err, size_t errlen);

/* Writes the entries as an XML property list, replacing path atomically
   (through path.tmp and a rename). Creates missing parent folders. */
bool plist_write(const char *path, const plist_entry *e, uint32_t n, char *err, size_t errlen);

/* plist_read and plist_write on bytes in memory: the property list in buf
   (no PLIST_MISSING or PLIST_UNREADABLE), and a malloc'd XML encoding of
   the entries, or NULL if they can't be encoded. */
plist_status plist_parse(const uint8_t *buf, size_t len, plist_entry **out, uint32_t *n, char *err,
                         size_t errlen);
uint8_t *plist_encode(const plist_entry *e, uint32_t n, size_t *len);

void plist_free(plist_entry *e, uint32_t n);
