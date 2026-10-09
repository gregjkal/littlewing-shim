#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*test_fn)(void);

void test_register(const char *name, test_fn fn);
void test_fail(const char *file, int line, const char *msg);
void test_skip(const char *reason);

/* Game folder: $LOONY_GAME_DIR, or /Applications/Loony Labyrinth. */
const char *test_game_dir(void);
const char *test_game_exe_path(void);
bool test_game_present(void);
/* Crystal Caliburn's folder: $LOONY_CC_DIR, or /Applications/Crystal Caliburn. */
const char *test_cc_dir(void);
bool test_cc_present(void);
/* MONSTER FAIR's bundle: $LOONY_MF_APP, or /Applications/MONSTER FAIR.app. */
const char *test_mf_app(void);
const char *test_mf_exe_path(void);
bool test_mf_present(void);
/* Whether to run the slow tests (minutes of play each): $LOONY_SLOW_TESTS
   set to anything but empty or 0. */
bool test_slow_enabled(void);

/* Creates a fresh, empty folder under $TMPDIR and writes its path to out. */
void test_tmp_dir(char *out, size_t cap);

/* Deletes a folder and everything in it. */
void test_remove_tree(const char *path);

/* Runs fn(arg) in a forked child with its stderr captured into out
   (NUL-terminated, truncated to outlen - 1 bytes). Returns the child's exit
   status, or -1 if it was killed by a signal. The child exits 0 if fn returns. */
int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen);

#define TEST(name)                                                              \
    static void name(void);                                                     \
    __attribute__((constructor)) static void name##_register(void) {            \
        test_register(#name, name);                                             \
    }                                                                           \
    static void name(void)

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            test_fail(__FILE__, __LINE__, #cond);                               \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        unsigned long long a_ = (unsigned long long)(a);                        \
        unsigned long long b_ = (unsigned long long)(b);                        \
        if (a_ != b_) {                                                         \
            char m_[256];                                                       \
            snprintf(m_, sizeof m_, "%s == %s (0x%llx != 0x%llx)", #a, #b, a_, b_); \
            test_fail(__FILE__, __LINE__, m_);                                  \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b)                                                         \
    do {                                                                        \
        const char *a_ = (a), *b_ = (b);                                        \
        if (strcmp(a_, b_) != 0) {                                              \
            char m_[512];                                                       \
            snprintf(m_, sizeof m_, "%s == %s (\"%s\" != \"%s\")", #a, #b, a_, b_); \
            test_fail(__FILE__, __LINE__, m_);                                  \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                             \
    do {                                                                        \
        const char *h_ = (hay), *n_ = (needle);                                 \
        if (!strstr(h_, n_)) {                                                  \
            char m_[512];                                                       \
            snprintf(m_, sizeof m_, "output does not contain \"%s\"", n_);      \
            test_fail(__FILE__, __LINE__, m_);                                  \
            fprintf(stderr, "---- output ----\n%s\n----------------\n", h_);    \
            return;                                                             \
        }                                                                       \
    } while (0)

#define SKIP_UNLESS_GAME()                                                      \
    do {                                                                        \
        if (!test_game_present()) {                                             \
            test_skip("game files not found");                                  \
            return;                                                             \
        }                                                                       \
    } while (0)

#define SKIP_UNLESS_CC()                                                        \
    do {                                                                        \
        if (!test_cc_present()) {                                               \
            test_skip("Crystal Caliburn not found");                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define SKIP_UNLESS_MF()                                                        \
    do {                                                                        \
        if (!test_mf_present()) {                                               \
            test_skip("MONSTER FAIR not found");                                \
            return;                                                             \
        }                                                                       \
    } while (0)

#define SKIP_UNLESS_SLOW()                                                      \
    do {                                                                        \
        if (!test_slow_enabled()) {                                             \
            test_skip("slow; set LOONY_SLOW_TESTS=1 to run");                   \
            return;                                                             \
        }                                                                       \
    } while (0)
