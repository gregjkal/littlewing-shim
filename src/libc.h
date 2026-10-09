#pragma once
#include <stddef.h>
#include <stdint.h>

/* The part of libSystem a Mach-O game imports: the C library on the shim's
   heap and clock, dlsym for the one symbol the game looks up, and the dyld
   and keymgr calls crt1 would make. */

/* Resets the state and allocates the data objects in the guest heap.
   Requires mm_init(). exe_path is the program's path, for argv. */
void libc_init(const char *exe_path);

/* Registers the handlers. Requires trap_init(). */
void libc_register(void);

/* The loader's resolver for libSystem's data symbols (see image_data_fn):
   errno, _DefaultRuneLocale, __keymgr_global, mach_init_routine and
   _cthread_init_routine. 0 for anything else. */
uint32_t libc_data_symbol(const char *name);

/* Writes main's argv ({exe_path, NULL}), envp ({NULL}) and apple
   ({exe_path, NULL}) into the guest heap. */
void libc_main_args(uint32_t *argv, uint32_t *envp, uint32_t *apple);

/* sprintf's formatting, for the conversions the game uses: %d %i %u %x %X
   %c %s %% with flags, width and precision. Arguments are words, read with
   arg(first), arg(first + 1) and so on. Crashes on any other conversion.
   Returns the length written (out is truncated to cap - 1 bytes). */
size_t libc_format(const char *fmt, uint32_t (*arg)(int n), int first, char *out, size_t cap);
