/*-------------------------------------------------------------------------
 *
 * verify_nbtree.c
 *		Verifies the integrity of nbtree indexes based on invariants.
 *
 * For B-Tree indexes, verification includes checking that each page in the
 * target index has items in logical order as reported by an insertion scankey
 * (the insertion scankey sort-wise NULL semantics are needed for
 * verification).
 *
 *
 * Copyright (c) 2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_nbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


PG_MODULE_MAGIC;

/*
 * A B-Tree cannot possibly have this many levels, since there must be one
 * block per level, which is bound by the range of BlockNumber:
 */
#define InvalidBtreeLevel	((uint32) InvalidBlockNumber)

/*
 * State associated with verifying a B-Tree index
 *
 * target is the point of reference for a verification operation.
 *
 * Other B-Tree pages may be allocated, but those are always auxiliary (e.g.,
 * they are current target's child pages). Conceptually, problems are only
 * ever found in the current target page. Each page found by verification's
 * left/right, top/bottom scan becomes the target exactly once.
 */
typedef struct BtreeCheckState
{
	/*
	 * Unchanging state, established at start of verification:
	 */

	/* B-Tree Index Relation */
	Relation	rel;
	/* ShareLock held on heap/index, rather than AccessShareLock? */
	bool		readonly;
	/* Per-page context */
	MemoryContext targetcontext;
	/* Buffer access strategy */
	BufferAccessStrategy checkstrategy;

	/*
	 * Mutable state, for verification of particular page:
	 */

	/* Current target page */
	Page		target;
	/* Target block number */
	BlockNumber targetblock;
	/* Target page's LSN */
	XLogRecPtr	targetlsn;
} BtreeCheckState;

/*
 * Starting point for verifying an entire B-Tree index level
 */
typedef struct BtreeLevel
{
	/* Level number (0 is leaf page level). */
	uint32		level;

	/* Left most block on level.  Scan of level begins here. */
	BlockNumber leftmost;

	/* Is this level reported as "true" root level by meta page? */
	bool		istruerootlevel;
} BtreeLevel;

PG_FUNCTION_INFO_V1(bt_index_check);
PG_FUNCTION_INFO_V1(bt_index_parent_check);

static void bt_index_check_internal(Oid indrelid, bool parentcheck);
static inline void btree_index_checkable(Relation rel);
static void bt_check_every_level(Relation rel, bool readonly);
static BtreeLevel bt_check_level_from_leftmost(BtreeCheckState *state,
							 BtreeLevel level);
static void bt_target_page_check(BtreeCheckState *state);
static ScanKey bt_right_page_check_scankey(BtreeCheckState *state);
static void bt_downlink_check(BtreeCheckState *state, BlockNumber childblock,
				  ScanKey targetkey);
static inline bool offset_is_negative_infinity(BTPageOpaque opaque,
							OffsetNumber offset);
static inline bool invariant_leq_offset(BtreeCheckState *state,
					 ScanKey key,
					 OffsetNumber upperbound);
static inline bool invariant_geq_offset(BtreeCheckState *state,
					 ScanKey key,
					 OffsetNumber lowerbound);
static inline bool invariant_leq_nontarget_offset(BtreeCheckState *state,
							   Page other,
							   ScanKey key,
							   OffsetNumber upperbound);
static Page palloc_btree_page(BtreeCheckState *state, BlockNumber blocknum);

/*
 * bt_index_check(index regclass)
 *
 * Verify integrity of B-Tree index.
 *
 * Acquires AccessShareLock on heap & index relations.  Does not consider
 * invariants that exist between parent/child pages.
 */
Datum
bt_index_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);

	bt_index_check_internal(indrelid, false);

	PG_RETURN_VOID();
}

/*
 * bt_index_parent_check(index regclass)
 *
 * Verify integrity of B-Tree index.
 *
 * Acquires ShareLock on heap & index relations.  Verifies that downlinks in
 * parent pages are valid lower bounds on child pages.
 */
Datum
bt_index_parent_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);

	bt_index_check_internal(indrelid, true);

	PG_RETURN_VOID();
}

/*
 * Helper for bt_index_[parent_]check, coordinating the bulk of the work.
 */
static void
bt_index_check_internal(Oid indrelid, bool parentcheck)
{
	Oid			heapid;
	Relation	indrel;
	Relation	heaprel;
	LOCKMODE	lockmode;

	if (parentcheck)
		lockmode = ShareLock;
	else
		lockmode = AccessShareLock;

	/*
	 * We must lock table before index to avoid deadlocks.  However, if the
	 * passed indrelid isn't an index then IndexGetRelation() will fail.
	 * Rather than emitting a not-very-helpful error message, postpone
	 * complaining, expecting that the is-it-an-index test below will fail.
	 *
	 * In hot standby mode this will raise an error when parentcheck is true.
	 */
	heapid = IndexGetRelation(indrelid, true);
	if (OidIsValid(heapid))
		heaprel = heap_open(heapid, lockmode);
	else
		heaprel = NULL;

	/*
	 * Open the target index relations separately (like relation_openrv(), but
	 * with heap relation locked first to prevent deadlocking).  In hot
	 * standby mode this will raise an error when parentcheck is true.
	 */
	indrel = index_open(indrelid, lockmode);

	/*
	 * Since we did the IndexGetRelation call above without any lock, it's
	 * barely possible that a race against an index drop/recreation could have
	 * netted us the wrong table.  Although the table itself won't actually be
	 * examined during verification currently, a recheck still seems like a
	 * good idea.
	 */
	if (heaprel == NULL || heapid != IndexGetRelation(indrelid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index %s",
						RelationGetRelationName(indrel))));

	/* Relation suitable for checking as B-Tree? */
	btree_index_checkable(indrel);

	/* Check index */
	bt_check_every_level(indrel, parentcheck);

	/*
	 * Release locks early. That's ok here because nothing in the called
	 * routines will trigger shared cache invalidations to be sent, so we can
	 * relax the usual pattern of only releasing locks after commit.
	 */
	index_close(indrel, lockmode);
	if (heaprel)
		heap_close(heaprel, lockmode);
}

/*
 * Basic checks about the suitability of a relation for checking as a B-Tree
 * index.
 *
 * NB: Intentionally not checking permissions, the function is normally not
 * callable by non-superusers. If granted, it's useful to be able to check a
 * whole cluster.
 */
static inline void
btree_index_checkable(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_INDEX ||
		rel->rd_rel->relam != BTREE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only B-Tree indexes are supported as targets for verification"),
				 errdetail("Relation \"%s\" is not a B-Tree index.",
						   RelationGetRelationName(rel))));

	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions"),
			 errdetail("Index \"%s\" is associated with temporary relation.",
					   RelationGetRelationName(rel))));

	if (!IndexIsValid(rel->rd_index))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot check index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Index is not valid")));
}

/*
 * Main entry point for B-Tree SQL-callable functions. Walks the B-Tree in
 * logical order, verifying invariants as it goes.
 *
 * It is the caller's responsibility to acquire appropriate heavyweight lock on
 * the index relation, and advise us if extra checks are safe when a ShareLock
 * is held.
 *
 * A ShareLock is generally assumed to prevent any kind of physical
 * modification to the index structure, including modifications that VACUUM may
 * make.  This does not include setting of the LP_DEAD bit by concurrent index
 * scans, although that is just metadata that is not able to directly affect
 * any check performed here.  Any concurrent process that might act on the
 * LP_DEAD bit being set (recycle space) requires a heavyweight lock that
 * cannot be held while we hold a ShareLock.  (Besides, even if that could
 * happen, the ad-hoc recycling when a page might otherwise split is performed
 * per-page, and requires an exclusive buffer lock, which wouldn't cause us
 * trouble.  _bt_delitems_vacuum() may only delete leaf items, and so the extra
 * parent/child check cannot be affected.)
 */
static void
bt_check_every_level(Relation rel, bool readonly)
{
	BtreeCheckState *state;
	Page		metapage;
	BTMetaPageData *metad;
	uint32		previouslevel;
	BtreeLevel	current;

	/*
	 * RecentGlobalXmin assertion matches index_getnext_tid().  See note on
	 * RecentGlobalXmin/B-Tree page deletion.
	 */
	Assert(TransactionIdIsValid(RecentGlobalXmin));

	/*
	 * Initialize state for entire verification operation
	 */
	state = palloc(sizeof(BtreeCheckState));
	state->rel = rel;
	state->readonly = readonly;
	/* Create context for page */
	state->targetcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "amcheck context",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);
	state->checkstrategy = GetAccessStrategy(BAS_BULKREAD);

	/* Get true root block from meta-page */
	metapage = palloc_btree_page(state, BTREE_METAPAGE);
	metad = BTPageGetMeta(metapage);

	/*
	 * Certain deletion patterns can result in "skinny" B-Tree indexes, where
	 * the fast root and true root differ.
	 *
	 * Start from the true root, not the fast root, unlike conventional index
	 * scans.  This approach is more thorough, and removes the risk of
	 * following a stale fast root from the meta page.
	 */
	if (metad->btm_fastroot != metad->btm_root)
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("harmless fast root mismatch in index %s",
						RelationGetRelationName(rel)),
				 errdetail_internal("Fast root block %u (level %u) differs from true root block %u (level %u).",
									metad->btm_fastroot, metad->btm_fastlevel,
									metad->btm_root, metad->btm_level)));

	/*
	 * Starting at the root, verify every level.  Move left to right, top to
	 * bottom.  Note that there may be no pages other than the meta page (meta
	 * page can indicate that root is P_NONE when the index is totally empty).
	 */
	previouslevel = InvalidBtreeLevel;
	current.level = metad->btm_level;
	current.leftmost = metad->btm_root;
	current.istruerootlevel = true;
	while (current.leftmost != P_NONE)
	{
		/*
		 * Verify this level, and get left most page for next level down, if
		 * not at leaf level
		 */
		current = bt_check_level_from_leftmost(state, current);

		if (current.leftmost == InvalidBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has no valid pages on level below %u or first level",
							RelationGetRelationName(rel), previouslevel)));

		previouslevel = current.level;
	}

	/* Be tidy: */
	MemoryContextDelete(state->targetcontext);
}

/*
 * Given a left-most block at some level, move right, verifying each page
 * individually (with more verification across pages for "readonly"
 * callers).  Caller should pass the true root page as the leftmost initially,
 * working their way down by passing what is returned for the last call here
 * until level 0 (leaf page level) was reached.
 *
 * Returns state for next call, if any.  This includes left-most block number
 * one level lower that should be passed on next level/call, which is set to
 * P_NONE on last call here (when leaf level is verified).  Level numbers
 * follow the nbtree convention: higher levels have higher numbers, because new
 * levels are added only due to a root page split.  Note that prior to the
 * first root page split, the root is also a leaf page, so there is always a
 * level 0 (leaf level), and it's always the last level processed.
 *
 * Note on memory management:  State's per-page context is reset here, between
 * each call to bt_target_page_check().
 */
static BtreeLevel
bt_check_level_from_leftmost(BtreeCheckState *state, BtreeLevel level)
{
	/* State to establish early, concerning entire level */
	BTPageOpaque opaque;
	MemoryContext oldcontext;
	BtreeLevel	nextleveldown;

	/* Variables for iterating across level using right links */
	BlockNumber leftcurrent = P_NONE;
	BlockNumber current = level.leftmost;

	/* Initialize return state */
	nextleveldown.leftmost = InvalidBlockNumber;
	nextleveldown.level = InvalidBtreeLevel;
	nextleveldown.istruerootlevel = false;

	/* Use page-level context for duration of this call */
	oldcontext = MemoryContextSwitchTo(state->targetcontext);

	elog(DEBUG2, "verifying level %u%s", level.level,
		 level.istruerootlevel ?
		 " (true root level)" : level.level == 0 ? " (leaf level)" : "");

	do
	{
		/* Don't rely on CHECK_FOR_INTERRUPTS() calls at lower level */
		CHECK_FOR_INTERRUPTS();

		/* Initialize state for this iteration */
		state->targetblock = current;
		state->target = palloc_btree_page(state, state->targetblock);
		state->targetlsn = PageGetLSN(state->target);

		opaque = (BTPageOpaque) PageGetSpecialPointer(state->target);

		if (P_IGNORE(opaque))
		{
			if (P_RIGHTMOST(opaque))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("block %u fell off the end of index \"%s\"",
							 current, RelationGetRelationName(state->rel))));
			else
				ereport(DEBUG1,
						(errcode(ERRCODE_NO_DATA),
						 errmsg("block %u of index \"%s\" ignored",
							 current, RelationGetRelationName(state->rel))));
			goto nextpage;
		}
		else if (nextleveldown.leftmost == InvalidBlockNumber)
		{
			/*
			 * A concurrent page split could make the caller supplied leftmost
			 * block no longer contain the leftmost page, or no longer be the
			 * true root, but where that isn't possible due to heavyweight
			 * locking, check that the first valid page meets caller's
			 * expectations.
			 */
			if (state->readonly)
			{
				if (!P_LEFTMOST(opaque))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
						   errmsg("block %u is not leftmost in index \"%s\"",
							 current, RelationGetRelationName(state->rel))));

				if (level.istruerootlevel && !P_ISROOT(opaque))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
						  errmsg("block %u is not true root in index \"%s\"",
							 current, RelationGetRelationName(state->rel))));
			}

			/*
			 * Before beginning any non-trivial examination of level, prepare
			 * state for next bt_check_level_from_leftmost() invocation for
			 * the next level for the next level down (if any).
			 *
			 * There should be at least one non-ignorable page per level,
			 * unless this is the leaf level, which is assumed by caller to be
			 * final level.
			 */
			if (!P_ISLEAF(opaque))
			{
				IndexTuple	itup;
				ItemId		itemid;

				/* Internal page -- downlink gets leftmost on next level */
				itemid = PageGetItemId(state->target, P_FIRSTDATAKEY(opaque));
				itup = (IndexTuple) PageGetItem(state->target, itemid);
				nextleveldown.leftmost = ItemPointerGetBlockNumber(&(itup->t_tid));
				nextleveldown.level = opaque->btpo.level - 1;
			}
			else
			{
				/*
				 * Leaf page -- final level caller must process.
				 *
				 * Note that this could also be the root page, if there has
				 * been no root page split yet.
				 */
				nextleveldown.leftmost = P_NONE;
				nextleveldown.level = InvalidBtreeLevel;
			}

			/*
			 * Finished setting up state for this call/level.  Control will
			 * never end up back here in any future loop iteration for this
			 * level.
			 */
		}

		if (state->readonly && opaque->btpo_prev != leftcurrent)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("left link/right link pair in index \"%s\" not in agreement",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Block=%u left block=%u left link from block=%u.",
								  current, leftcurrent, opaque->btpo_prev)));

		/* Check level, which must be valid for non-ignorable page */
		if (level.level != opaque->btpo.level)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("leftmost down link for level points to block in index \"%s\" whose level is not one level down",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Block pointed to=%u expected level=%u level in pointed to block=%u.",
								 current, level.level, opaque->btpo.level)));

		/* Verify invariants for page -- all important checks occur here */
		bt_target_page_check(state);

nextpage:

		/* Try to detect circular links */
		if (current == leftcurrent || current == opaque->btpo_prev)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
			  errmsg("circular link chain found in block %u of index \"%s\"",
					 current, RelationGetRelationName(state->rel))));

		leftcurrent = current;
		current = opaque->btpo_next;

		/* Free page and associated memory for this iteration */
		MemoryContextReset(state->targetcontext);
	}
	while (current != P_NONE);

	/* Don't change context for caller */
	MemoryContextSwitchTo(oldcontext);

	return nextleveldown;
}

/*
 * Function performs the following checks on target page, or pages ancillary to
 * target page:
 *
 * - That every "real" data item is less than or equal to the high key, which
 *	 is an upper bound on the items on the pages (where there is a high key at
 *	 all -- pages that are rightmost lack one).
 *
 * - That within the page, every "real" item is less than or equal to the item
 *	 immediately to its right, if any (i.e., that the items are in order within
 *	 the page, so that the binary searches performed by index scans are sane).
 *
 * - That the last item stored on the page is less than or equal to the first
 *	 "real" data item on the page to the right (if such a first item is
 *	 available).
 *
 * Furthermore, when state passed shows ShareLock held, and target page is
 * internal page, function also checks:
 *
 * - That all child pages respect downlinks lower bound.
 *
 * Note:  Memory allocated in this routine is expected to be released by caller
 * resetting state->targetcontext.
 */
static void
bt_target_page_check(BtreeCheckState *state)
{
	OffsetNumber offset;
	OffsetNumber max;
	BTPageOpaque topaque;

	topaque = (BTPageOpaque) PageGetSpecialPointer(state->target);
	max = PageGetMaxOffsetNumber(state->target);

	elog(DEBUG2, "verifying %u items on %s block %u", max,
		 P_ISLEAF(topaque) ? "leaf" : "internal", state->targetblock);

	/*
	 * Loop over page items, starting from first non-highkey item, not high
	 * key (if any).  Also, immediately skip "negative infinity" real item (if
	 * any).
	 */
	for (offset = P_FIRSTDATAKEY(topaque);
		 offset <= max;
		 offset = OffsetNumberNext(offset))
	{
		ItemId		itemid;
		IndexTuple	itup;
		ScanKey		skey;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Don't try to generate scankey using "negative infinity" garbage
		 * data
		 */
		if (offset_is_negative_infinity(topaque, offset))
			continue;

		/* Build insertion scankey for current page offset */
		itemid = PageGetItemId(state->target, offset);
		itup = (IndexTuple) PageGetItem(state->target, itemid);
		skey = _bt_mkscankey(state->rel, itup);

		/*
		 * * High key check *
		 *
		 * If there is a high key (if this is not the rightmost page on its
		 * entire level), check that high key actually is upper bound on all
		 * page items.
		 *
		 * We prefer to check all items against high key rather than checking
		 * just the last and trusting that the operator class obeys the
		 * transitive law (which implies that all previous items also
		 * respected the high key invariant if they pass the item order
		 * check).
		 *
		 * Ideally, we'd compare every item in the index against every other
		 * item in the index, and not trust opclass obedience of the
		 * transitive law to bridge the gap between children and their
		 * grandparents (as well as great-grandparents, and so on).  We don't
		 * go to those lengths because that would be prohibitively expensive,
		 * and probably not markedly more effective in practice.
		 */
		if (!P_RIGHTMOST(topaque) &&
			!invariant_leq_offset(state, skey, P_HIKEY))
		{
			char	   *itid,
					   *htid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumber(&(itup->t_tid)),
							ItemPointerGetOffsetNumber(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("high key invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Index tid=%s points to %s tid=%s page lsn=%X/%X.",
										itid,
										P_ISLEAF(topaque) ? "heap" : "index",
										htid,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
		}

		/*
		 * * Item order check *
		 *
		 * Check that items are stored on page in logical order, by checking
		 * current item is less than or equal to next item (if any).
		 */
		if (OffsetNumberNext(offset) <= max &&
			!invariant_leq_offset(state, skey,
								  OffsetNumberNext(offset)))
		{
			char	   *itid,
					   *htid,
					   *nitid,
					   *nhtid;

			itid = psprintf("(%u,%u)", state->targetblock, offset);
			htid = psprintf("(%u,%u)",
							ItemPointerGetBlockNumber(&(itup->t_tid)),
							ItemPointerGetOffsetNumber(&(itup->t_tid)));
			nitid = psprintf("(%u,%u)", state->targetblock,
							 OffsetNumberNext(offset));

			/* Reuse itup to get pointed-to heap location of second item */
			itemid = PageGetItemId(state->target, OffsetNumberNext(offset));
			itup = (IndexTuple) PageGetItem(state->target, itemid);
			nhtid = psprintf("(%u,%u)",
							 ItemPointerGetBlockNumber(&(itup->t_tid)),
							 ItemPointerGetOffsetNumber(&(itup->t_tid)));

			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("item order invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
			   errdetail_internal("Lower index tid=%s (points to %s tid=%s) "
								  "higher index tid=%s (points to %s tid=%s) "
								  "page lsn=%X/%X.",
								  itid,
								  P_ISLEAF(topaque) ? "heap" : "index",
								  htid,
								  nitid,
								  P_ISLEAF(topaque) ? "heap" : "index",
								  nhtid,
								  (uint32) (state->targetlsn >> 32),
								  (uint32) state->targetlsn)));
		}

		/*
		 * * Last item check *
		 *
		 * Check last item against next/right page's first data item's when
		 * last item on page is reached.  This additional check can detect
		 * transposed pages.
		 *
		 * This check is similar to the item order check that will have
		 * already been performed for every other "real" item on target page
		 * when last item is checked.  The difference is that the next item
		 * (the item that is compared to target's last item) needs to come
		 * from the next/sibling page.  There may not be such an item
		 * available from sibling for various reasons, though (e.g., target is
		 * the rightmost page on level).
		 */
		else if (offset == max)
		{
			ScanKey		rightkey;

			/* Get item in next/right page */
			rightkey = bt_right_page_check_scankey(state);

			if (rightkey &&
				!invariant_geq_offset(state, rightkey, max))
			{
				/*
				 * As explained at length in bt_right_page_check_scankey(),
				 * there is a known !readonly race that could account for
				 * apparent violation of invariant, which we must check for
				 * before actually proceeding with raising error.  Our canary
				 * condition is that target page was deleted.
				 */
				if (!state->readonly)
				{
					/* Get fresh copy of target page */
					state->target = palloc_btree_page(state, state->targetblock);
					/* Note that we deliberately do not update target LSN */
					topaque = (BTPageOpaque) PageGetSpecialPointer(state->target);

					/*
					 * All !readonly checks now performed; just return
					 */
					if (P_IGNORE(topaque))
						return;
				}

				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("cross page item order invariant violated for index \"%s\"",
								RelationGetRelationName(state->rel)),
						 errdetail_internal("Last item on page tid=(%u,%u) page lsn=%X/%X.",
											state->targetblock, offset,
											(uint32) (state->targetlsn >> 32),
											(uint32) state->targetlsn)));
			}
		}

		/*
		 * * Downlink check *
		 *
		 * Additional check of child items iff this is an internal page and
		 * caller holds a ShareLock.  This happens for every downlink (item)
		 * in target excluding the negative-infinity downlink (again, this is
		 * because it has no useful value to compare).
		 */
		if (!P_ISLEAF(topaque) && state->readonly)
		{
			BlockNumber childblock = ItemPointerGetBlockNumber(&(itup->t_tid));

			bt_downlink_check(state, childblock, skey);
		}
	}
}

/*
 * Return a scankey for an item on page to right of current target (or the
 * first non-ignorable page), sufficient to check ordering invariant on last
 * item in current target page.  Returned scankey relies on local memory
 * allocated for the child page, which caller cannot pfree().  Caller's memory
 * context should be reset between calls here.
 *
 * This is the first data item, and so all adjacent items are checked against
 * their immediate sibling item (which may be on a sibling page, or even a
 * "cousin" page at parent boundaries where target's rightlink points to page
 * with different parent page).  If no such valid item is available, return
 * NULL instead.
 *
 * Note that !readonly callers must reverify that target page has not
 * been concurrently deleted.
 */
static ScanKey
bt_right_page_check_scankey(BtreeCheckState *state)
{
	BTPageOpaque opaque;
	ItemId		rightitem;
	BlockNumber targetnext;
	Page		rightpage;
	OffsetNumber nline;

	/* Determine target's next block number */
	opaque = (BTPageOpaque) PageGetSpecialPointer(state->target);

	/* If target is already rightmost, no right sibling; nothing to do here */
	if (P_RIGHTMOST(opaque))
		return NULL;

	/*
	 * General notes on concurrent page splits and page deletion:
	 *
	 * Routines like _bt_search() don't require *any* page split interlock
	 * when descending the tree, including something very light like a buffer
	 * pin. That's why it's okay that we don't either.  This avoidance of any
	 * need to "couple" buffer locks is the raison d' etre of the Lehman & Yao
	 * algorithm, in fact.
	 *
	 * That leaves deletion.  A deleted page won't actually be recycled by
	 * VACUUM early enough for us to fail to at least follow its right link
	 * (or left link, or downlink) and find its sibling, because recycling
	 * does not occur until no possible index scan could land on the page.
	 * Index scans can follow links with nothing more than their snapshot as
	 * an interlock and be sure of at least that much.  (See page
	 * recycling/RecentGlobalXmin notes in nbtree README.)
	 *
	 * Furthermore, it's okay if we follow a rightlink and find a half-dead or
	 * dead (ignorable) page one or more times.  There will either be a
	 * further right link to follow that leads to a live page before too long
	 * (before passing by parent's rightmost child), or we will find the end
	 * of the entire level instead (possible when parent page is itself the
	 * rightmost on its level).
	 */
	targetnext = opaque->btpo_next;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		rightpage = palloc_btree_page(state, targetnext);
		opaque = (BTPageOpaque) PageGetSpecialPointer(rightpage);

		if (!P_IGNORE(opaque) || P_RIGHTMOST(opaque))
			break;

		/* We landed on a deleted page, so step right to find a live page */
		targetnext = opaque->btpo_next;
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("level %u leftmost page of index \"%s\" was found deleted or half dead",
					opaque->btpo.level, RelationGetRelationName(state->rel)),
				 errdetail_internal("Deleted page found when building scankey from right sibling.")));

		/* Be slightly more pro-active in freeing this memory, just in case */
		pfree(rightpage);
	}

	/*
	 * No ShareLock held case -- why it's safe to proceed.
	 *
	 * Problem:
	 *
	 * We must avoid false positive reports of corruption when caller treats
	 * item returned here as an upper bound on target's last item.  In
	 * general, false positives are disallowed.  Avoiding them here when
	 * caller is !readonly is subtle.
	 *
	 * A concurrent page deletion by VACUUM of the target page can result in
	 * the insertion of items on to this right sibling page that would
	 * previously have been inserted on our target page.  There might have
	 * been insertions that followed the target's downlink after it was made
	 * to point to right sibling instead of target by page deletion's first
	 * phase. The inserters insert items that would belong on target page.
	 * This race is very tight, but it's possible.  This is our only problem.
	 *
	 * Non-problems:
	 *
	 * We are not hindered by a concurrent page split of the target; we'll
	 * never land on the second half of the page anyway.  A concurrent split
	 * of the right page will also not matter, because the first data item
	 * remains the same within the left half, which we'll reliably land on. If
	 * we had to skip over ignorable/deleted pages, it cannot matter because
	 * their key space has already been atomically merged with the first
	 * non-ignorable page we eventually find (doesn't matter whether the page
	 * we eventually find is a true sibling or a cousin of target, which we go
	 * into below).
	 *
	 * Solution:
	 *
	 * Caller knows that it should reverify that target is not ignorable
	 * (half-dead or deleted) when cross-page sibling item comparison appears
	 * to indicate corruption (invariant fails).  This detects the single race
	 * condition that exists for caller.  This is correct because the
	 * continued existence of target block as non-ignorable (not half-dead or
	 * deleted) implies that target page was not merged into from the right by
	 * deletion; the key space at or after target never moved left.  Target's
	 * parent either has the same downlink to target as before, or a <=
	 * downlink due to deletion at the left of target.  Target either has the
	 * same highkey as before, or a highkey <= before when there is a page
	 * split. (The rightmost concurrently-split-from-target-page page will
	 * still have the same highkey as target was originally found to have,
	 * which for our purposes is equivalent to target's highkey itself never
	 * changing, since we reliably skip over
	 * concurrently-split-from-target-page pages.)
	 *
	 * In simpler terms, we allow that the key space of the target may expand
	 * left (the key space can move left on the left side of target only), but
	 * the target key space cannot expand right and get ahead of us without
	 * our detecting it.  The key space of the target cannot shrink, unless it
	 * shrinks to zero due to the deletion of the original page, our canary
	 * condition.  (To be very precise, we're a bit stricter than that because
	 * it might just have been that the target page split and only the
	 * original target page was deleted.  We can be more strict, just not more
	 * lax.)
	 *
	 * Top level tree walk caller moves on to next page (makes it the new
	 * target) following recovery from this race.  (cf.  The rationale for
	 * child/downlink verification needing a ShareLock within
	 * bt_downlink_check(), where page deletion is also the main source of
	 * trouble.)
	 *
	 * Note that it doesn't matter if right sibling page here is actually a
	 * cousin page, because in order for the key space to be readjusted in a
	 * way that causes us issues in next level up (guiding problematic
	 * concurrent insertions to the cousin from the grandparent rather than to
	 * the sibling from the parent), there'd have to be page deletion of
	 * target's parent page (affecting target's parent's downlink in target's
	 * grandparent page).  Internal page deletion only occurs when there are
	 * no child pages (they were all fully deleted), and caller is checking
	 * that the target's parent has at least one non-deleted (so
	 * non-ignorable) child: the target page.  (Note that the first phase of
	 * deletion atomically marks the page to be deleted half-dead/ignorable at
	 * the same time downlink in its parent is removed, so caller will
	 * definitely not fail to detect that this happened.)
	 *
	 * This trick is inspired by the method backward scans use for dealing
	 * with concurrent page splits; concurrent page deletion is a problem that
	 * similarly receives special consideration sometimes (it's possible that
	 * the backwards scan will re-read its "original" block after failing to
	 * find a right-link to it, having already moved in the opposite direction
	 * (right/"forwards") a few times to try to locate one).  Just like us,
	 * that happens only to determine if there was a concurrent page deletion
	 * of a reference page, and just like us if there was a page deletion of
	 * that reference page it means we can move on from caring about the
	 * reference page.  See the nbtree README for a full description of how
	 * that works.
	 */
	nline = PageGetMaxOffsetNumber(rightpage);

	/*
	 * Get first data item, if any
	 */
	if (P_ISLEAF(opaque) && nline >= P_FIRSTDATAKEY(opaque))
	{
		/* Return first data item (if any) */
		rightitem = PageGetItemId(rightpage, P_FIRSTDATAKEY(opaque));
	}
	else if (!P_ISLEAF(opaque) &&
			 nline >= OffsetNumberNext(P_FIRSTDATAKEY(opaque)))
	{
		/*
		 * Return first item after the internal page's "negative infinity"
		 * item
		 */
		rightitem = PageGetItemId(rightpage,
								  OffsetNumberNext(P_FIRSTDATAKEY(opaque)));
	}
	else
	{
		/*
		 * No first item.  Page is probably empty leaf page, but it's also
		 * possible that it's an internal page with only a negative infinity
		 * item.
		 */
		ereport(DEBUG1,
				(errcode(ERRCODE_NO_DATA),
				 errmsg("%s block %u of index \"%s\" has no first data item",
						P_ISLEAF(opaque) ? "leaf" : "internal", targetnext,
						RelationGetRelationName(state->rel))));
		return NULL;
	}

	/*
	 * Return first real item scankey.  Note that this relies on right page
	 * memory remaining allocated.
	 */
	return _bt_mkscankey(state->rel,
						 (IndexTuple) PageGetItem(rightpage, rightitem));
}

/*
 * Checks one of target's downlink against its child page.
 *
 * Conceptually, the target page continues to be what is checked here.  The
 * target block is still blamed in the event of finding an invariant violation.
 * The downlink insertion into the target is probably where any problem raised
 * here arises, and there is no such thing as a parent link, so doing the
 * verification this way around is much more practical.
 */
static void
bt_downlink_check(BtreeCheckState *state, BlockNumber childblock,
				  ScanKey targetkey)
{
	OffsetNumber offset;
	OffsetNumber maxoffset;
	Page		child;
	BTPageOpaque copaque;

	/*
	 * Caller must have ShareLock on target relation, because of
	 * considerations around page deletion by VACUUM.
	 *
	 * NB: In general, page deletion deletes the right sibling's downlink, not
	 * the downlink of the page being deleted; the deleted page's downlink is
	 * reused for its sibling.  The key space is thereby consolidated between
	 * the deleted page and its right sibling.  (We cannot delete a parent
	 * page's rightmost child unless it is the last child page, and we intend
	 * to also delete the parent itself.)
	 *
	 * If this verification happened without a ShareLock, the following race
	 * condition could cause false positives:
	 *
	 * In general, concurrent page deletion might occur, including deletion of
	 * the left sibling of the child page that is examined here.  If such a
	 * page deletion were to occur, closely followed by an insertion into the
	 * newly expanded key space of the child, a window for the false positive
	 * opens up: the stale parent/target downlink originally followed to get
	 * to the child legitimately ceases to be a lower bound on all items in
	 * the page, since the key space was concurrently expanded "left".
	 * (Insertion followed the "new" downlink for the child, not our now-stale
	 * downlink, which was concurrently physically removed in target/parent as
	 * part of deletion's first phase.)
	 *
	 * Note that while the cross-page-same-level last item check uses a trick
	 * that allows it to perform verification for !readonly callers, a similar
	 * trick seems difficult here.  The trick that that other check uses is,
	 * in essence, to lock down race conditions to those that occur due to
	 * concurrent page deletion of the target; that's a race that can be
	 * reliably detected before actually reporting corruption.
	 *
	 * On the other hand, we'd need to lock down race conditions involving
	 * deletion of child's left page, for long enough to read the child page
	 * into memory (in other words, a scheme with concurrently held buffer
	 * locks on both child and left-of-child pages).  That's unacceptable for
	 * amcheck functions on general principle, though.
	 */
	Assert(state->readonly);

	/*
	 * Verify child page has the downlink key from target page (its parent) as
	 * a lower bound.
	 *
	 * Check all items, rather than checking just the first and trusting that
	 * the operator class obeys the transitive law.
	 */
	child = palloc_btree_page(state, childblock);
	copaque = (BTPageOpaque) PageGetSpecialPointer(child);
	maxoffset = PageGetMaxOffsetNumber(child);

	for (offset = P_FIRSTDATAKEY(copaque);
		 offset <= maxoffset;
		 offset = OffsetNumberNext(offset))
	{
		/*
		 * Skip comparison of target page key against "negative infinity"
		 * item, if any.  Checking it would indicate that it's not an upper
		 * bound, but that's only because of the hard-coding within
		 * _bt_compare().
		 */
		if (offset_is_negative_infinity(copaque, offset))
			continue;

		if (!invariant_leq_nontarget_offset(state, child,
											targetkey, offset))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("down-link lower bound invariant violated for index \"%s\"",
							RelationGetRelationName(state->rel)),
					 errdetail_internal("Parent block=%u child index tid=(%u,%u) parent page lsn=%X/%X.",
									  state->targetblock, childblock, offset,
										(uint32) (state->targetlsn >> 32),
										(uint32) state->targetlsn)));
	}

	pfree(child);
}

/*
 * Is particular offset within page (whose special state is passed by caller)
 * the page negative-infinity item?
 *
 * As noted in comments above _bt_compare(), there is special handling of the
 * first data item as a "negative infinity" item.  The hard-coding within
 * _bt_compare() makes comparing this item for the purposes of verification
 * pointless at best, since the IndexTuple only contains a valid TID (a
 * reference TID to child page).
 */
static inline bool
offset_is_negative_infinity(BTPageOpaque opaque, OffsetNumber offset)
{
	/*
	 * For internal pages only, the first item after high key, if any, is
	 * negative infinity item.  Internal pages always have a negative infinity
	 * item, whereas leaf pages never have one.  This implies that negative
	 * infinity item is either first or second line item, or there is none
	 * within page.
	 *
	 * Right-most pages don't have a high key, but could be said to
	 * conceptually have a "positive infinity" high key.  Thus, there is a
	 * symmetry between down link items in parent pages, and high keys in
	 * children.  Together, they represent the part of the key space that
	 * belongs to each page in the index.  For example, all children of the
	 * root page will have negative infinity as a lower bound from root
	 * negative infinity downlink, and positive infinity as an upper bound
	 * (implicitly, from "imaginary" positive infinity high key in root).
	 */
	return !P_ISLEAF(opaque) && offset == P_FIRSTDATAKEY(opaque);
}

/*
 * Does the invariant hold that the key is less than or equal to a given upper
 * bound offset item?
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_leq_offset(BtreeCheckState *state, ScanKey key,
					 OffsetNumber upperbound)
{
	int16		natts = state->rel->rd_rel->relnatts;
	int32		cmp;

	cmp = _bt_compare(state->rel, natts, key, state->target, upperbound);

	return cmp <= 0;
}

/*
 * Does the invariant hold that the key is greater than or equal to a given
 * lower bound offset item?
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_geq_offset(BtreeCheckState *state, ScanKey key,
					 OffsetNumber lowerbound)
{
	int16		natts = state->rel->rd_rel->relnatts;
	int32		cmp;

	cmp = _bt_compare(state->rel, natts, key, state->target, lowerbound);

	return cmp >= 0;
}

/*
 * Does the invariant hold that the key is less than or equal to a given upper
 * bound offset item, with the offset relating to a caller-supplied page that
 * is not the current target page? Caller's non-target page is typically a
 * child page of the target, checked as part of checking a property of the
 * target page (i.e. the key comes from the target).
 *
 * If this function returns false, convention is that caller throws error due
 * to corruption.
 */
static inline bool
invariant_leq_nontarget_offset(BtreeCheckState *state,
							   Page nontarget, ScanKey key,
							   OffsetNumber upperbound)
{
	int16		natts = state->rel->rd_rel->relnatts;
	int32		cmp;

	cmp = _bt_compare(state->rel, natts, key, nontarget, upperbound);

	return cmp <= 0;
}

/*
 * Given a block number of a B-Tree page, return page in palloc()'d memory.
 * While at it, perform some basic checks of the page.
 *
 * There is never an attempt to get a consistent view of multiple pages using
 * multiple concurrent buffer locks; in general, we only acquire a single pin
 * and buffer lock at a time, which is often all that the nbtree code requires.
 *
 * Operating on a copy of the page is useful because it prevents control
 * getting stuck in an uninterruptible state when an underlying operator class
 * misbehaves.
 */
static Page
palloc_btree_page(BtreeCheckState *state, BlockNumber blocknum)
{
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	page = palloc(BLCKSZ);

	/*
	 * We copy the page into local storage to avoid holding pin on the buffer
	 * longer than we must.
	 */
	buffer = ReadBufferExtended(state->rel, MAIN_FORKNUM, blocknum, RBM_NORMAL,
								state->checkstrategy);
	LockBuffer(buffer, BT_READ);

	/*
	 * Perform the same basic sanity checking that nbtree itself performs for
	 * every page:
	 */
	_bt_checkpage(state->rel, buffer);

	/* Only use copy of page in palloc()'d memory */
	memcpy(page, BufferGetPage(buffer), BLCKSZ);
	UnlockReleaseBuffer(buffer);

	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	if (opaque->btpo_flags & BTP_META && blocknum != BTREE_METAPAGE)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid meta page found at block %u in index \"%s\"",
						blocknum, RelationGetRelationName(state->rel))));

	/* Check page from block that ought to be meta page */
	if (blocknum == BTREE_METAPAGE)
	{
		BTMetaPageData *metad = BTPageGetMeta(page);

		if (!(opaque->btpo_flags & BTP_META) ||
			metad->btm_magic != BTREE_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" meta page is corrupt",
							RelationGetRelationName(state->rel))));

		if (metad->btm_version != BTREE_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("version mismatch in index \"%s\": file version %d, code version %d",
							RelationGetRelationName(state->rel),
							metad->btm_version, BTREE_VERSION)));
	}

	/*
	 * Deleted pages have no sane "level" field, so can only check non-deleted
	 * page level
	 */
	if (P_ISLEAF(opaque) && !P_ISDELETED(opaque) && opaque->btpo.level != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
			errmsg("invalid leaf page level %u for block %u in index \"%s\"",
		opaque->btpo.level, blocknum, RelationGetRelationName(state->rel))));

	if (blocknum != BTREE_METAPAGE && !P_ISLEAF(opaque) &&
		!P_ISDELETED(opaque) && opaque->btpo.level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
		 errmsg("invalid internal page level 0 for block %u in index \"%s\"",
				opaque->btpo.level, RelationGetRelationName(state->rel))));

	if (!P_ISLEAF(opaque) && P_HAS_GARBAGE(opaque))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
		   errmsg("internal page block %u in index \"%s\" has garbage items",
				  blocknum, RelationGetRelationName(state->rel))));

	return page;
}
