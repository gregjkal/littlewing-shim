#pragma once
#include <stddef.h>
#include <stdint.h>

/* Dialog Manager, for now only what startup needs. Alert and StopAlert don't
   draw anything yet (that needs the dialog work in milestone 6): they log the
   alert's text, with ParamText substitutions, and return its default item,
   as if the user pressed Return. */

/* Resets ParamText. */
void dialogs_init(void);

/* The text Alert would show for ALRT id, items joined with " | ". Empty if
   the alert doesn't exist. */
void dialogs_alert_text(int16_t id, char *out, size_t cap);

/* Registers Alert, StopAlert and ParamText. */
void dialogs_register(void);
