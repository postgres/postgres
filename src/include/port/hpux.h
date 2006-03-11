/* $PostgreSQL: pgsql/src/include/port/hpux.h,v 1.23 2006/03/11 04:38:38 momjian Exp $ */

#ifndef			BIG_ENDIAN
#define			BIG_ENDIAN		4321
#endif
#ifndef			LITTLE_ENDIAN
#define			LITTLE_ENDIAN	1234
#endif
#ifndef			PDP_ENDIAN
#define			PDP_ENDIAN		3412
#endif

#if defined(__hppa)

#ifndef			BYTE_ORDER
#define			BYTE_ORDER		BIG_ENDIAN
#endif
#elif defined(__ia64)

/* HPUX runs IA64 in big-endian mode */
#ifndef			BYTE_ORDER
#define			BYTE_ORDER		BIG_ENDIAN
#endif
#else
#error unrecognized CPU type for HP-UX

#endif
