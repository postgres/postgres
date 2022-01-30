/*-------------------------------------------------------------------------
 *
 * heap_surgery.c
 *	  Functions to perform surgery on the damaged heap table.
 *
 * Copyright (c) 2020-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_surgery/heap_surgery.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_proc_d.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* Options to forcefully change the state of a heap tuple. */
typedef enum HeapTupleForceOption
{
	HEAP_FORCE_KILL,
	HEAP_FORCE_FREEZE
} HeapTupleForceOption;

PG_FUNCTION_INFO_V1(heap_force_kill);
PG_FUNCTION_INFO_V1(heap_force_freeze);

static int32 tidcmp(const void *a, const void *b);
static Datum heap_force_common(FunctionCallInfo fcinfo,
							   HeapTupleForceOption heap_force_opt);
static void sanity_check_tid_array(ArrayType *ta, int *ntids);
static BlockNumber find_tids_one_page(ItemPointer tids, int ntids,
									  OffsetNumber *next_start_ptr);

/*-------------------------------------------------------------------------
 * heap_force_kill()
 *
 * Force kill the tuple(s) pointed to by the item pointer(s) stored in the
 * given TID array.
 *
 * Usage: SELECT heap_force_kill(regclass, tid[]);
 *-------------------------------------------------------------------------
 */
Datum
heap_force_kill(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(heap_force_common(fcinfo, HEAP_FORCE_KILL));
}

/*-------------------------------------------------------------------------
 * heap_force_freeze()
 *
 * Force freeze the tuple(s) pointed to by the item pointer(s) stored in the
 * given TID array.
 *
 * Usage: SELECT heap_force_freeze(regclass, tid[]);
 *-------------------------------------------------------------------------
 */
Datum
heap_force_freeze(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(heap_force_common(fcinfo, HEAP_FORCE_FREEZE));
}

/*-------------------------------------------------------------------------
 * heap_force_common()
 *
 * Common code for heap_force_kill and heap_force_freeze
 *-------------------------------------------------------------------------
 */
static Datum
heap_force_common(FunctionCallInfo fcinfo, HeapTupleForceOption heap_force_opt)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P_COPY(1);
	ItemPointer tids;
	int			ntids,
				nblocks;
	Relation	rel;
	OffsetNumber curr_start_ptr,
				next_start_ptr;
	bool		include_this_tid[MaxHeapTuplesPerPage];

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("heap surgery functions cannot be executed during recovery.")));

	/* Check inputs. */
	sanity_check_tid_array(ta, &ntids);

	rel = relation_open(relid, RowExclusiveLock);

	/*
	 * Check target relation.
	 */
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot operate on relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	/* Must be owner of the table or superuser. */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	tids = ((ItemPointer) ARR_DATA_PTR(ta));

	/*
	 * If there is more than one TID in the array, sort them so that we can
	 * easily fetch all the TIDs belonging to one particular page from the
	 * array.
	 */
	if (ntids > 1)
		qsort((void *) tids, ntids, sizeof(ItemPointerData), tidcmp);

	curr_start_ptr = next_start_ptr = 0;
	nblocks = RelationGetNumberOfBlocks(rel);

	/*
	 * Loop, performing the necessary actions for each block.
	 */
	while (next_start_ptr != ntids)
	{
		Buffer		buf;
		Buffer		vmbuf = InvalidBuffer;
		Page		page;
		BlockNumber blkno;
		OffsetNumber curoff;
		OffsetNumber maxoffset;
		int			i;
		bool		did_modify_page = false;
		bool		did_modify_vm = false;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Find all the TIDs belonging to one particular page starting from
		 * next_start_ptr and process them one by one.
		 */
		blkno = find_tids_one_page(tids, ntids, &next_start_ptr);

		/* Check whether the block number is valid. */
		if (blkno >= nblocks)
		{
			/* Update the current_start_ptr before moving to the next page. */
			curr_start_ptr = next_start_ptr;

			ereport(NOTICE,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("skipping block %u for relation \"%s\" because the block number is out of range",
							blkno, RelationGetRelationName(rel))));
			continue;
		}

		buf = ReadBuffer(rel, blkno);
		LockBufferForCleanup(buf);

		page = BufferGetPage(buf);

		maxoffset = PageGetMaxOffsetNumber(page);

		/*
		 * Figure out which TIDs we are going to process and which ones we are
		 * going to skip.
		 */
		memset(include_this_tid, 0, sizeof(include_this_tid));
		for (i = curr_start_ptr; i < next_start_ptr; i++)
		{
			OffsetNumber offno = ItemPointerGetOffsetNumberNoCheck(&tids[i]);
			ItemId		itemid;

			/* Check whether the offset number is valid. */
			if (offno == InvalidOffsetNumber || offno > maxoffset)
			{
				ereport(NOTICE,
						errmsg("skipping tid (%u, %u) for relation \"%s\" because the item number is out of range",
							   blkno, offno, RelationGetRelationName(rel)));
				continue;
			}

			itemid = PageGetItemId(page, offno);

			/* Only accept an item ID that is used. */
			if (ItemIdIsRedirected(itemid))
			{
				ereport(NOTICE,
						errmsg("skipping tid (%u, %u) for relation \"%s\" because it redirects to item %u",
							   blkno, offno, RelationGetRelationName(rel),
							   ItemIdGetRedirect(itemid)));
				continue;
			}
			else if (ItemIdIsDead(itemid))
			{
				ereport(NOTICE,
						(errmsg("skipping tid (%u, %u) for relation \"%s\" because it is marked dead",
								blkno, offno, RelationGetRelationName(rel))));
				continue;
			}
			else if (!ItemIdIsUsed(itemid))
			{
				ereport(NOTICE,
						(errmsg("skipping tid (%u, %u) for relation \"%s\" because it is marked unused",
								blkno, offno, RelationGetRelationName(rel))));
				continue;
			}

			/* Mark it for processing. */
			Assert(offno < MaxHeapTuplesPerPage);
			include_this_tid[offno] = true;
		}

		/*
		 * Before entering the critical section, pin the visibility map page
		 * if it appears to be necessary.
		 */
		if (heap_force_opt == HEAP_FORCE_KILL && PageIsAllVisible(page))
			visibilitymap_pin(rel, blkno, &vmbuf);

		/* No ereport(ERROR) from here until all the changes are logged. */
		START_CRIT_SECTION();

		for (curoff = FirstOffsetNumber; curoff <= maxoffset;
			 curoff = OffsetNumberNext(curoff))
		{
			ItemId		itemid;

			if (!include_this_tid[curoff])
				continue;

			itemid = PageGetItemId(page, curoff);
			Assert(ItemIdIsNormal(itemid));

			did_modify_page = true;

			if (heap_force_opt == HEAP_FORCE_KILL)
			{
				ItemIdSetDead(itemid);

				/*
				 * If the page is marked all-visible, we must clear
				 * PD_ALL_VISIBLE flag on the page header and an all-visible
				 * bit on the visibility map corresponding to the page.
				 */
				if (PageIsAllVisible(page))
				{
					PageClearAllVisible(page);
					visibilitymap_clear(rel, blkno, vmbuf,
										VISIBILITYMAP_VALID_BITS);
					did_modify_vm = true;
				}
			}
			else
			{
				HeapTupleHeader htup;

				Assert(heap_force_opt == HEAP_FORCE_FREEZE);

				htup = (HeapTupleHeader) PageGetItem(page, itemid);

				/*
				 * Reset all visibility-related fields of the tuple. This
				 * logic should mimic heap_execute_freeze_tuple(), but we
				 * choose to reset xmin and ctid just to be sure that no
				 * potentially-garbled data is left behind.
				 */
				ItemPointerSet(&htup->t_ctid, blkno, curoff);
				HeapTupleHeaderSetXmin(htup, FrozenTransactionId);
				HeapTupleHeaderSetXmax(htup, InvalidTransactionId);
				if (htup->t_infomask & HEAP_MOVED)
				{
					if (htup->t_infomask & HEAP_MOVED_OFF)
						HeapTupleHeaderSetXvac(htup, InvalidTransactionId);
					else
						HeapTupleHeaderSetXvac(htup, FrozenTransactionId);
				}

				/*
				 * Clear all the visibility-related bits of this tuple and
				 * mark it as frozen. Also, get rid of HOT_UPDATED and
				 * KEYS_UPDATES bits.
				 */
				htup->t_infomask &= ~HEAP_XACT_MASK;
				htup->t_infomask |= (HEAP_XMIN_FROZEN | HEAP_XMAX_INVALID);
				htup->t_infomask2 &= ~HEAP_HOT_UPDATED;
				htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
			}
		}

		/*
		 * If the page was modified, only then, we mark the buffer dirty or do
		 * the WAL logging.
		 */
		if (did_modify_page)
		{
			/* Mark buffer dirty before we write WAL. */
			MarkBufferDirty(buf);

			/* XLOG stuff */
			if (RelationNeedsWAL(rel))
				log_newpage_buffer(buf, true);
		}

		/* WAL log the VM page if it was modified. */
		if (did_modify_vm && RelationNeedsWAL(rel))
			log_newpage_buffer(vmbuf, false);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		if (vmbuf != InvalidBuffer)
			ReleaseBuffer(vmbuf);

		/* Update the current_start_ptr before moving to the next page. */
		curr_start_ptr = next_start_ptr;
	}

	relation_close(rel, RowExclusiveLock);

	pfree(ta);

	PG_RETURN_VOID();
}

/*-------------------------------------------------------------------------
 * tidcmp()
 *
 * Compare two item pointers, return -1, 0, or +1.
 *
 * See ItemPointerCompare for details.
 * ------------------------------------------------------------------------
 */
static int32
tidcmp(const void *a, const void *b)
{
	ItemPointer iptr1 = ((const ItemPointer) a);
	ItemPointer iptr2 = ((const ItemPointer) b);

	return ItemPointerCompare(iptr1, iptr2);
}

/*-------------------------------------------------------------------------
 * sanity_check_tid_array()
 *
 * Perform sanity checks on the given tid array, and set *ntids to the
 * number of items in the array.
 * ------------------------------------------------------------------------
 */
static void
sanity_check_tid_array(ArrayType *ta, int *ntids)
{
	if (ARR_HASNULL(ta) && array_contains_nulls(ta))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	if (ARR_NDIM(ta) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));

	*ntids = ArrayGetNItems(ARR_NDIM(ta), ARR_DIMS(ta));
}

/*-------------------------------------------------------------------------
 * find_tids_one_page()
 *
 * Find all the tids residing in the same page as tids[next_start_ptr], and
 * update next_start_ptr so that it points to the first tid in the next page.
 *
 * NOTE: The input tids[] array must be sorted.
 * ------------------------------------------------------------------------
 */
static BlockNumber
find_tids_one_page(ItemPointer tids, int ntids, OffsetNumber *next_start_ptr)
{
	int			i;
	BlockNumber prev_blkno,
				blkno;

	prev_blkno = blkno = InvalidBlockNumber;

	for (i = *next_start_ptr; i < ntids; i++)
	{
		ItemPointerData tid = tids[i];

		blkno = ItemPointerGetBlockNumberNoCheck(&tid);

		if (i == *next_start_ptr)
			prev_blkno = blkno;

		if (prev_blkno != blkno)
			break;
	}

	*next_start_ptr = i;
	return prev_blkno;
}
