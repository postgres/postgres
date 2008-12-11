/*
 * $PostgreSQL: pgsql/src/include/port/sunos4.h,v 1.11 2008/12/11 09:17:07 petere Exp $ 
 *
 * sprintf() returns char *, not int, on SunOS 4.1.x */
#define SPRINTF_CHAR

#include <unistd.h>
