/* $PostgreSQL: pgsql/src/include/port/solaris.h,v 1.12 2004/08/11 17:20:50 tgl Exp $ */

/*
 * Sort this out for all operating systems some time.  The __xxx
 * symbols are defined on both GCC and Solaris CC, although GCC
 * doesn't document them.  The __xxx__ symbols are only on GCC.
 */
#if defined(__i386) && !defined(__i386__)
#define __i386__
#endif

#if defined(__sparc) && !defined(__sparc__)
#define __sparc__
#endif

#if defined(__i386__)
#include <sys/isa_defs.h>
#endif

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
#ifdef __sparc__
#define		  BYTE_ORDER	  BIG_ENDIAN
#endif
#ifdef __i386__
#define		 BYTE_ORDER		 LITTLE_ENDIAN
#endif
#endif

/*
 * Many versions of Solaris have broken strtod() --- see bug #4751182.
 * For the moment we just assume they all do; it's probably not worth
 * the trouble to add a configure test for this.
 */
#define HAVE_BUGGY_SOLARIS_STRTOD
