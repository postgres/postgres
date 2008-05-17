/*
 * $PostgreSQL: pgsql/src/include/port/sunos4.h,v 1.10 2008/05/17 01:28:24 adunstan Exp $ 
 *
 * sprintf() returns char *, not int, on SunOS 4.1.x */
#define SPRINTF_CHAR
