#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MISC_GESTALT_UNDEF_SELECTOR_ERR (-5551)
#define MISC_IC_INSTANCE 0x0FFF0001u /* opaque ICInstance returned by ICStart */

/* Resets the clock (TickCount starts at 0), cursor and Apple Event state. */
void misc_init(void);

/* Ticks (1/60 s) since misc_init(). */
uint32_t misc_ticks(void);

/* False while HideCursor has hidden the cursor (until InitCursor). */
bool misc_cursor_visible(void);

/* The handler AEInstallEventHandler recorded for (event class, event ID).
   Returns false if there is none. */
bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
                     uint32_t *refcon);

/* Registers Gestalt, time, string, cursor, Apple Event, Internet Config,
   KeyScript, GetMBarHeight, BlockMoveData and ExitToShell imports. */
void misc_register(void);
