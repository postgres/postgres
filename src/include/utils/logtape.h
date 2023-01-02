/*-------------------------------------------------------------------------
 *
 * logtape.h
 *	  Management of "logical tapes" within temporary files.
 *
 * See logtape.c for explanations.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/logtape.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LOGTAPE_H
#define LOGTAPE_H

#include "storage/sharedfileset.h"

/*
 * LogicalTapeSet and LogicalTape are opaque types whose details are not
 * known outside logtape.c.
 */
typedef struct LogicalTapeSet LogicalTapeSet;
typedef struct LogicalTape LogicalTape;


/*
 * The approach tuplesort.c takes to parallel external sorts is that workers,
 * whose state is almost the same as independent serial sorts, are made to
 * produce a final materialized tape of sorted output in all cases.  This is
 * frozen, just like any case requiring a final materialized tape.  However,
 * there is one difference, which is that freezing will also export an
 * underlying shared fileset BufFile for sharing.  Freezing produces TapeShare
 * metadata for the worker when this happens, which is passed along through
 * shared memory to leader.
 *
 * The leader process can then pass an array of TapeShare metadata (one per
 * worker participant) to LogicalTapeSetCreate(), alongside a handle to a
 * shared fileset, which is sufficient to construct a new logical tapeset that
 * consists of each of the tapes materialized by workers.
 *
 * Note that while logtape.c does create an empty leader tape at the end of the
 * tapeset in the leader case, it can never be written to due to a restriction
 * in the shared buffile infrastructure.
 */
typedef struct TapeShare
{
	/*
	 * Currently, all the leader process needs is the location of the
	 * materialized tape's first block.
	 */
	long		firstblocknumber;
} TapeShare;

/*
 * prototypes for functions in logtape.c
 */

extern LogicalTapeSet *LogicalTapeSetCreate(bool preallocate,
											SharedFileSet *fileset, int worker);
extern void LogicalTapeClose(LogicalTape *lt);
extern void LogicalTapeSetClose(LogicalTapeSet *lts);
extern LogicalTape *LogicalTapeCreate(LogicalTapeSet *lts);
extern LogicalTape *LogicalTapeImport(LogicalTapeSet *lts, int worker, TapeShare *shared);
extern void LogicalTapeSetForgetFreeSpace(LogicalTapeSet *lts);
extern size_t LogicalTapeRead(LogicalTape *lt, void *ptr, size_t size);
extern void LogicalTapeWrite(LogicalTape *lt, const void *ptr, size_t size);
extern void LogicalTapeRewindForRead(LogicalTape *lt, size_t buffer_size);
extern void LogicalTapeFreeze(LogicalTape *lt, TapeShare *share);
extern size_t LogicalTapeBackspace(LogicalTape *lt, size_t size);
extern void LogicalTapeSeek(LogicalTape *lt, long blocknum, int offset);
extern void LogicalTapeTell(LogicalTape *lt, long *blocknum, int *offset);
extern long LogicalTapeSetBlocks(LogicalTapeSet *lts);

#endif							/* LOGTAPE_H */
