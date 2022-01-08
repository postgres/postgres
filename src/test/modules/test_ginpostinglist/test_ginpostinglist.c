/*--------------------------------------------------------------------------
 *
 * test_ginpostinglist.c
 *		Test varbyte-encoding in ginpostinglist.c
 *
 * Copyright (c) 2019-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_ginpostinglist/test_ginpostinglist.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginblock.h"
#include "access/htup_details.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_ginpostinglist);

/*
 * Encodes a pair of TIDs, and decodes it back. The first TID is always
 * (0, 1), the second one is formed from the blk/off arguments. The 'maxsize'
 * argument is passed to ginCompressPostingList(); it can be used to test the
 * overflow checks.
 *
 * The reason that we test a pair, instead of just a single TID, is that
 * the GinPostingList stores the first TID as is, and the varbyte-encoding
 * is only used for the deltas between TIDs. So testing a single TID would
 * not exercise the varbyte encoding at all.
 *
 * This function prints NOTICEs to describe what is tested, and how large the
 * resulting GinPostingList is. Any incorrect results, e.g. if the encode +
 * decode round trip doesn't return the original input, are reported as
 * ERRORs.
 */
static void
test_itemptr_pair(BlockNumber blk, OffsetNumber off, int maxsize)
{
	ItemPointerData orig_itemptrs[2];
	ItemPointer decoded_itemptrs;
	GinPostingList *pl;
	int			nwritten;
	int			ndecoded;

	elog(NOTICE, "testing with (%u, %d), (%u, %d), max %d bytes",
		 0, 1, blk, off, maxsize);
	ItemPointerSet(&orig_itemptrs[0], 0, 1);
	ItemPointerSet(&orig_itemptrs[1], blk, off);

	/* Encode, and decode it back */
	pl = ginCompressPostingList(orig_itemptrs, 2, maxsize, &nwritten);
	elog(NOTICE, "encoded %d item pointers to %zu bytes",
		 nwritten, SizeOfGinPostingList(pl));

	if (SizeOfGinPostingList(pl) > maxsize)
		elog(ERROR, "overflow: result was %zu bytes, max %d",
			 SizeOfGinPostingList(pl), maxsize);

	decoded_itemptrs = ginPostingListDecode(pl, &ndecoded);
	if (nwritten != ndecoded)
		elog(NOTICE, "encoded %d itemptrs, %d came back", nwritten, ndecoded);

	/* Check the result */
	if (!ItemPointerEquals(&orig_itemptrs[0], &decoded_itemptrs[0]))
		elog(ERROR, "mismatch on first itemptr: (%u, %d) vs (%u, %d)",
			 0, 1,
			 ItemPointerGetBlockNumber(&decoded_itemptrs[0]),
			 ItemPointerGetOffsetNumber(&decoded_itemptrs[0]));

	if (ndecoded == 2 &&
		!ItemPointerEquals(&orig_itemptrs[0], &decoded_itemptrs[0]))
	{
		elog(ERROR, "mismatch on second itemptr: (%u, %d) vs (%u, %d)",
			 0, 1,
			 ItemPointerGetBlockNumber(&decoded_itemptrs[0]),
			 ItemPointerGetOffsetNumber(&decoded_itemptrs[0]));
	}
}

/*
 * SQL-callable entry point to perform all tests.
 */
Datum
test_ginpostinglist(PG_FUNCTION_ARGS)
{
	test_itemptr_pair(0, 2, 14);
	test_itemptr_pair(0, MaxHeapTuplesPerPage, 14);
	test_itemptr_pair(MaxBlockNumber, MaxHeapTuplesPerPage, 14);
	test_itemptr_pair(MaxBlockNumber, MaxHeapTuplesPerPage, 16);

	PG_RETURN_VOID();
}
