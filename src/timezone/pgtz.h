#include "postgres.h"
#include "miscadmin.h"

#ifndef HAVE_SYMLINK
#define HAVE_SYMLINK 0
#endif


#define NOID
#define TZDIR pg_TZDIR()

char	   *pg_TZDIR(void);
