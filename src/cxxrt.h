#pragma once
#include <stddef.h>
#include <stdint.h>

/* The part of libstdc++ and libgcc_s a Mach-O game imports: new and delete
   on the shim's heap, static-local guards, and exceptions, which crash
   naming the thrown type (the game is not expected to throw while it runs). */

/* Resets the state and allocates the type-info vtables in the guest heap.
   Requires mm_init(). */
void cxxrt_init(void);

/* Registers the handlers. Requires trap_init(). */
void cxxrt_register(void);

/* The loader's resolver for the C++ runtime (see image_data_fn): the three
   type-info vtables are data; __cxa_pure_virtual and __gxx_personality_v0
   are code. 0 for anything else. */
uint32_t cxxrt_data_symbol(const char *name);

/* Writes a readable form of a type_info's mangled name ("N2RT12TOSExceptionE"
   reads "RT::TOSException"); anything it can't read is copied as it is. */
void cxxrt_type_name(const char *mangled, char *out, size_t cap);
