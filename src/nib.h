#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Interface Builder nibs, as a Carbon program's main.nib/objects.xib holds
   them: the subset MONSTER FAIR uses. A window is found by its name in the
   nib's nameTable; its controls are the root control's subviews (objects,
   or references to objects defined elsewhere in the file). Only buttons,
   static and edit texts, image views and icons are read; any other control
   is refused by name. Menus are never read. */

#define NIB_MAX_CONTROLS 16

typedef enum {
    NIB_BUTTON,
    NIB_STATIC_TEXT,
    NIB_EDIT_TEXT,
    NIB_IMAGE_VIEW,
    NIB_ICON,
} nib_kind;

typedef struct {
    int top, left, bottom, right;
} nib_rect;

typedef struct {
    nib_kind kind;
    nib_rect bounds;     /* in the window */
    char title[512];     /* buttons and static text, UTF-8; "&#10;" is a line break */
    uint32_t command;    /* buttons: the HICommand ID ('ok  ', 'not!'), 0 if none */
    int button_type;     /* buttons: 1 default, 2 cancel, 0 otherwise */
    uint32_t signature;  /* edit texts and image views: the ControlID */
    int32_t id;
} nib_control;

typedef struct {
    char name[64];       /* its name in the nameTable */
    char title[256];
    nib_rect rect;       /* windowRect: where IB put it on a 1024x768 screen */
    int window_class;    /* carbonWindowClass, 0 if not given */
    int ncontrols;
    nib_control controls[NIB_MAX_CONTROLS];
} nib_window;

/* Reads the window called name from objects.xib's text. False, with err
   saying why, if the XML can't be read, there's no such window, or it holds
   a control this reader doesn't know. */
bool nib_read_window(const char *xml, size_t len, const char *name, nib_window *out, char *err,
                     size_t errlen);
