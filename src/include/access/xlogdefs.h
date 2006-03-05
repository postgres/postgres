/*
 * xlogdefs.h
 *
 * Postgres transaction log manager record pointer and
 * timeline number definitions
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xlogdefs.h,v 1.15 2006/03/05 15:58:54 momjian Exp $
 */
#ifndef XLOG_DEFS_H
#define XLOG_DEFS_H

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
 * TimeLineID (TLI) - identifies different database histories to prevent
 * confusion after restoring a prior state of a database installation.
 * TLI does not change in a normal stop/restart of the database (including
 * crash-and-recover cases); but we must assign a new TLI after doing
 * a recovery to a prior state, a/k/a point-in-time recovery.  This makes
 * the new WAL logfile sequence we generate distinguishable from the
 * sequence that was generated in the previous incarnation.
 */
typedef uint32 TimeLineID;

#endif   /* XLOG_DEFS_H */
