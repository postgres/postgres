/*--------------------------------------------------------------------------
 *
 * test_tidstore.c
 *		Test TidStore data structure.
 *
 * Note: all locking in this test module is useless since there is only
 * a single process to use the TidStore. It is meant to be an example of
 * usage.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_tidstore/test_tidstore.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tidstore.h"
#include "fmgr.h"
#include "storage/block.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "utils/array.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_create);
PG_FUNCTION_INFO_V1(do_set_block_offsets);
PG_FUNCTION_INFO_V1(check_set_block_offsets);
PG_FUNCTION_INFO_V1(test_is_full);
PG_FUNCTION_INFO_V1(test_destroy);

static TidStore *tidstore = NULL;
static size_t tidstore_empty_size;

/* array for verification of some tests */
typedef struct ItemArray
{
	ItemPointerData *insert_tids;
	ItemPointerData *lookup_tids;
	ItemPointerData *iter_tids;
	int			max_tids;
	int			num_tids;
} ItemArray;

static ItemArray items;

/* comparator routine for ItemPointer */
static int
itemptr_cmp(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;
	OffsetNumber loff,
				roff;

	lblk = ItemPointerGetBlockNumber((ItemPointer) left);
	rblk = ItemPointerGetBlockNumber((ItemPointer) right);

	if (lblk < rblk)
		return -1;
	if (lblk > rblk)
		return 1;

	loff = ItemPointerGetOffsetNumber((ItemPointer) left);
	roff = ItemPointerGetOffsetNumber((ItemPointer) right);

	if (loff < roff)
		return -1;
	if (loff > roff)
		return 1;

	return 0;
}

/*
 * Create a TidStore. If shared is false, the tidstore is created
 * on TopMemoryContext, otherwise on DSA. Although the tidstore
 * is created on DSA, only the same process can subsequently use
 * the tidstore. The tidstore handle is not shared anywhere.
*/
Datum
test_create(PG_FUNCTION_ARGS)
{
	bool		shared = PG_GETARG_BOOL(0);
	MemoryContext old_ctx;

	/* doesn't really matter, since it's just a hint */
	size_t		tidstore_max_size = 2 * 1024 * 1024;
	size_t		array_init_size = 1024;

	Assert(tidstore == NULL);

	/*
	 * Create the TidStore on TopMemoryContext so that the same process use it
	 * for subsequent tests.
	 */
	old_ctx = MemoryContextSwitchTo(TopMemoryContext);

	if (shared)
	{
		int			tranche_id;

		tranche_id = LWLockNewTrancheId();
		LWLockRegisterTranche(tranche_id, "test_tidstore");

		tidstore = TidStoreCreateShared(tidstore_max_size, tranche_id);

		/*
		 * Remain attached until end of backend or explicitly detached so that
		 * the same process use the tidstore for subsequent tests.
		 */
		dsa_pin_mapping(TidStoreGetDSA(tidstore));
	}
	else
		/* VACUUM uses insert only, so we test the other option. */
		tidstore = TidStoreCreateLocal(tidstore_max_size, false);

	tidstore_empty_size = TidStoreMemoryUsage(tidstore);

	items.num_tids = 0;
	items.max_tids = array_init_size / sizeof(ItemPointerData);
	items.insert_tids = (ItemPointerData *) palloc0(array_init_size);
	items.lookup_tids = (ItemPointerData *) palloc0(array_init_size);
	items.iter_tids = (ItemPointerData *) palloc0(array_init_size);

	MemoryContextSwitchTo(old_ctx);

	PG_RETURN_VOID();
}

static void
sanity_check_array(ArrayType *ta)
{
	if (ARR_HASNULL(ta) && array_contains_nulls(ta))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	if (ARR_NDIM(ta) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));
}

static void
check_tidstore_available(void)
{
	if (tidstore == NULL)
		elog(ERROR, "tidstore is not created");
}

static void
purge_from_verification_array(BlockNumber blkno)
{
	int			dst = 0;

	for (int src = 0; src < items.num_tids; src++)
		if (ItemPointerGetBlockNumber(&items.insert_tids[src]) != blkno)
			items.insert_tids[dst++] = items.insert_tids[src];
	items.num_tids = dst;
}


/* Set the given block and offsets pairs */
Datum
do_set_block_offsets(PG_FUNCTION_ARGS)
{
	BlockNumber blkno = PG_GETARG_INT64(0);
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P_COPY(1);
	OffsetNumber *offs;
	int			noffs;

	check_tidstore_available();
	sanity_check_array(ta);

	noffs = ArrayGetNItems(ARR_NDIM(ta), ARR_DIMS(ta));
	offs = ((OffsetNumber *) ARR_DATA_PTR(ta));

	/* Set TIDs in the store */
	TidStoreLockExclusive(tidstore);
	TidStoreSetBlockOffsets(tidstore, blkno, offs, noffs);
	TidStoreUnlock(tidstore);

	/* Remove the existing items of blkno from the verification array */
	purge_from_verification_array(blkno);

	/* Set TIDs in verification array */
	for (int i = 0; i < noffs; i++)
	{
		ItemPointer tid;
		int			idx = items.num_tids + i;

		/* Enlarge the TID arrays if necessary */
		if (idx >= items.max_tids)
		{
			items.max_tids *= 2;
			items.insert_tids = repalloc(items.insert_tids, sizeof(ItemPointerData) * items.max_tids);
			items.lookup_tids = repalloc(items.lookup_tids, sizeof(ItemPointerData) * items.max_tids);
			items.iter_tids = repalloc(items.iter_tids, sizeof(ItemPointerData) * items.max_tids);
		}

		tid = &(items.insert_tids[idx]);
		ItemPointerSet(tid, blkno, offs[i]);
	}

	/* Update statistics */
	items.num_tids += noffs;

	PG_RETURN_INT64(blkno);
}

/*
 * Verify TIDs in store against the array.
 */
Datum
check_set_block_offsets(PG_FUNCTION_ARGS)
{
	TidStoreIter *iter;
	TidStoreIterResult *iter_result;
	int			num_iter_tids = 0;
	int			num_lookup_tids = 0;
	BlockNumber prevblkno = 0;

	check_tidstore_available();

	/* lookup each member in the verification array */
	for (int i = 0; i < items.num_tids; i++)
		if (!TidStoreIsMember(tidstore, &items.insert_tids[i]))
			elog(ERROR, "missing TID with block %u, offset %u",
				 ItemPointerGetBlockNumber(&items.insert_tids[i]),
				 ItemPointerGetOffsetNumber(&items.insert_tids[i]));

	/*
	 * Lookup all possible TIDs for each distinct block in the verification
	 * array and save successful lookups in the lookup array.
	 */

	for (int i = 0; i < items.num_tids; i++)
	{
		BlockNumber blkno = ItemPointerGetBlockNumber(&items.insert_tids[i]);

		if (i > 0 && blkno == prevblkno)
			continue;

		for (OffsetNumber offset = FirstOffsetNumber; offset < MaxOffsetNumber; offset++)
		{
			ItemPointerData tid;

			ItemPointerSet(&tid, blkno, offset);

			TidStoreLockShare(tidstore);
			if (TidStoreIsMember(tidstore, &tid))
				ItemPointerSet(&items.lookup_tids[num_lookup_tids++], blkno, offset);
			TidStoreUnlock(tidstore);
		}

		prevblkno = blkno;
	}

	/* Collect TIDs stored in the tidstore, in order */

	TidStoreLockShare(tidstore);
	iter = TidStoreBeginIterate(tidstore);
	while ((iter_result = TidStoreIterateNext(iter)) != NULL)
	{
		OffsetNumber offsets[MaxOffsetNumber];
		int			num_offsets;

		num_offsets = TidStoreGetBlockOffsets(iter_result, offsets, lengthof(offsets));
		Assert(num_offsets <= lengthof(offsets));
		for (int i = 0; i < num_offsets; i++)
			ItemPointerSet(&(items.iter_tids[num_iter_tids++]), iter_result->blkno,
						   offsets[i]);
	}
	TidStoreEndIterate(iter);
	TidStoreUnlock(tidstore);

	/*
	 * Sort verification and lookup arrays and test that all arrays are the
	 * same.
	 */

	if (num_lookup_tids != items.num_tids)
		elog(ERROR, "should have %d TIDs, have %d", items.num_tids, num_lookup_tids);
	if (num_iter_tids != items.num_tids)
		elog(ERROR, "should have %d TIDs, have %d", items.num_tids, num_iter_tids);

	qsort(items.insert_tids, items.num_tids, sizeof(ItemPointerData), itemptr_cmp);
	qsort(items.lookup_tids, items.num_tids, sizeof(ItemPointerData), itemptr_cmp);
	for (int i = 0; i < items.num_tids; i++)
	{
		if (itemptr_cmp((const void *) &items.insert_tids[i], (const void *) &items.iter_tids[i]) != 0)
			elog(ERROR, "TID iter array doesn't match verification array, got (%u,%u) expected (%u,%u)",
				 ItemPointerGetBlockNumber(&items.iter_tids[i]),
				 ItemPointerGetOffsetNumber(&items.iter_tids[i]),
				 ItemPointerGetBlockNumber(&items.insert_tids[i]),
				 ItemPointerGetOffsetNumber(&items.insert_tids[i]));
		if (itemptr_cmp((const void *) &items.insert_tids[i], (const void *) &items.lookup_tids[i]) != 0)
			elog(ERROR, "TID lookup array doesn't match verification array, got (%u,%u) expected (%u,%u)",
				 ItemPointerGetBlockNumber(&items.lookup_tids[i]),
				 ItemPointerGetOffsetNumber(&items.lookup_tids[i]),
				 ItemPointerGetBlockNumber(&items.insert_tids[i]),
				 ItemPointerGetOffsetNumber(&items.insert_tids[i]));
	}

	PG_RETURN_VOID();
}

/*
 * In real world use, we care if the memory usage is greater than
 * some configured limit. Here we just want to verify that
 * TidStoreMemoryUsage is not broken.
 */
Datum
test_is_full(PG_FUNCTION_ARGS)
{
	bool		is_full;

	check_tidstore_available();

	is_full = (TidStoreMemoryUsage(tidstore) > tidstore_empty_size);

	PG_RETURN_BOOL(is_full);
}

/* Free the tidstore */
Datum
test_destroy(PG_FUNCTION_ARGS)
{
	check_tidstore_available();

	TidStoreDestroy(tidstore);
	tidstore = NULL;
	items.num_tids = 0;
	pfree(items.insert_tids);
	pfree(items.lookup_tids);
	pfree(items.iter_tids);

	PG_RETURN_VOID();
}
