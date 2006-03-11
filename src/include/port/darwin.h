/* $PostgreSQL: pgsql/src/include/port/darwin.h,v 1.10 2006/03/11 04:38:38 momjian Exp $ */

#define __darwin__	1

#if HAVE_DECL_F_FULLFSYNC		/* not present before OS X 10.3 */
#define HAVE_FSYNC_WRITETHROUGH
#endif
