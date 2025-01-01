/*
 * xlogdefs.h
 *
 * Postgres write-ahead log manager record pointer and
 * timeline number definitions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
 */
typedef uint64 XLogRecPtr;

/*
 * Zero is used indicate an invalid pointer. Bootstrap skips the first possible
 * WAL segment, initializing the first WAL page at WAL segment size, so no XLOG
 * record can begin at zero.
 */
#define InvalidXLogRecPtr	0
#define XLogRecPtrIsInvalid(r)	((r) == InvalidXLogRecPtr)

/*
 * First LSN to use for "fake" LSNs.
 *
 * Values smaller than this can be used for special per-AM purposes.
 */
#define FirstNormalUnloggedLSN	((XLogRecPtr) 1000)

/*
 * Handy macro for printing XLogRecPtr in conventional format, e.g.,
 *
 * printf("%X/%X", LSN_FORMAT_ARGS(lsn));
 */
#define LSN_FORMAT_ARGS(lsn) (AssertVariableIsOfTypeMacro((lsn), XLogRecPtr), (uint32) ((lsn) >> 32)), ((uint32) (lsn))

/*
 * XLogSegNo - physical log file sequence number.
 */
typedef uint64 XLogSegNo;

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
 * Replication origin id - this is located in this file to avoid having to
 * include origin.h in a bunch of xlog related places.
 */
typedef uint16 RepOriginId;

/*
 * This chunk of hackery attempts to determine which file sync methods
 * are available on the current platform, and to choose an appropriate
 * default method.
 *
 * Note that we define our own O_DSYNC on Windows, but not O_SYNC.
 */
#if defined(PLATFORM_DEFAULT_WAL_SYNC_METHOD)
#define DEFAULT_WAL_SYNC_METHOD		PLATFORM_DEFAULT_WAL_SYNC_METHOD
#elif defined(O_DSYNC) && (!defined(O_SYNC) || O_DSYNC != O_SYNC)
#define DEFAULT_WAL_SYNC_METHOD		WAL_SYNC_METHOD_OPEN_DSYNC
#else
#define DEFAULT_WAL_SYNC_METHOD		WAL_SYNC_METHOD_FDATASYNC
#endif

#endif							/* XLOG_DEFS_H */
