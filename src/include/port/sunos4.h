/*
 * $PostgreSQL: pgsql/src/include/port/sunos4.h,v 1.12 2009/06/11 14:49:11 momjian Exp $
 *
 * sprintf() returns char *, not int, on SunOS 4.1.x */
#define SPRINTF_CHAR

#include <unistd.h>
