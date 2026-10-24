#pragma once
#include <stdint.h>

/* The Apple Event Manager's descriptors, and the Object Support Library
   calls PowerPlant makes to set itself up for AppleScript. Reference:
   "Inside Macintosh: Interapplication Communication", chapters 4 to 6.

   An AEDesc in guest memory is its two words, descriptorType and
   dataHandle. dataHandle is never a guest handle: it is 0 for no data, or
   an opaque ID (AE_TAG_BASE + 1 + index) naming a descriptor the shim
   keeps: its bytes, or, for a list, a record or an Apple Event, its items
   (each a keyword and a descriptor of its own), and an Apple Event's
   attributes. A descriptor owns its items: AEDisposeDesc frees them all,
   and every call that hands one back (AEGetParamDesc, AEGetNthDesc,
   AEDuplicateDesc) hands back a copy the game must dispose of.

   Coercion knows only the identity, typeWildCard, the 4-byte types among
   'type', 'enum', 'keyw' and 'long', and 'shor' to and from 'long'.

   AESend delivers an event to this program, the only process there is: to
   the handler AEInstallEventHandler recorded for its class and ID
   (misc.h), with a reply Apple Event when one is asked for. AEResolve
   finds no object (errAENoSuchObject); the object accessors and callbacks
   are accepted and never called. */

#define AE_TAG_BASE 0x09000000u
#define AE_TAG_LIMIT 0x0A000000u

#define AE_TYPE_NULL 0x6E756C6Cu        /* 'null' */
#define AE_TYPE_APPLE_EVENT 0x61657674u /* 'aevt' */

/* For the shim's own events (events.c): an Apple Event of the class and
   ID, with the attributes AECreateAppleEvent gives one (its class, ID,
   return ID, transaction ID and this process's address), written as an
   AEDesc at desc. */
void ae_new_apple_event(uint32_t desc, uint32_t event_class, uint32_t event_id);

/* A null descriptor at desc. */
void ae_new_null(uint32_t desc);

/* AEDisposeDesc's work: frees what the AEDesc at desc holds and makes it
   null. */
void ae_dispose(uint32_t desc);

/* Registers AECreateDesc, AEDisposeDesc, AEDuplicateDesc, AECoerceDesc,
   AEGetDescDataSize, AEGetDescData, AECreateList, AECountItems, AEPutDesc,
   AEGetNthDesc, AECreateAppleEvent, AEPutParamDesc, AEPutParamPtr,
   AEGetParamDesc, AEGetParamPtr, AEGetAttributePtr, AESend,
   AEManagerInfo, AEObjectInit, AESetObjectCallbacks,
   AEInstallObjectAccessor, AEResolve, CreateObjSpecifier,
   CreateOffsetDescriptor and the NewOSL...UPP calls. Requires mm_init(). */
void ae_register(void);
