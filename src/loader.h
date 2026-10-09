#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pef.h"

typedef enum { IMAGE_PEF, IMAGE_MACHO } image_kind;

typedef struct {
    image_kind kind;
    pef_file pef;           /* PEF only */
    uint32_t code_base, code_len; /* for code+0xNNNNN in crash reports */
    uint32_t data_base, data_len;
    uint32_t import_area;   /* PEF: 8 bytes per import */
    uint32_t *import_addr;  /* PEF: what each import resolved to; pef.nimports entries */
    uint32_t main_tvector;  /* PEF: 0 if none */
    uint32_t init_tvector;  /* PEF: 0 if none */

    uint32_t nnames;        /* imports, then (Mach-O) the synthetic ones */
    const char **names;     /* for trap_init; import i traps at GUEST_TRAP_ADDR(i) */
    uint32_t main_addr;     /* Mach-O: main's code address */
    uint32_t ninit;         /* Mach-O: __mod_init_func entries, in order */
    uint32_t *init_addrs;
} loaded_image;

/* Loads a PEF executable into guest memory: places sections, unpacks data,
   binds imports to trap addresses, and relocates. Requires gm_init(). buf
   must outlive img. On failure, writes err and leaves nothing to free. */
bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen);

/* Loads a Mach-O executable (thin, or the PowerPC slice of a fat one) at its
   linked addresses, binds every symbol pointer and external relocation, and
   finds main. Requires gm_init_layout(GM_LAYOUT_MACHO). buf must outlive img.
   On failure, writes err and leaves nothing to free. */
bool image_load_macho(const uint8_t *buf, size_t len, loaded_image *img, char *err,
                      size_t errlen);

void image_free(loaded_image *img);

/* Index of the import called name, or -1. */
int32_t image_find_import(const loaded_image *img, const char *name);

/* What a Mach-O import is, for the loader. Called once per import that a
   non-lazy pointer or an external relocation names (lazy pointers are always
   functions). Returns the guest address of the shim's data object for a data
   symbol, IMAGE_SYMBOL_CODE for a function whose address the program takes,
   or 0 if the shim doesn't know the symbol, which fails the load. */
#define IMAGE_SYMBOL_CODE 1u
typedef uint32_t (*image_data_fn)(const char *name);
void image_set_data_resolver(image_data_fn fn);
