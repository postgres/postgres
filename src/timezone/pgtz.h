#include "postgres.h"

#define NOID
#define HAVE_SYMLINK 0
#define HAVE_SYS_WAIT_H 0
#define TZDIR pgwin32_TZDIR()

char *pgwin32_TZDIR(void);
