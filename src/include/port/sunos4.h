/*
 * src/include/port/sunos4.h
 *
 * sprintf() returns char *, not int, on SunOS 4.1.x */
#define SPRINTF_CHAR

#include <unistd.h>
