#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resource Manager. Reference: "Inside Macintosh: More Macintosh Toolbox",
   chapter 1.

   The application's resource file is always open: a classic game's
   resource fork, or for a Mach-O bundle its Contents/Resources/<program>.rsrc
   (a resource file in a data fork, as Carbon opens it), or nothing. The
   game opens more with FSpOpenResFile, each a file's resource fork through
   the File Manager's overlay (files.h), and each becomes the current file.
   The files form a chain in the order they were opened, the application's
   at the bottom: GetResource and the shim's own lookups (rsrc_find) search
   from the current file down, Get1Resource the current file alone.
   Refnum 0 is the System file, which holds nothing.

   Changes (AddResource, ChangedResource, SetResInfo, SetResAttrs) stay in
   memory until WriteResource, UpdateResFile or CloseResFile writes the
   file, or rsrc_update_all at exit: the whole fork is written again, into
   the data folder, so a file in the game folder is never changed. The
   application's file is never written; a game that tries crashes, naming
   the call.

   A snapshot holds which handle each resource is loaded in and the current
   file. The files opened and their contents are the host's: putting back a
   snapshot taken with other files open, or another count of resources,
   fails hard. Only Loony Labyrinth seeks, and it opens no resource file. */

#define RSRC_NOT_FOUND_ERR (-192) /* resNotFound */
#define RSRC_FILE_NOT_FOUND_ERR (-193) /* resFNotFound: no such open resource file */
#define RSRC_ADD_FAILED_ERR (-194) /* addResFailed */
#define RSRC_ATTR_ERR      (-198) /* resAttrErr */
#define RSRC_MAP_READ_ERR  (-199) /* mapReadErr */
#define RSRC_APP_REFNUM    1      /* the application's file, what CurResFile returns at first */
#define RSRC_SYSTEM_REFNUM 0
#define RSRC_MAX_FILES     8      /* the application's and seven more */

/* Resource attributes. */
#define RSRC_ATTR_PURGEABLE 0x20
#define RSRC_ATTR_CHANGED   0x02

typedef struct {
    uint32_t type;
    int16_t id;
    uint8_t attrs;
    char *name;        /* NULL if unnamed */
    uint32_t data_off; /* fork offset of the resource's bytes (after the length word) */
    uint32_t len;      /* 0 for one added since the file was last written */
    uint32_t handle;   /* guest handle once loaded, else 0 */
    uint8_t file;      /* which open file it is in (an index, rsrc.c's) */
    bool added;        /* added since the file was last written: its bytes are its handle's */
} rsrc_entry;

/* Parses the application's resource file, closing every file opened
   before. fork must outlive every later rsrc_ call. On failure writes err
   and returns false. */
bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen);
void rsrc_close(void);

/* For a program with no resource file: the application's file is empty,
   so every lookup in it fails as if the resource were absent. */
void rsrc_open_empty(void);

/* The application's file: its types, its resources, and entry i of them,
   0 to rsrc_total() - 1, in the fork's order. */
uint32_t rsrc_type_count(void);
uint32_t rsrc_total(void);
rsrc_entry *rsrc_at(uint32_t i);

/* The resources of type in every open file. */
uint32_t rsrc_count(uint32_t type);

/* Searching the chain from the current file down. NULL if absent. */
rsrc_entry *rsrc_find(uint32_t type, int16_t id);
/* Case-insensitive (ASCII) name match. NULL if absent. */
rsrc_entry *rsrc_find_named(uint32_t type, const char *name);
/* The entry, in any open file, whose loaded handle is h, or NULL. */
rsrc_entry *rsrc_find_handle(uint32_t h);
/* e's bytes as they are in its file, e->len of them; for an added
   resource, its handle's (mm_handle_size of them). */
const uint8_t *rsrc_data(const rsrc_entry *e);

/* Writes every open file that has changes. For exit. */
void rsrc_update_all(void);

/* A resource file holding entries (n of them, each with its type, id,
   attrs, name and len, and its bytes at data[i]), as a Mac writes one:
   malloc'd, *len bytes. For writing a file, and for tests. */
uint8_t *rsrc_build(const rsrc_entry *entries, const uint8_t *const *data, uint32_t n, size_t *len);

/* Registers the Resource Manager imports. Requires rsrc_open() and mm_init(). */
void rsrc_register(void);
