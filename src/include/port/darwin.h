/* $PostgreSQL: pgsql/src/include/port/darwin.h,v 1.11 2006/10/04 00:30:09 momjian Exp $ */

#define __darwin__	1

#if HAVE_DECL_F_FULLFSYNC		/* not present before OS X 10.3 */
#define HAVE_FSYNC_WRITETHROUGH

#endif
