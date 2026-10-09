#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Dialog Manager: Alert, StopAlert and ParamText; GetNewDialog,
   ModalDialog, GetDialogItem, GetDialogItemText and DisposeDialog.

   Dialogs are drawn straight onto the emulated screen from their ALRT, DLOG
   and DITL resources, plainly and with the 8x8 font (font.h): a framed white
   box, buttons, static text with ParamText substitutions, edit fields,
   pictures and icons. What was under a dialog comes back when it closes.
   While one is open it takes the input (events_set_modal): clicks on
   buttons, Return or Enter for the default button, Esc or Cmd-. for a
   button titled "Cancel", typing, Delete, Tab and Shift-Tab between edit
   fields, and Cmd-V. No game timers fire meanwhile.

   Nib windows (a Mac OS X game's): CreateNibReference, CreateWindowFromNib
   and DisposeNibReference read the bundle's main.nib (nib.h). A nib window
   is a window port (qd_new_window), drawn like a DLOG while it's shown, and
   RunAppModalLoopForWindow takes the input until QuitAppModalLoopForWindow.
   Clicking a button (or Return for the one whose command is 'ok  ', Esc for
   'not!') sends kEventCommandProcess with its command to the window's
   handlers (events_send_command). HIViewGetRoot, HIViewFindByID and
   GetControlByID find controls by ControlID; HIViewSetVisible,
   GetControlData (an edit text's text) and the HIImageView calls (the
   image view draws a CGImage, cgimage.h) work on them.
   CreateStandardAlert and RunStandardAlert draw the error and explanation
   with an OK button.

   With LOONY_AUTO_ALERTS=1, Alert, StopAlert and RunStandardAlert draw
   nothing and answer at once with their default item, as if the user
   pressed Return (headless runs whose golden frames predate dialogs use
   this), and RunAppModalLoopForWindow presses the default button. */

/* DialogRefs are opaque IDs: DLG_TAG_BASE + 16 * slot. */
#define DLG_TAG_BASE 0x0B000000u
#define DLG_MAX 8

/* DITL item types (the low 7 bits) and the disabled flag. */
#define DLG_ITEM_USER 0
#define DLG_ITEM_BUTTON 4
#define DLG_ITEM_CHECKBOX 5
#define DLG_ITEM_RADIO 6
#define DLG_ITEM_STATIC_TEXT 8
#define DLG_ITEM_EDIT_TEXT 16
#define DLG_ITEM_ICON 32
#define DLG_ITEM_PICTURE 64
#define DLG_ITEM_DISABLED 128

/* Resets ParamText and closes all dialogs and nibs (without restoring the
   screen). Reads LOONY_AUTO_ALERTS. Installs the QuickDraw window hook, so
   it runs after qd_init. */
void dialogs_init(void);

/* The text Alert would show for ALRT id, items joined with " | ". Empty if
   the alert doesn't exist. */
void dialogs_alert_text(int16_t id, char *out, size_t cap);

/* Registers the Dialog Manager imports. */
void dialogs_register(void);
