/*-------------------------------------------------------------------------
 *
 * hio.h
 *	  POSTGRES heap access method input/output definitions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HIO_H
#define HIO_H

#include "access/htup.h"
#include "storage/buf.h"
#include "utils/relcache.h"

/*
 * state for bulk inserts --- private to heapam.c and hio.c
 *
 * If current_buf isn't InvalidBuffer, then we are holding an extra pin
 * on that buffer.
 *
 * "typedef struct BulkInsertStateData *BulkInsertState" is in heapam.h
 */
typedef struct BulkInsertStateData
{
	BufferAccessStrategy strategy;	/* our BULKWRITE strategy object */
	Buffer		current_buf;	/* current insertion target page */

	/*
	 * State for bulk extensions.
	 *
	 * last_free..next_free are further pages that were unused at the time of
	 * the last extension. They might be in use by the time we use them
	 * though, so rechecks are needed.
	 *
	 * XXX: Eventually these should probably live in RelationData instead,
	 * alongside targetblock.
	 *
	 * already_extended_by is the number of pages that this bulk inserted
	 * extended by. If we already extended by a significant number of pages,
	 * we can be more aggressive about extending going forward.
	 */
	BlockNumber next_free;
	BlockNumber last_free;
	uint32		already_extended_by;
} BulkInsertStateData;


extern void RelationPutHeapTuple(Relation relation, Buffer buffer,
								 HeapTuple tuple, bool token);
extern Buffer RelationGetBufferForTuple(Relation relation, Size len,
										Buffer otherBuffer, int options,
										BulkInsertStateData *bistate,
										Buffer *vmbuffer, Buffer *vmbuffer_other,
										int num_pages);

#endif							/* HIO_H */
