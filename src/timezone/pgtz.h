#include "postgres.h"

#ifndef HAVE_SYMLINK
#define HAVE_SYMLINK 0
#endif

#define NOID
#define TZDIR pgwin32_TZDIR()

char *pgwin32_TZDIR(void);
