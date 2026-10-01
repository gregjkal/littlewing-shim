#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resource Manager over the application's resource fork, the only resource
   file. Reference: "Inside Macintosh: More Macintosh Toolbox", chapter 1. */

#define RSRC_NOT_FOUND_ERR (-192) /* resNotFound */
#define RSRC_APP_REFNUM    1      /* what CurResFile returns */

typedef struct {
    uint32_t type;
    int16_t id;
    uint8_t attrs;
    char *name;        /* NULL if unnamed */
    uint32_t data_off; /* fork offset of the resource's bytes (after the length word) */
    uint32_t len;
    uint32_t handle;   /* guest handle once loaded, else 0 */
} rsrc_entry;

/* Parses a resource fork. fork must outlive every later rsrc_ call. Replaces
   any fork opened before. On failure writes err and returns false. */
bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen);
void rsrc_close(void);

uint32_t rsrc_type_count(void);
uint32_t rsrc_total(void);
uint32_t rsrc_count(uint32_t type);

/* NULL if absent. */
rsrc_entry *rsrc_find(uint32_t type, int16_t id);
/* Case-insensitive (ASCII) name match. NULL if absent. */
rsrc_entry *rsrc_find_named(uint32_t type, const char *name);
/* The entry whose loaded handle is h, or NULL. */
rsrc_entry *rsrc_find_handle(uint32_t h);
const uint8_t *rsrc_data(const rsrc_entry *e);

/* Registers the Resource Manager imports. Requires rsrc_open() and mm_init(). */
void rsrc_register(void);
