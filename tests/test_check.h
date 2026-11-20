/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_TEST_CHECK_H
#define OPENST133_TEST_CHECK_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TEST_CHECK(expression)                                                    \
    do {                                                                          \
        if (!(expression)) {                                                       \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                          __FILE__, __LINE__, #expression);                        \
            exit(EXIT_FAILURE);                                                    \
        }                                                                          \
    } while (0)

static inline char *test_mkdtemp(char *directory)
{
#ifdef _WIN32
    char *suffix = strstr(directory, "XXXXXX");
    unsigned int attempt;

    if (suffix == NULL || strcmp(suffix, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }
    for (attempt = 0u; attempt < 1000u; ++attempt) {
        (void)snprintf(suffix, 7u, "%06lu",
            ((unsigned long)_getpid() + (unsigned long)attempt) % 1000000ul);
        if (_mkdir(directory) == 0) {
            return directory;
        }
        if (errno != EEXIST) {
            return NULL;
        }
    }
    errno = EEXIST;
    return NULL;
#else
    return mkdtemp(directory);
#endif
}

static inline int test_setenv(const char *name, const char *value, int overwrite)
{
#ifdef _WIN32
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static inline int test_unsetenv(const char *name)
{
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

#endif
