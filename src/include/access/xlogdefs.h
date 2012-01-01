/*
 * xlogdefs.h
 *
 * Postgres transaction log manager record pointer and
 * timeline number definitions
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogdefs.h
 */
#ifndef XLOG_DEFS_H
#define XLOG_DEFS_H

#include <fcntl.h>				/* need open() flags */

/*
 * Pointer to a location in the XLOG.  These pointers are 64 bits wide,
 * because we don't want them ever to overflow.
 *
 * NOTE: xrecoff == 0 is used to indicate an invalid pointer.  This is OK
 * because we use page headers in the XLOG, so no XLOG record can start
 * right at the beginning of a file.
 *
 * NOTE: the "log file number" is somewhat misnamed, since the actual files
 * making up the XLOG are much smaller than 4Gb.  Each actual file is an
 * XLogSegSize-byte "segment" of a logical log file having the indicated
 * xlogid.	The log file number and segment number together identify a
 * physical XLOG file.	Segment number and offset within the physical file
 * are computed from xrecoff div and mod XLogSegSize.
 */
typedef struct XLogRecPtr
{
	uint32		xlogid;			/* log file #, 0 based */
	uint32		xrecoff;		/* byte offset of location in log file */
} XLogRecPtr;

#define XLogRecPtrIsInvalid(r)	((r).xrecoff == 0)


/*
 * Macros for comparing XLogRecPtrs
 *
 * Beware of passing expressions with side-effects to these macros,
 * since the arguments may be evaluated multiple times.
 */
#define XLByteLT(a, b)		\
			((a).xlogid < (b).xlogid || \
			 ((a).xlogid == (b).xlogid && (a).xrecoff < (b).xrecoff))

#define XLByteLE(a, b)		\
			((a).xlogid < (b).xlogid || \
			 ((a).xlogid == (b).xlogid && (a).xrecoff <= (b).xrecoff))

#define XLByteEQ(a, b)		\
			((a).xlogid == (b).xlogid && (a).xrecoff == (b).xrecoff)


/*
 * Macro for advancing a record pointer by the specified number of bytes.
 */
#define XLByteAdvance(recptr, nbytes)						\
	do {													\
		if (recptr.xrecoff + nbytes >= XLogFileSize)		\
		{													\
			recptr.xlogid += 1;								\
			recptr.xrecoff									\
				= recptr.xrecoff + nbytes - XLogFileSize;	\
		}													\
		else												\
			recptr.xrecoff += nbytes;						\
	} while (0)


/*
 * TimeLineID (TLI) - identifies different database histories to prevent
 * confusion after restoring a prior state of a database installation.
 * TLI does not change in a normal stop/restart of the database (including
 * crash-and-recover cases); but we must assign a new TLI after doing
 * a recovery to a prior state, a/k/a point-in-time recovery.  This makes
 * the new WAL logfile sequence we generate distinguishable from the
 * sequence that was generated in the previous incarnation.
 */
typedef uint32 TimeLineID;

/*
 *	Because O_DIRECT bypasses the kernel buffers, and because we never
 *	read those buffers except during crash recovery or if wal_level != minimal,
 *	it is a win to use it in all cases where we sync on each write().  We could
 *	allow O_DIRECT with fsync(), but it is unclear if fsync() could process
 *	writes not buffered in the kernel.	Also, O_DIRECT is never enough to force
 *	data to the drives, it merely tries to bypass the kernel cache, so we still
 *	need O_SYNC/O_DSYNC.
 */
#ifdef O_DIRECT
#define PG_O_DIRECT				O_DIRECT
#else
#define PG_O_DIRECT				0
#endif

/*
 * This chunk of hackery attempts to determine which file sync methods
 * are available on the current platform, and to choose an appropriate
 * default method.	We assume that fsync() is always available, and that
 * configure determined whether fdatasync() is.
 */
#if defined(O_SYNC)
#define OPEN_SYNC_FLAG		O_SYNC
#elif defined(O_FSYNC)
#define OPEN_SYNC_FLAG		O_FSYNC
#endif

#if defined(O_DSYNC)
#if defined(OPEN_SYNC_FLAG)
/* O_DSYNC is distinct? */
#if O_DSYNC != OPEN_SYNC_FLAG
#define OPEN_DATASYNC_FLAG		O_DSYNC
#endif
#else							/* !defined(OPEN_SYNC_FLAG) */
/* Win32 only has O_DSYNC */
#define OPEN_DATASYNC_FLAG		O_DSYNC
#endif
#endif

#if defined(PLATFORM_DEFAULT_SYNC_METHOD)
#define DEFAULT_SYNC_METHOD		PLATFORM_DEFAULT_SYNC_METHOD
#elif defined(OPEN_DATASYNC_FLAG)
#define DEFAULT_SYNC_METHOD		SYNC_METHOD_OPEN_DSYNC
#elif defined(HAVE_FDATASYNC)
#define DEFAULT_SYNC_METHOD		SYNC_METHOD_FDATASYNC
#else
#define DEFAULT_SYNC_METHOD		SYNC_METHOD_FSYNC
#endif

/*
 * Limitation of buffer-alignment for direct IO depends on OS and filesystem,
 * but XLOG_BLCKSZ is assumed to be enough for it.
 */
#ifdef O_DIRECT
#define ALIGNOF_XLOG_BUFFER		XLOG_BLCKSZ
#else
#define ALIGNOF_XLOG_BUFFER		ALIGNOF_BUFFER
#endif

#endif   /* XLOG_DEFS_H */
