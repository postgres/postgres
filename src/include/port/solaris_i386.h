#define USE_POSIX_TIME
#define NO_EMPTY_STMTS
#define SYSV_DIRENT
#define HAS_TEST_AND_SET
typedef unsigned char slock_t;

#include "sys/isa_defs.h"

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

#ifndef			NAN

#ifndef			__nan_bytes
#define __nan_bytes				{ 0, 0, 0, 0, 0, 0, 0xf8, 0x7f }
#endif	 /* __nan_bytes */

#ifdef			__GNUC__
#define NAN \
  (__extension__ ((union { unsigned char __c[8];					  \
						   double __d; })							  \
				  { __nan_bytes }).__d)

#else							/* Not GCC.  */
#define				   NAN	   (*(__const double *) __nan)
#endif	 /* GCC.  */
#endif	 /* NAN */

#ifndef		   index
#define index  strchr
#endif
