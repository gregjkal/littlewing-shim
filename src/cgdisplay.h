#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The CoreGraphics display calls MONSTER FAIR makes to play full screen:
   the emulated screen (qd_screen) is the main display. Switching modes
   resizes it (qd_resize_screen), and CGDisplayBaseAddress and
   CGDisplayBytesPerRow describe its pixels, so a game drawing there draws on
   screen. While the display is captured the screen is presented on every
   pump, since drawing there bypasses QuickDraw. Capturing never makes the
   host window full screen: Cmd-F does, as for the classic games.

   A display mode is an opaque ID (CGDISPLAY_MODE_BASE and up) for a width,
   height and depth. The game never reads one, so modes aren't
   CFDictionaries. */

#define CGDISPLAY_MAIN 0x04272D00u /* CGMainDisplayID's answer */
#define CGDISPLAY_MODE_BASE 0x0C800000u

/* Forgets the modes and releases the display. */
void cgdisplay_init(void);

/* Whether CGDisplayCapture has been called more often than CGDisplayRelease. */
bool cgdisplay_captured(void);

/* Registers CGMainDisplayID, CGDisplayPixelsWide, CGDisplayPixelsHigh,
   CGDisplayBytesPerRow, CGDisplayBaseAddress, CGDisplayCurrentMode,
   CGDisplayBestModeForParameters, CGDisplaySwitchToMode, CGDisplayCapture,
   CGDisplayIsCaptured, CGDisplayRelease, CGDisplayHideCursor and
   CGDisplayShowCursor. */
void cgdisplay_register(void);
