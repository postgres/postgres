#define USE_POSIX_TIME
#define NO_EMPTY_STMTS
#define SYSV_DIRENT

#define HAS_TEST_AND_SET
#define NEED_I386_TAS_ASM

/***************************************
 * Define this if you are compiling with
 * the native UNIXWARE C compiler.
 ***************************************/
#define USE_UNIVEL_CC

typedef unsigned char slock_t;

#define DISABLE_COMPLEX_MACRO

/***************************************************************
 * strcasecmp() is in c89.a. The following include will get the
 * needed prototype.
 ***************************************************************/
#include <strings.h>

#ifndef			BIG_ENDIAN
#define			BIG_ENDIAN		4321
#endif
#ifndef			LITTLE_ENDIAN
#define			LITTLE_ENDIAN	1234
#endif
#ifndef			PDP_ENDIAN
#define			PDP_ENDIAN		3412
#endif
#ifndef			BYTE_ORDER
#define			BYTE_ORDER		LITTLE_ENDIAN
#endif
