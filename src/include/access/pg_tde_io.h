/*-------------------------------------------------------------------------
 *
 * pg_tde_io.h
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
#ifndef PG_TDE_IO_H
#define PG_TDE_IO_H

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
	 * State for bulk extensions. Further pages that were unused at the time
	 * of the extension. They might be in use by the time we use them though,
	 * so rechecks are needed.
	 *
	 * XXX: Eventually these should probably live in RelationData instead,
	 * alongside targetblock.
	 */
	BlockNumber next_free;
	BlockNumber last_free;
} BulkInsertStateData;


extern void pg_tde_RelationPutHeapTuple(Relation relation, Buffer buffer,
								 HeapTuple tuple, bool token);
extern Buffer pg_tde_RelationGetBufferForTuple(Relation relation, Size len,
										Buffer otherBuffer, int options,
										BulkInsertStateData *bistate,
										Buffer *vmbuffer, Buffer *vmbuffer_other,
										int num_pages);

#endif							/* PG_TDE_IO_H */
