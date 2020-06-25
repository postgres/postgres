/*-------------------------------------------------------------------------
 *
 * nbtinsert.c
 *	  Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"

/* Minimum tree height for application of fastpath optimization */
#define BTREE_FASTPATH_MIN_LEVEL	2


static BTStack _bt_search_insert(Relation rel, BTInsertState insertstate);
static TransactionId _bt_check_unique(Relation rel, BTInsertState insertstate,
									  Relation heapRel,
									  IndexUniqueCheck checkUnique, bool *is_unique,
									  uint32 *speculativeToken);
static OffsetNumber _bt_findinsertloc(Relation rel,
									  BTInsertState insertstate,
									  bool checkingunique,
									  BTStack stack,
									  Relation heapRel);
static void _bt_stepright(Relation rel, BTInsertState insertstate, BTStack stack);
static void _bt_insertonpg(Relation rel, BTScanInsert itup_key,
						   Buffer buf,
						   Buffer cbuf,
						   BTStack stack,
						   IndexTuple itup,
						   Size itemsz,
						   OffsetNumber newitemoff,
						   int postingoff,
						   bool split_only_page);
static Buffer _bt_split(Relation rel, BTScanInsert itup_key, Buffer buf,
						Buffer cbuf, OffsetNumber newitemoff, Size newitemsz,
						IndexTuple newitem, IndexTuple orignewitem,
						IndexTuple nposting, uint16 postingoff);
static void _bt_insert_parent(Relation rel, Buffer buf, Buffer rbuf,
							  BTStack stack, bool is_root, bool is_only);
static Buffer _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);
static inline bool _bt_pgaddtup(Page page, Size itemsize, IndexTuple itup,
								OffsetNumber itup_off, bool newfirstdataitem);
static void _bt_vacuum_one_page(Relation rel, Buffer buffer, Relation heapRel);

/*
 *	_bt_doinsert() -- Handle insertion of a single index tuple in the tree.
 *
 *		This routine is called by the public interface routine, btinsert.
 *		By here, itup is filled in, including the TID.
 *
 *		If checkUnique is UNIQUE_CHECK_NO or UNIQUE_CHECK_PARTIAL, this
 *		will allow duplicates.  Otherwise (UNIQUE_CHECK_YES or
 *		UNIQUE_CHECK_EXISTING) it will throw error for a duplicate.
 *		For UNIQUE_CHECK_EXISTING we merely run the duplicate check, and
 *		don't actually insert.
 *
 *		The result value is only significant for UNIQUE_CHECK_PARTIAL:
 *		it must be true if the entry is known unique, else false.
 *		(In the current implementation we'll also return true after a
 *		successful UNIQUE_CHECK_YES or UNIQUE_CHECK_EXISTING call, but
 *		that's just a coding artifact.)
 */
bool
_bt_doinsert(Relation rel, IndexTuple itup,
			 IndexUniqueCheck checkUnique, Relation heapRel)
{
	bool		is_unique = false;
	BTInsertStateData insertstate;
	BTScanInsert itup_key;
	BTStack		stack;
	bool		checkingunique = (checkUnique != UNIQUE_CHECK_NO);

	/* we need an insertion scan key to do our search, so build one */
	itup_key = _bt_mkscankey(rel, itup);

	if (checkingunique)
	{
		if (!itup_key->anynullkeys)
		{
			/* No (heapkeyspace) scantid until uniqueness established */
			itup_key->scantid = NULL;
		}
		else
		{
			/*
			 * Scan key for new tuple contains NULL key values.  Bypass
			 * checkingunique steps.  They are unnecessary because core code
			 * considers NULL unequal to every value, including NULL.
			 *
			 * This optimization avoids O(N^2) behavior within the
			 * _bt_findinsertloc() heapkeyspace path when a unique index has a
			 * large number of "duplicates" with NULL key values.
			 */
			checkingunique = false;
			/* Tuple is unique in the sense that core code cares about */
			Assert(checkUnique != UNIQUE_CHECK_EXISTING);
			is_unique = true;
		}
	}

	/*
	 * Fill in the BTInsertState working area, to track the current page and
	 * position within the page to insert on.
	 *
	 * Note that itemsz is passed down to lower level code that deals with
	 * inserting the item.  It must be MAXALIGN()'d.  This ensures that space
	 * accounting code consistently considers the alignment overhead that we
	 * expect PageAddItem() will add later.  (Actually, index_form_tuple() is
	 * already conservative about alignment, but we don't rely on that from
	 * this distance.  Besides, preserving the "true" tuple size in index
	 * tuple headers for the benefit of nbtsplitloc.c might happen someday.
	 * Note that heapam does not MAXALIGN() each heap tuple's lp_len field.)
	 */
	insertstate.itup = itup;
	insertstate.itemsz = MAXALIGN(IndexTupleSize(itup));
	insertstate.itup_key = itup_key;
	insertstate.bounds_valid = false;
	insertstate.buf = InvalidBuffer;
	insertstate.postingoff = 0;

search:

	/*
	 * Find and lock the leaf page that the tuple should be added to by
	 * searching from the root page.  insertstate.buf will hold a buffer that
	 * is locked in exclusive mode afterwards.
	 */
	stack = _bt_search_insert(rel, &insertstate);

	/*
	 * checkingunique inserts are not allowed to go ahead when two tuples with
	 * equal key attribute values would be visible to new MVCC snapshots once
	 * the xact commits.  Check for conflicts in the locked page/buffer (if
	 * needed) here.
	 *
	 * It might be necessary to check a page to the right in _bt_check_unique,
	 * though that should be very rare.  In practice the first page the value
	 * could be on (with scantid omitted) is almost always also the only page
	 * that a matching tuple might be found on.  This is due to the behavior
	 * of _bt_findsplitloc with duplicate tuples -- a group of duplicates can
	 * only be allowed to cross a page boundary when there is no candidate
	 * leaf page split point that avoids it.  Also, _bt_check_unique can use
	 * the leaf page high key to determine that there will be no duplicates on
	 * the right sibling without actually visiting it (it uses the high key in
	 * cases where the new item happens to belong at the far right of the leaf
	 * page).
	 *
	 * NOTE: obviously, _bt_check_unique can only detect keys that are already
	 * in the index; so it cannot defend against concurrent insertions of the
	 * same key.  We protect against that by means of holding a write lock on
	 * the first page the value could be on, with omitted/-inf value for the
	 * implicit heap TID tiebreaker attribute.  Any other would-be inserter of
	 * the same key must acquire a write lock on the same page, so only one
	 * would-be inserter can be making the check at one time.  Furthermore,
	 * once we are past the check we hold write locks continuously until we
	 * have performed our insertion, so no later inserter can fail to see our
	 * insertion.  (This requires some care in _bt_findinsertloc.)
	 *
	 * If we must wait for another xact, we release the lock while waiting,
	 * and then must perform a new search.
	 *
	 * For a partial uniqueness check, we don't wait for the other xact. Just
	 * let the tuple in and return false for possibly non-unique, or true for
	 * definitely unique.
	 */
	if (checkingunique)
	{
		TransactionId xwait;
		uint32		speculativeToken;

		xwait = _bt_check_unique(rel, &insertstate, heapRel, checkUnique,
								 &is_unique, &speculativeToken);

		if (unlikely(TransactionIdIsValid(xwait)))
		{
			/* Have to wait for the other guy ... */
			_bt_relbuf(rel, insertstate.buf);
			insertstate.buf = InvalidBuffer;

			/*
			 * If it's a speculative insertion, wait for it to finish (ie. to
			 * go ahead with the insertion, or kill the tuple).  Otherwise
			 * wait for the transaction to finish as usual.
			 */
			if (speculativeToken)
				SpeculativeInsertionWait(xwait, speculativeToken);
			else
				XactLockTableWait(xwait, rel, &itup->t_tid, XLTW_InsertIndex);

			/* start over... */
			if (stack)
				_bt_freestack(stack);
			goto search;
		}

		/* Uniqueness is established -- restore heap tid as scantid */
		if (itup_key->heapkeyspace)
			itup_key->scantid = &itup->t_tid;
	}

	if (checkUnique != UNIQUE_CHECK_EXISTING)
	{
		OffsetNumber newitemoff;

		/*
		 * The only conflict predicate locking cares about for indexes is when
		 * an index tuple insert conflicts with an existing lock.  We don't
		 * know the actual page we're going to insert on for sure just yet in
		 * checkingunique and !heapkeyspace cases, but it's okay to use the
		 * first page the value could be on (with scantid omitted) instead.
		 */
		CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(insertstate.buf));

		/*
		 * Do the insertion.  Note that insertstate contains cached binary
		 * search bounds established within _bt_check_unique when insertion is
		 * checkingunique.
		 */
		newitemoff = _bt_findinsertloc(rel, &insertstate, checkingunique,
									   stack, heapRel);
		_bt_insertonpg(rel, itup_key, insertstate.buf, InvalidBuffer, stack,
					   itup, insertstate.itemsz, newitemoff,
					   insertstate.postingoff, false);
	}
	else
	{
		/* just release the buffer */
		_bt_relbuf(rel, insertstate.buf);
	}

	/* be tidy */
	if (stack)
		_bt_freestack(stack);
	pfree(itup_key);

	return is_unique;
}

/*
 *	_bt_search_insert() -- _bt_search() wrapper for inserts
 *
 * Search the tree for a particular scankey, or more precisely for the first
 * leaf page it could be on.  Try to make use of the fastpath optimization's
 * rightmost leaf page cache before actually searching the tree from the root
 * page, though.
 *
 * Return value is a stack of parent-page pointers (though see notes about
 * fastpath optimization and page splits below).  insertstate->buf is set to
 * the address of the leaf-page buffer, which is write-locked and pinned in
 * all cases (if necessary by creating a new empty root page for caller).
 *
 * The fastpath optimization avoids most of the work of searching the tree
 * repeatedly when a single backend inserts successive new tuples on the
 * rightmost leaf page of an index.  A backend cache of the rightmost leaf
 * page is maintained within _bt_insertonpg(), and used here.  The cache is
 * invalidated here when an insert of a non-pivot tuple must take place on a
 * non-rightmost leaf page.
 *
 * The optimization helps with indexes on an auto-incremented field.  It also
 * helps with indexes on datetime columns, as well as indexes with lots of
 * NULL values.  (NULLs usually get inserted in the rightmost page for single
 * column indexes, since they usually get treated as coming after everything
 * else in the key space.  Individual NULL tuples will generally be placed on
 * the rightmost leaf page due to the influence of the heap TID column.)
 *
 * Note that we avoid applying the optimization when there is insufficient
 * space on the rightmost page to fit caller's new item.  This is necessary
 * because we'll need to return a real descent stack when a page split is
 * expected (actually, caller can cope with a leaf page split that uses a NULL
 * stack, but that's very slow and so must be avoided).  Note also that the
 * fastpath optimization acquires the lock on the page conditionally as a way
 * of reducing extra contention when there are concurrent insertions into the
 * rightmost page (we give up if we'd have to wait for the lock).  We assume
 * that it isn't useful to apply the optimization when there is contention,
 * since each per-backend cache won't stay valid for long.
 */
static BTStack
_bt_search_insert(Relation rel, BTInsertState insertstate)
{
	Assert(insertstate->buf == InvalidBuffer);
	Assert(!insertstate->bounds_valid);
	Assert(insertstate->postingoff == 0);

	if (RelationGetTargetBlock(rel) != InvalidBlockNumber)
	{
		/* Simulate a _bt_getbuf() call with conditional locking */
		insertstate->buf = ReadBuffer(rel, RelationGetTargetBlock(rel));
		if (ConditionalLockBuffer(insertstate->buf))
		{
			Page		page;
			BTPageOpaque lpageop;

			_bt_checkpage(rel, insertstate->buf);
			page = BufferGetPage(insertstate->buf);
			lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

			/*
			 * Check if the page is still the rightmost leaf page and has
			 * enough free space to accommodate the new tuple.  Also check
			 * that the insertion scan key is strictly greater than the first
			 * non-pivot tuple on the page.  (Note that we expect itup_key's
			 * scantid to be unset when our caller is a checkingunique
			 * inserter.)
			 */
			if (P_RIGHTMOST(lpageop) &&
				P_ISLEAF(lpageop) &&
				!P_IGNORE(lpageop) &&
				PageGetFreeSpace(page) > insertstate->itemsz &&
				PageGetMaxOffsetNumber(page) >= P_HIKEY &&
				_bt_compare(rel, insertstate->itup_key, page, P_HIKEY) > 0)
			{
				/*
				 * Caller can use the fastpath optimization because cached
				 * block is still rightmost leaf page, which can fit caller's
				 * new tuple without splitting.  Keep block in local cache for
				 * next insert, and have caller use NULL stack.
				 *
				 * Note that _bt_insert_parent() has an assertion that catches
				 * leaf page splits that somehow follow from a fastpath insert
				 * (it should only be passed a NULL stack when it must deal
				 * with a concurrent root page split, and never because a NULL
				 * stack was returned here).
				 */
				return NULL;
			}

			/* Page unsuitable for caller, drop lock and pin */
			_bt_relbuf(rel, insertstate->buf);
		}
		else
		{
			/* Lock unavailable, drop pin */
			ReleaseBuffer(insertstate->buf);
		}

		/* Forget block, since cache doesn't appear to be useful */
		RelationSetTargetBlock(rel, InvalidBlockNumber);
	}

	/* Cannot use optimization -- descend tree, return proper descent stack */
	return _bt_search(rel, insertstate->itup_key, &insertstate->buf, BT_WRITE,
					  NULL);
}

/*
 *	_bt_check_unique() -- Check for violation of unique index constraint
 *
 * Returns InvalidTransactionId if there is no conflict, else an xact ID
 * we must wait for to see if it commits a conflicting tuple.   If an actual
 * conflict is detected, no return --- just ereport().  If an xact ID is
 * returned, and the conflicting tuple still has a speculative insertion in
 * progress, *speculativeToken is set to non-zero, and the caller can wait for
 * the verdict on the insertion using SpeculativeInsertionWait().
 *
 * However, if checkUnique == UNIQUE_CHECK_PARTIAL, we always return
 * InvalidTransactionId because we don't want to wait.  In this case we
 * set *is_unique to false if there is a potential conflict, and the
 * core code must redo the uniqueness check later.
 *
 * As a side-effect, sets state in insertstate that can later be used by
 * _bt_findinsertloc() to reuse most of the binary search work we do
 * here.
 *
 * Do not call here when there are NULL values in scan key.  NULL should be
 * considered unequal to NULL when checking for duplicates, but we are not
 * prepared to handle that correctly.
 */
static TransactionId
_bt_check_unique(Relation rel, BTInsertState insertstate, Relation heapRel,
				 IndexUniqueCheck checkUnique, bool *is_unique,
				 uint32 *speculativeToken)
{
	IndexTuple	itup = insertstate->itup;
	IndexTuple	curitup = NULL;
	ItemId		curitemid;
	BTScanInsert itup_key = insertstate->itup_key;
	SnapshotData SnapshotDirty;
	OffsetNumber offset;
	OffsetNumber maxoff;
	Page		page;
	BTPageOpaque opaque;
	Buffer		nbuf = InvalidBuffer;
	bool		found = false;
	bool		inposting = false;
	bool		prevalldead = true;
	int			curposti = 0;

	/* Assume unique until we find a duplicate */
	*is_unique = true;

	InitDirtySnapshot(SnapshotDirty);

	page = BufferGetPage(insertstate->buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Find the first tuple with the same key.
	 *
	 * This also saves the binary search bounds in insertstate.  We use them
	 * in the fastpath below, but also in the _bt_findinsertloc() call later.
	 */
	Assert(!insertstate->bounds_valid);
	offset = _bt_binsrch_insert(rel, insertstate);

	/*
	 * Scan over all equal tuples, looking for live conflicts.
	 */
	Assert(!insertstate->bounds_valid || insertstate->low == offset);
	Assert(!itup_key->anynullkeys);
	Assert(itup_key->scantid == NULL);
	for (;;)
	{
		/*
		 * Each iteration of the loop processes one heap TID, not one index
		 * tuple.  Current offset number for page isn't usually advanced on
		 * iterations that process heap TIDs from posting list tuples.
		 *
		 * "inposting" state is set when _inside_ a posting list --- not when
		 * we're at the start (or end) of a posting list.  We advance curposti
		 * at the end of the iteration when inside a posting list tuple.  In
		 * general, every loop iteration either advances the page offset or
		 * advances curposti --- an iteration that handles the rightmost/max
		 * heap TID in a posting list finally advances the page offset (and
		 * unsets "inposting").
		 *
		 * Make sure the offset points to an actual index tuple before trying
		 * to examine it...
		 */
		if (offset <= maxoff)
		{
			/*
			 * Fastpath: In most cases, we can use cached search bounds to
			 * limit our consideration to items that are definitely
			 * duplicates.  This fastpath doesn't apply when the original page
			 * is empty, or when initial offset is past the end of the
			 * original page, which may indicate that we need to examine a
			 * second or subsequent page.
			 *
			 * Note that this optimization allows us to avoid calling
			 * _bt_compare() directly when there are no duplicates, as long as
			 * the offset where the key will go is not at the end of the page.
			 */
			if (nbuf == InvalidBuffer && offset == insertstate->stricthigh)
			{
				Assert(insertstate->bounds_valid);
				Assert(insertstate->low >= P_FIRSTDATAKEY(opaque));
				Assert(insertstate->low <= insertstate->stricthigh);
				Assert(_bt_compare(rel, itup_key, page, offset) < 0);
				break;
			}

			/*
			 * We can skip items that are already marked killed.
			 *
			 * In the presence of heavy update activity an index may contain
			 * many killed items with the same key; running _bt_compare() on
			 * each killed item gets expensive.  Just advance over killed
			 * items as quickly as we can.  We only apply _bt_compare() when
			 * we get to a non-killed item.  We could reuse the bounds to
			 * avoid _bt_compare() calls for known equal tuples, but it
			 * doesn't seem worth it.  Workloads with heavy update activity
			 * tend to have many deduplication passes, so we'll often avoid
			 * most of those comparisons, too (we call _bt_compare() when the
			 * posting list tuple is initially encountered, though not when
			 * processing later TIDs from the same tuple).
			 */
			if (!inposting)
				curitemid = PageGetItemId(page, offset);
			if (inposting || !ItemIdIsDead(curitemid))
			{
				ItemPointerData htid;
				bool		all_dead = false;

				if (!inposting)
				{
					/* Plain tuple, or first TID in posting list tuple */
					if (_bt_compare(rel, itup_key, page, offset) != 0)
						break;	/* we're past all the equal tuples */

					/* Advanced curitup */
					curitup = (IndexTuple) PageGetItem(page, curitemid);
					Assert(!BTreeTupleIsPivot(curitup));
				}

				/* okay, we gotta fetch the heap tuple using htid ... */
				if (!BTreeTupleIsPosting(curitup))
				{
					/* ... htid is from simple non-pivot tuple */
					Assert(!inposting);
					htid = curitup->t_tid;
				}
				else if (!inposting)
				{
					/* ... htid is first TID in new posting list */
					inposting = true;
					prevalldead = true;
					curposti = 0;
					htid = *BTreeTupleGetPostingN(curitup, 0);
				}
				else
				{
					/* ... htid is second or subsequent TID in posting list */
					Assert(curposti > 0);
					htid = *BTreeTupleGetPostingN(curitup, curposti);
				}

				/*
				 * If we are doing a recheck, we expect to find the tuple we
				 * are rechecking.  It's not a duplicate, but we have to keep
				 * scanning.
				 */
				if (checkUnique == UNIQUE_CHECK_EXISTING &&
					ItemPointerCompare(&htid, &itup->t_tid) == 0)
				{
					found = true;
				}

				/*
				 * Check if there's any table tuples for this index entry
				 * satisfying SnapshotDirty. This is necessary because for AMs
				 * with optimizations like heap's HOT, we have just a single
				 * index entry for the entire chain.
				 */
				else if (table_index_fetch_tuple_check(heapRel, &htid,
													   &SnapshotDirty,
													   &all_dead))
				{
					TransactionId xwait;

					/*
					 * It is a duplicate. If we are only doing a partial
					 * check, then don't bother checking if the tuple is being
					 * updated in another transaction. Just return the fact
					 * that it is a potential conflict and leave the full
					 * check till later. Don't invalidate binary search
					 * bounds.
					 */
					if (checkUnique == UNIQUE_CHECK_PARTIAL)
					{
						if (nbuf != InvalidBuffer)
							_bt_relbuf(rel, nbuf);
						*is_unique = false;
						return InvalidTransactionId;
					}

					/*
					 * If this tuple is being updated by other transaction
					 * then we have to wait for its commit/abort.
					 */
					xwait = (TransactionIdIsValid(SnapshotDirty.xmin)) ?
						SnapshotDirty.xmin : SnapshotDirty.xmax;

					if (TransactionIdIsValid(xwait))
					{
						if (nbuf != InvalidBuffer)
							_bt_relbuf(rel, nbuf);
						/* Tell _bt_doinsert to wait... */
						*speculativeToken = SnapshotDirty.speculativeToken;
						/* Caller releases lock on buf immediately */
						insertstate->bounds_valid = false;
						return xwait;
					}

					/*
					 * Otherwise we have a definite conflict.  But before
					 * complaining, look to see if the tuple we want to insert
					 * is itself now committed dead --- if so, don't complain.
					 * This is a waste of time in normal scenarios but we must
					 * do it to support CREATE INDEX CONCURRENTLY.
					 *
					 * We must follow HOT-chains here because during
					 * concurrent index build, we insert the root TID though
					 * the actual tuple may be somewhere in the HOT-chain.
					 * While following the chain we might not stop at the
					 * exact tuple which triggered the insert, but that's OK
					 * because if we find a live tuple anywhere in this chain,
					 * we have a unique key conflict.  The other live tuple is
					 * not part of this chain because it had a different index
					 * entry.
					 */
					htid = itup->t_tid;
					if (table_index_fetch_tuple_check(heapRel, &htid,
													  SnapshotSelf, NULL))
					{
						/* Normal case --- it's still live */
					}
					else
					{
						/*
						 * It's been deleted, so no error, and no need to
						 * continue searching
						 */
						break;
					}

					/*
					 * Check for a conflict-in as we would if we were going to
					 * write to this page.  We aren't actually going to write,
					 * but we want a chance to report SSI conflicts that would
					 * otherwise be masked by this unique constraint
					 * violation.
					 */
					CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(insertstate->buf));

					/*
					 * This is a definite conflict.  Break the tuple down into
					 * datums and report the error.  But first, make sure we
					 * release the buffer locks we're holding ---
					 * BuildIndexValueDescription could make catalog accesses,
					 * which in the worst case might touch this same index and
					 * cause deadlocks.
					 */
					if (nbuf != InvalidBuffer)
						_bt_relbuf(rel, nbuf);
					_bt_relbuf(rel, insertstate->buf);
					insertstate->buf = InvalidBuffer;
					insertstate->bounds_valid = false;

					{
						Datum		values[INDEX_MAX_KEYS];
						bool		isnull[INDEX_MAX_KEYS];
						char	   *key_desc;

						index_deform_tuple(itup, RelationGetDescr(rel),
										   values, isnull);

						key_desc = BuildIndexValueDescription(rel, values,
															  isnull);

						ereport(ERROR,
								(errcode(ERRCODE_UNIQUE_VIOLATION),
								 errmsg("duplicate key value violates unique constraint \"%s\"",
										RelationGetRelationName(rel)),
								 key_desc ? errdetail("Key %s already exists.",
													  key_desc) : 0,
								 errtableconstraint(heapRel,
													RelationGetRelationName(rel))));
					}
				}
				else if (all_dead && (!inposting ||
									  (prevalldead &&
									   curposti == BTreeTupleGetNPosting(curitup) - 1)))
				{
					/*
					 * The conflicting tuple (or all HOT chains pointed to by
					 * all posting list TIDs) is dead to everyone, so mark the
					 * index entry killed.
					 */
					ItemIdMarkDead(curitemid);
					opaque->btpo_flags |= BTP_HAS_GARBAGE;

					/*
					 * Mark buffer with a dirty hint, since state is not
					 * crucial. Be sure to mark the proper buffer dirty.
					 */
					if (nbuf != InvalidBuffer)
						MarkBufferDirtyHint(nbuf, true);
					else
						MarkBufferDirtyHint(insertstate->buf, true);
				}

				/*
				 * Remember if posting list tuple has even a single HOT chain
				 * whose members are not all dead
				 */
				if (!all_dead && inposting)
					prevalldead = false;
			}
		}

		if (inposting && curposti < BTreeTupleGetNPosting(curitup) - 1)
		{
			/* Advance to next TID in same posting list */
			curposti++;
			continue;
		}
		else if (offset < maxoff)
		{
			/* Advance to next tuple */
			curposti = 0;
			inposting = false;
			offset = OffsetNumberNext(offset);
		}
		else
		{
			int			highkeycmp;

			/* If scankey == hikey we gotta check the next page too */
			if (P_RIGHTMOST(opaque))
				break;
			highkeycmp = _bt_compare(rel, itup_key, page, P_HIKEY);
			Assert(highkeycmp <= 0);
			if (highkeycmp != 0)
				break;
			/* Advance to next non-dead page --- there must be one */
			for (;;)
			{
				BlockNumber nblkno = opaque->btpo_next;

				nbuf = _bt_relandgetbuf(rel, nbuf, nblkno, BT_READ);
				page = BufferGetPage(nbuf);
				opaque = (BTPageOpaque) PageGetSpecialPointer(page);
				if (!P_IGNORE(opaque))
					break;
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of index \"%s\"",
						 RelationGetRelationName(rel));
			}
			/* Will also advance to next tuple */
			curposti = 0;
			inposting = false;
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
			/* Don't invalidate binary search bounds */
		}
	}

	/*
	 * If we are doing a recheck then we should have found the tuple we are
	 * checking.  Otherwise there's something very wrong --- probably, the
	 * index is on a non-immutable expression.
	 */
	if (checkUnique == UNIQUE_CHECK_EXISTING && !found)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to re-find tuple within index \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("This may be because of a non-immutable index expression."),
				 errtableconstraint(heapRel,
									RelationGetRelationName(rel))));

	if (nbuf != InvalidBuffer)
		_bt_relbuf(rel, nbuf);

	return InvalidTransactionId;
}


/*
 *	_bt_findinsertloc() -- Finds an insert location for a tuple
 *
 *		On entry, insertstate buffer contains the page the new tuple belongs
 *		on.  It is exclusive-locked and pinned by the caller.
 *
 *		If 'checkingunique' is true, the buffer on entry is the first page
 *		that contains duplicates of the new key.  If there are duplicates on
 *		multiple pages, the correct insertion position might be some page to
 *		the right, rather than the first page.  In that case, this function
 *		moves right to the correct target page.
 *
 *		(In a !heapkeyspace index, there can be multiple pages with the same
 *		high key, where the new tuple could legitimately be placed on.  In
 *		that case, the caller passes the first page containing duplicates,
 *		just like when checkingunique=true.  If that page doesn't have enough
 *		room for the new tuple, this function moves right, trying to find a
 *		legal page that does.)
 *
 *		On exit, insertstate buffer contains the chosen insertion page, and
 *		the offset within that page is returned.  If _bt_findinsertloc needed
 *		to move right, the lock and pin on the original page are released, and
 *		the new buffer is exclusively locked and pinned instead.
 *
 *		If insertstate contains cached binary search bounds, we will take
 *		advantage of them.  This avoids repeating comparisons that we made in
 *		_bt_check_unique() already.
 *
 *		If there is not enough room on the page for the new tuple, we try to
 *		make room by removing any LP_DEAD tuples.
 */
static OffsetNumber
_bt_findinsertloc(Relation rel,
				  BTInsertState insertstate,
				  bool checkingunique,
				  BTStack stack,
				  Relation heapRel)
{
	BTScanInsert itup_key = insertstate->itup_key;
	Page		page = BufferGetPage(insertstate->buf);
	BTPageOpaque lpageop;
	OffsetNumber newitemoff;

	lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

	/* Check 1/3 of a page restriction */
	if (unlikely(insertstate->itemsz > BTMaxItemSize(page)))
		_bt_check_third_page(rel, heapRel, itup_key->heapkeyspace, page,
							 insertstate->itup);

	Assert(P_ISLEAF(lpageop) && !P_INCOMPLETE_SPLIT(lpageop));
	Assert(!insertstate->bounds_valid || checkingunique);
	Assert(!itup_key->heapkeyspace || itup_key->scantid != NULL);
	Assert(itup_key->heapkeyspace || itup_key->scantid == NULL);
	Assert(!itup_key->allequalimage || itup_key->heapkeyspace);

	if (itup_key->heapkeyspace)
	{
		/* Keep track of whether checkingunique duplicate seen */
		bool		uniquedup = false;

		/*
		 * If we're inserting into a unique index, we may have to walk right
		 * through leaf pages to find the one leaf page that we must insert on
		 * to.
		 *
		 * This is needed for checkingunique callers because a scantid was not
		 * used when we called _bt_search().  scantid can only be set after
		 * _bt_check_unique() has checked for duplicates.  The buffer
		 * initially stored in insertstate->buf has the page where the first
		 * duplicate key might be found, which isn't always the page that new
		 * tuple belongs on.  The heap TID attribute for new tuple (scantid)
		 * could force us to insert on a sibling page, though that should be
		 * very rare in practice.
		 */
		if (checkingunique)
		{
			if (insertstate->low < insertstate->stricthigh)
			{
				/* Encountered a duplicate in _bt_check_unique() */
				Assert(insertstate->bounds_valid);
				uniquedup = true;
			}

			for (;;)
			{
				/*
				 * Does the new tuple belong on this page?
				 *
				 * The earlier _bt_check_unique() call may well have
				 * established a strict upper bound on the offset for the new
				 * item.  If it's not the last item of the page (i.e. if there
				 * is at least one tuple on the page that goes after the tuple
				 * we're inserting) then we know that the tuple belongs on
				 * this page.  We can skip the high key check.
				 */
				if (insertstate->bounds_valid &&
					insertstate->low <= insertstate->stricthigh &&
					insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
					break;

				/* Test '<=', not '!=', since scantid is set now */
				if (P_RIGHTMOST(lpageop) ||
					_bt_compare(rel, itup_key, page, P_HIKEY) <= 0)
					break;

				_bt_stepright(rel, insertstate, stack);
				/* Update local state after stepping right */
				page = BufferGetPage(insertstate->buf);
				lpageop = (BTPageOpaque) PageGetSpecialPointer(page);
				/* Assume duplicates (if checkingunique) */
				uniquedup = true;
			}
		}

		/*
		 * If the target page is full, see if we can obtain enough space by
		 * erasing LP_DEAD items.  If that fails to free enough space, see if
		 * we can avoid a page split by performing a deduplication pass over
		 * the page.
		 *
		 * We only perform a deduplication pass for a checkingunique caller
		 * when the incoming item is a duplicate of an existing item on the
		 * leaf page.  This heuristic avoids wasting cycles -- we only expect
		 * to benefit from deduplicating a unique index page when most or all
		 * recently added items are duplicates.  See nbtree/README.
		 */
		if (PageGetFreeSpace(page) < insertstate->itemsz)
		{
			if (P_HAS_GARBAGE(lpageop))
			{
				_bt_vacuum_one_page(rel, insertstate->buf, heapRel);
				insertstate->bounds_valid = false;

				/* Might as well assume duplicates (if checkingunique) */
				uniquedup = true;
			}

			if (itup_key->allequalimage && BTGetDeduplicateItems(rel) &&
				(!checkingunique || uniquedup) &&
				PageGetFreeSpace(page) < insertstate->itemsz)
			{
				_bt_dedup_one_page(rel, insertstate->buf, heapRel,
								   insertstate->itup, insertstate->itemsz,
								   checkingunique);
				insertstate->bounds_valid = false;
			}
		}
	}
	else
	{
		/*----------
		 * This is a !heapkeyspace (version 2 or 3) index.  The current page
		 * is the first page that we could insert the new tuple to, but there
		 * may be other pages to the right that we could opt to use instead.
		 *
		 * If the new key is equal to one or more existing keys, we can
		 * legitimately place it anywhere in the series of equal keys.  In
		 * fact, if the new key is equal to the page's "high key" we can place
		 * it on the next page.  If it is equal to the high key, and there's
		 * not room to insert the new tuple on the current page without
		 * splitting, then we move right hoping to find more free space and
		 * avoid a split.
		 *
		 * Keep scanning right until we
		 *		(a) find a page with enough free space,
		 *		(b) reach the last page where the tuple can legally go, or
		 *		(c) get tired of searching.
		 * (c) is not flippant; it is important because if there are many
		 * pages' worth of equal keys, it's better to split one of the early
		 * pages than to scan all the way to the end of the run of equal keys
		 * on every insert.  We implement "get tired" as a random choice,
		 * since stopping after scanning a fixed number of pages wouldn't work
		 * well (we'd never reach the right-hand side of previously split
		 * pages).  The probability of moving right is set at 0.99, which may
		 * seem too high to change the behavior much, but it does an excellent
		 * job of preventing O(N^2) behavior with many equal keys.
		 *----------
		 */
		while (PageGetFreeSpace(page) < insertstate->itemsz)
		{
			/*
			 * Before considering moving right, see if we can obtain enough
			 * space by erasing LP_DEAD items
			 */
			if (P_HAS_GARBAGE(lpageop))
			{
				_bt_vacuum_one_page(rel, insertstate->buf, heapRel);
				insertstate->bounds_valid = false;

				if (PageGetFreeSpace(page) >= insertstate->itemsz)
					break;		/* OK, now we have enough space */
			}

			/*
			 * Nope, so check conditions (b) and (c) enumerated above
			 *
			 * The earlier _bt_check_unique() call may well have established a
			 * strict upper bound on the offset for the new item.  If it's not
			 * the last item of the page (i.e. if there is at least one tuple
			 * on the page that's greater than the tuple we're inserting to)
			 * then we know that the tuple belongs on this page.  We can skip
			 * the high key check.
			 */
			if (insertstate->bounds_valid &&
				insertstate->low <= insertstate->stricthigh &&
				insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
				break;

			if (P_RIGHTMOST(lpageop) ||
				_bt_compare(rel, itup_key, page, P_HIKEY) != 0 ||
				random() <= (MAX_RANDOM_VALUE / 100))
				break;

			_bt_stepright(rel, insertstate, stack);
			/* Update local state after stepping right */
			page = BufferGetPage(insertstate->buf);
			lpageop = (BTPageOpaque) PageGetSpecialPointer(page);
		}
	}

	/*
	 * We should now be on the correct page.  Find the offset within the page
	 * for the new tuple. (Possibly reusing earlier search bounds.)
	 */
	Assert(P_RIGHTMOST(lpageop) ||
		   _bt_compare(rel, itup_key, page, P_HIKEY) <= 0);

	newitemoff = _bt_binsrch_insert(rel, insertstate);

	if (insertstate->postingoff == -1)
	{
		/*
		 * There is an overlapping posting list tuple with its LP_DEAD bit
		 * set.  We don't want to unnecessarily unset its LP_DEAD bit while
		 * performing a posting list split, so delete all LP_DEAD items early.
		 * This is the only case where LP_DEAD deletes happen even though
		 * there is space for newitem on the page.
		 */
		_bt_vacuum_one_page(rel, insertstate->buf, heapRel);

		/*
		 * Do new binary search.  New insert location cannot overlap with any
		 * posting list now.
		 */
		insertstate->bounds_valid = false;
		insertstate->postingoff = 0;
		newitemoff = _bt_binsrch_insert(rel, insertstate);
		Assert(insertstate->postingoff == 0);
	}

	return newitemoff;
}

/*
 * Step right to next non-dead page, during insertion.
 *
 * This is a bit more complicated than moving right in a search.  We must
 * write-lock the target page before releasing write lock on current page;
 * else someone else's _bt_check_unique scan could fail to see our insertion.
 * Write locks on intermediate dead pages won't do because we don't know when
 * they will get de-linked from the tree.
 *
 * This is more aggressive than it needs to be for non-unique !heapkeyspace
 * indexes.
 */
static void
_bt_stepright(Relation rel, BTInsertState insertstate, BTStack stack)
{
	Page		page;
	BTPageOpaque lpageop;
	Buffer		rbuf;
	BlockNumber rblkno;

	page = BufferGetPage(insertstate->buf);
	lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

	rbuf = InvalidBuffer;
	rblkno = lpageop->btpo_next;
	for (;;)
	{
		rbuf = _bt_relandgetbuf(rel, rbuf, rblkno, BT_WRITE);
		page = BufferGetPage(rbuf);
		lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * If this page was incompletely split, finish the split now.  We do
		 * this while holding a lock on the left sibling, which is not good
		 * because finishing the split could be a fairly lengthy operation.
		 * But this should happen very seldom.
		 */
		if (P_INCOMPLETE_SPLIT(lpageop))
		{
			_bt_finish_split(rel, rbuf, stack);
			rbuf = InvalidBuffer;
			continue;
		}

		if (!P_IGNORE(lpageop))
			break;
		if (P_RIGHTMOST(lpageop))
			elog(ERROR, "fell off the end of index \"%s\"",
				 RelationGetRelationName(rel));

		rblkno = lpageop->btpo_next;
	}
	/* rbuf locked; unlock buf, update state for caller */
	_bt_relbuf(rel, insertstate->buf);
	insertstate->buf = rbuf;
	insertstate->bounds_valid = false;
}

/*----------
 *	_bt_insertonpg() -- Insert a tuple on a particular page in the index.
 *
 *		This recursive procedure does the following things:
 *
 *			+  if postingoff != 0, splits existing posting list tuple
 *			   (since it overlaps with new 'itup' tuple).
 *			+  if necessary, splits the target page, using 'itup_key' for
 *			   suffix truncation on leaf pages (caller passes NULL for
 *			   non-leaf pages).
 *			+  inserts the new tuple (might be split from posting list).
 *			+  if the page was split, pops the parent stack, and finds the
 *			   right place to insert the new child pointer (by walking
 *			   right using information stored in the parent stack).
 *			+  invokes itself with the appropriate tuple for the right
 *			   child page on the parent.
 *			+  updates the metapage if a true root or fast root is split.
 *
 *		On entry, we must have the correct buffer in which to do the
 *		insertion, and the buffer must be pinned and write-locked.  On return,
 *		we will have dropped both the pin and the lock on the buffer.
 *
 *		This routine only performs retail tuple insertions.  'itup' should
 *		always be either a non-highkey leaf item, or a downlink (new high
 *		key items are created indirectly, when a page is split).  When
 *		inserting to a non-leaf page, 'cbuf' is the left-sibling of the page
 *		we're inserting the downlink for.  This function will clear the
 *		INCOMPLETE_SPLIT flag on it, and release the buffer.
 *----------
 */
static void
_bt_insertonpg(Relation rel,
			   BTScanInsert itup_key,
			   Buffer buf,
			   Buffer cbuf,
			   BTStack stack,
			   IndexTuple itup,
			   Size itemsz,
			   OffsetNumber newitemoff,
			   int postingoff,
			   bool split_only_page)
{
	Page		page;
	BTPageOpaque lpageop;
	IndexTuple	oposting = NULL;
	IndexTuple	origitup = NULL;
	IndexTuple	nposting = NULL;

	page = BufferGetPage(buf);
	lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

	/* child buffer must be given iff inserting on an internal page */
	Assert(P_ISLEAF(lpageop) == !BufferIsValid(cbuf));
	/* tuple must have appropriate number of attributes */
	Assert(!P_ISLEAF(lpageop) ||
		   BTreeTupleGetNAtts(itup, rel) ==
		   IndexRelationGetNumberOfAttributes(rel));
	Assert(P_ISLEAF(lpageop) ||
		   BTreeTupleGetNAtts(itup, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(!BTreeTupleIsPosting(itup));
	Assert(MAXALIGN(IndexTupleSize(itup)) == itemsz);

	/*
	 * Every internal page should have exactly one negative infinity item at
	 * all times.  Only _bt_split() and _bt_newroot() should add items that
	 * become negative infinity items through truncation, since they're the
	 * only routines that allocate new internal pages.
	 */
	Assert(P_ISLEAF(lpageop) || newitemoff > P_FIRSTDATAKEY(lpageop));

	/* The caller should've finished any incomplete splits already. */
	if (P_INCOMPLETE_SPLIT(lpageop))
		elog(ERROR, "cannot insert to incompletely split page %u",
			 BufferGetBlockNumber(buf));

	/*
	 * Do we need to split an existing posting list item?
	 */
	if (postingoff != 0)
	{
		ItemId		itemid = PageGetItemId(page, newitemoff);

		/*
		 * The new tuple is a duplicate with a heap TID that falls inside the
		 * range of an existing posting list tuple on a leaf page.  Prepare to
		 * split an existing posting list.  Overwriting the posting list with
		 * its post-split version is treated as an extra step in either the
		 * insert or page split critical section.
		 */
		Assert(P_ISLEAF(lpageop) && !ItemIdIsDead(itemid));
		Assert(itup_key->heapkeyspace && itup_key->allequalimage);
		oposting = (IndexTuple) PageGetItem(page, itemid);

		/* use a mutable copy of itup as our itup from here on */
		origitup = itup;
		itup = CopyIndexTuple(origitup);
		nposting = _bt_swap_posting(itup, oposting, postingoff);
		/* itup now contains rightmost/max TID from oposting */

		/* Alter offset so that newitem goes after posting list */
		newitemoff = OffsetNumberNext(newitemoff);
	}

	/*
	 * Do we need to split the page to fit the item on it?
	 *
	 * Note: PageGetFreeSpace() subtracts sizeof(ItemIdData) from its result,
	 * so this comparison is correct even though we appear to be accounting
	 * only for the item and not for its line pointer.
	 */
	if (PageGetFreeSpace(page) < itemsz)
	{
		bool		is_root = P_ISROOT(lpageop);
		bool		is_only = P_LEFTMOST(lpageop) && P_RIGHTMOST(lpageop);
		Buffer		rbuf;

		Assert(!split_only_page);

		/* split the buffer into left and right halves */
		rbuf = _bt_split(rel, itup_key, buf, cbuf, newitemoff, itemsz, itup,
						 origitup, nposting, postingoff);
		PredicateLockPageSplit(rel,
							   BufferGetBlockNumber(buf),
							   BufferGetBlockNumber(rbuf));

		/*----------
		 * By here,
		 *
		 *		+  our target page has been split;
		 *		+  the original tuple has been inserted;
		 *		+  we have write locks on both the old (left half)
		 *		   and new (right half) buffers, after the split; and
		 *		+  we know the key we want to insert into the parent
		 *		   (it's the "high key" on the left child page).
		 *
		 * We're ready to do the parent insertion.  We need to hold onto the
		 * locks for the child pages until we locate the parent, but we can
		 * at least release the lock on the right child before doing the
		 * actual insertion.  The lock on the left child will be released
		 * last of all by parent insertion, where it is the 'cbuf' of parent
		 * page.
		 *----------
		 */
		_bt_insert_parent(rel, buf, rbuf, stack, is_root, is_only);
	}
	else
	{
		bool		isleaf = P_ISLEAF(lpageop);
		bool		isrightmost = P_RIGHTMOST(lpageop);
		Buffer		metabuf = InvalidBuffer;
		Page		metapg = NULL;
		BTMetaPageData *metad = NULL;
		BlockNumber blockcache;

		/*
		 * If we are doing this insert because we split a page that was the
		 * only one on its tree level, but was not the root, it may have been
		 * the "fast root".  We need to ensure that the fast root link points
		 * at or above the current page.  We can safely acquire a lock on the
		 * metapage here --- see comments for _bt_newroot().
		 */
		if (split_only_page)
		{
			Assert(!isleaf);
			Assert(BufferIsValid(cbuf));

			metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
			metapg = BufferGetPage(metabuf);
			metad = BTPageGetMeta(metapg);

			if (metad->btm_fastlevel >= lpageop->btpo.level)
			{
				/* no update wanted */
				_bt_relbuf(rel, metabuf);
				metabuf = InvalidBuffer;
			}
		}

		/* Do the update.  No ereport(ERROR) until changes are logged */
		START_CRIT_SECTION();

		if (postingoff != 0)
			memcpy(oposting, nposting, MAXALIGN(IndexTupleSize(nposting)));

		if (PageAddItem(page, (Item) itup, itemsz, newitemoff, false,
						false) == InvalidOffsetNumber)
			elog(PANIC, "failed to add new item to block %u in index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));

		MarkBufferDirty(buf);

		if (BufferIsValid(metabuf))
		{
			/* upgrade meta-page if needed */
			if (metad->btm_version < BTREE_NOVAC_VERSION)
				_bt_upgrademetapage(metapg);
			metad->btm_fastroot = BufferGetBlockNumber(buf);
			metad->btm_fastlevel = lpageop->btpo.level;
			MarkBufferDirty(metabuf);
		}

		/*
		 * Clear INCOMPLETE_SPLIT flag on child if inserting the new item
		 * finishes a split
		 */
		if (!isleaf)
		{
			Page		cpage = BufferGetPage(cbuf);
			BTPageOpaque cpageop = (BTPageOpaque) PageGetSpecialPointer(cpage);

			Assert(P_INCOMPLETE_SPLIT(cpageop));
			cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
			MarkBufferDirty(cbuf);
		}

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_btree_insert xlrec;
			xl_btree_metadata xlmeta;
			uint8		xlinfo;
			XLogRecPtr	recptr;
			uint16		upostingoff;

			xlrec.offnum = newitemoff;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfBtreeInsert);

			if (isleaf && postingoff == 0)
			{
				/* Simple leaf insert */
				xlinfo = XLOG_BTREE_INSERT_LEAF;
			}
			else if (postingoff != 0)
			{
				/*
				 * Leaf insert with posting list split.  Must include
				 * postingoff field before newitem/orignewitem.
				 */
				Assert(isleaf);
				xlinfo = XLOG_BTREE_INSERT_POST;
			}
			else
			{
				/* Internal page insert, which finishes a split on cbuf */
				xlinfo = XLOG_BTREE_INSERT_UPPER;
				XLogRegisterBuffer(1, cbuf, REGBUF_STANDARD);

				if (BufferIsValid(metabuf))
				{
					/* Actually, it's an internal page insert + meta update */
					xlinfo = XLOG_BTREE_INSERT_META;

					Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
					xlmeta.version = metad->btm_version;
					xlmeta.root = metad->btm_root;
					xlmeta.level = metad->btm_level;
					xlmeta.fastroot = metad->btm_fastroot;
					xlmeta.fastlevel = metad->btm_fastlevel;
					xlmeta.oldest_btpo_xact = metad->btm_oldest_btpo_xact;
					xlmeta.last_cleanup_num_heap_tuples =
						metad->btm_last_cleanup_num_heap_tuples;
					xlmeta.allequalimage = metad->btm_allequalimage;

					XLogRegisterBuffer(2, metabuf,
									   REGBUF_WILL_INIT | REGBUF_STANDARD);
					XLogRegisterBufData(2, (char *) &xlmeta,
										sizeof(xl_btree_metadata));
				}
			}

			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			if (postingoff == 0)
			{
				/* Just log itup from caller */
				XLogRegisterBufData(0, (char *) itup, IndexTupleSize(itup));
			}
			else
			{
				/*
				 * Insert with posting list split (XLOG_BTREE_INSERT_POST
				 * record) case.
				 *
				 * Log postingoff.  Also log origitup, not itup.  REDO routine
				 * must reconstruct final itup (as well as nposting) using
				 * _bt_swap_posting().
				 */
				upostingoff = postingoff;

				XLogRegisterBufData(0, (char *) &upostingoff, sizeof(uint16));
				XLogRegisterBufData(0, (char *) origitup,
									IndexTupleSize(origitup));
			}

			recptr = XLogInsert(RM_BTREE_ID, xlinfo);

			if (BufferIsValid(metabuf))
				PageSetLSN(metapg, recptr);
			if (!isleaf)
				PageSetLSN(BufferGetPage(cbuf), recptr);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/* Release subsidiary buffers */
		if (BufferIsValid(metabuf))
			_bt_relbuf(rel, metabuf);
		if (!isleaf)
			_bt_relbuf(rel, cbuf);

		/*
		 * Cache the block number if this is the rightmost leaf page.  Cache
		 * may be used by a future inserter within _bt_search_insert().
		 */
		blockcache = InvalidBlockNumber;
		if (isrightmost && isleaf && !P_ISROOT(lpageop))
			blockcache = BufferGetBlockNumber(buf);

		/* Release buffer for insertion target block */
		_bt_relbuf(rel, buf);

		/*
		 * If we decided to cache the insertion target block before releasing
		 * its buffer lock, then cache it now.  Check the height of the tree
		 * first, though.  We don't go for the optimization with small
		 * indexes.  Defer final check to this point to ensure that we don't
		 * call _bt_getrootheight while holding a buffer lock.
		 */
		if (BlockNumberIsValid(blockcache) &&
			_bt_getrootheight(rel) >= BTREE_FASTPATH_MIN_LEVEL)
			RelationSetTargetBlock(rel, blockcache);
	}

	/* be tidy */
	if (postingoff != 0)
	{
		/* itup is actually a modified copy of caller's original */
		pfree(nposting);
		pfree(itup);
	}
}

/*
 *	_bt_split() -- split a page in the btree.
 *
 *		On entry, buf is the page to split, and is pinned and write-locked.
 *		newitemoff etc. tell us about the new item that must be inserted
 *		along with the data from the original page.
 *
 *		itup_key is used for suffix truncation on leaf pages (internal
 *		page callers pass NULL).  When splitting a non-leaf page, 'cbuf'
 *		is the left-sibling of the page we're inserting the downlink for.
 *		This function will clear the INCOMPLETE_SPLIT flag on it, and
 *		release the buffer.
 *
 *		orignewitem, nposting, and postingoff are needed when an insert of
 *		orignewitem results in both a posting list split and a page split.
 *		These extra posting list split details are used here in the same
 *		way as they are used in the more common case where a posting list
 *		split does not coincide with a page split.  We need to deal with
 *		posting list splits directly in order to ensure that everything
 *		that follows from the insert of orignewitem is handled as a single
 *		atomic operation (though caller's insert of a new pivot/downlink
 *		into parent page will still be a separate operation).  See
 *		nbtree/README for details on the design of posting list splits.
 *
 *		Returns the new right sibling of buf, pinned and write-locked.
 *		The pin and lock on buf are maintained.
 */
static Buffer
_bt_split(Relation rel, BTScanInsert itup_key, Buffer buf, Buffer cbuf,
		  OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
		  IndexTuple orignewitem, IndexTuple nposting, uint16 postingoff)
{
	Buffer		rbuf;
	Page		origpage;
	Page		leftpage,
				rightpage;
	BlockNumber origpagenumber,
				rightpagenumber;
	BTPageOpaque ropaque,
				lopaque,
				oopaque;
	Buffer		sbuf = InvalidBuffer;
	Page		spage = NULL;
	BTPageOpaque sopaque = NULL;
	Size		itemsz;
	ItemId		itemid;
	IndexTuple	firstright,
				lefthighkey;
	OffsetNumber firstrightoff;
	OffsetNumber afterleftoff,
				afterrightoff,
				minusinfoff;
	OffsetNumber origpagepostingoff;
	OffsetNumber maxoff;
	OffsetNumber i;
	bool		newitemonleft,
				isleaf,
				isrightmost;

	/*
	 * origpage is the original page to be split.  leftpage is a temporary
	 * buffer that receives the left-sibling data, which will be copied back
	 * into origpage on success.  rightpage is the new page that will receive
	 * the right-sibling data.
	 *
	 * leftpage is allocated after choosing a split point.  rightpage's new
	 * buffer isn't acquired until after leftpage is initialized and has new
	 * high key, the last point where splitting the page may fail (barring
	 * corruption).  Failing before acquiring new buffer won't have lasting
	 * consequences, since origpage won't have been modified and leftpage is
	 * only workspace.
	 */
	origpage = BufferGetPage(buf);
	oopaque = (BTPageOpaque) PageGetSpecialPointer(origpage);
	isleaf = P_ISLEAF(oopaque);
	isrightmost = P_RIGHTMOST(oopaque);
	maxoff = PageGetMaxOffsetNumber(origpage);
	origpagenumber = BufferGetBlockNumber(buf);

	/*
	 * Choose a point to split origpage at.
	 *
	 * A split point can be thought of as a point _between_ two existing data
	 * items on origpage (the lastleft and firstright tuples), provided you
	 * pretend that the new item that didn't fit is already on origpage.
	 *
	 * Since origpage does not actually contain newitem, the representation of
	 * split points needs to work with two boundary cases: splits where
	 * newitem is lastleft, and splits where newitem is firstright.
	 * newitemonleft resolves the ambiguity that would otherwise exist when
	 * newitemoff == firstrightoff.  In all other cases it's clear which side
	 * of the split every tuple goes on from context.  newitemonleft is
	 * usually (but not always) redundant information.
	 *
	 * firstrightoff is supposed to be an origpage offset number, but it's
	 * possible that its value will be maxoff+1, which is "past the end" of
	 * origpage.  This happens in the rare case where newitem goes after all
	 * existing items (i.e. newitemoff is maxoff+1) and we end up splitting
	 * origpage at the point that leaves newitem alone on new right page.  Any
	 * "!newitemonleft && newitemoff == firstrightoff" split point makes
	 * newitem the firstright tuple, though, so this case isn't a special
	 * case.
	 */
	firstrightoff = _bt_findsplitloc(rel, origpage, newitemoff, newitemsz,
									 newitem, &newitemonleft);

	/* Allocate temp buffer for leftpage */
	leftpage = PageGetTempPage(origpage);
	_bt_pageinit(leftpage, BufferGetPageSize(buf));
	lopaque = (BTPageOpaque) PageGetSpecialPointer(leftpage);

	/*
	 * leftpage won't be the root when we're done.  Also, clear the SPLIT_END
	 * and HAS_GARBAGE flags.
	 */
	lopaque->btpo_flags = oopaque->btpo_flags;
	lopaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
	/* set flag in leftpage indicating that rightpage has no downlink yet */
	lopaque->btpo_flags |= BTP_INCOMPLETE_SPLIT;
	lopaque->btpo_prev = oopaque->btpo_prev;
	/* handle btpo_next after rightpage buffer acquired */
	lopaque->btpo.level = oopaque->btpo.level;
	/* handle btpo_cycleid after rightpage buffer acquired */

	/*
	 * Copy the original page's LSN into leftpage, which will become the
	 * updated version of the page.  We need this because XLogInsert will
	 * examine the LSN and possibly dump it in a page image.
	 */
	PageSetLSN(leftpage, PageGetLSN(origpage));

	/*
	 * Determine page offset number of existing overlapped-with-orignewitem
	 * posting list when it is necessary to perform a posting list split in
	 * passing.  Note that newitem was already changed by caller (newitem no
	 * longer has the orignewitem TID).
	 *
	 * This page offset number (origpagepostingoff) will be used to pretend
	 * that the posting split has already taken place, even though the
	 * required modifications to origpage won't occur until we reach the
	 * critical section.  The lastleft and firstright tuples of our page split
	 * point should, in effect, come from an imaginary version of origpage
	 * that has the nposting tuple instead of the original posting list tuple.
	 *
	 * Note: _bt_findsplitloc() should have compensated for coinciding posting
	 * list splits in just the same way, at least in theory.  It doesn't
	 * bother with that, though.  In practice it won't affect its choice of
	 * split point.
	 */
	origpagepostingoff = InvalidOffsetNumber;
	if (postingoff != 0)
	{
		Assert(isleaf);
		Assert(ItemPointerCompare(&orignewitem->t_tid,
								  &newitem->t_tid) < 0);
		Assert(BTreeTupleIsPosting(nposting));
		origpagepostingoff = OffsetNumberPrev(newitemoff);
	}

	/*
	 * The high key for the new left page is a possibly-truncated copy of
	 * firstright on the leaf level (it's "firstright itself" on internal
	 * pages; see !isleaf comments below).  This may seem to be contrary to
	 * Lehman & Yao's approach of using a copy of lastleft as the new high key
	 * when splitting on the leaf level.  It isn't, though.
	 *
	 * Suffix truncation will leave the left page's high key fully equal to
	 * lastleft when lastleft and firstright are equal prior to heap TID (that
	 * is, the tiebreaker TID value comes from lastleft).  It isn't actually
	 * necessary for a new leaf high key to be a copy of lastleft for the L&Y
	 * "subtree" invariant to hold.  It's sufficient to make sure that the new
	 * leaf high key is strictly less than firstright, and greater than or
	 * equal to (not necessarily equal to) lastleft.  In other words, when
	 * suffix truncation isn't possible during a leaf page split, we take
	 * L&Y's exact approach to generating a new high key for the left page.
	 * (Actually, that is slightly inaccurate.  We don't just use a copy of
	 * lastleft.  A tuple with all the keys from firstright but the max heap
	 * TID from lastleft is used, to avoid introducing a special case.)
	 */
	if (!newitemonleft && newitemoff == firstrightoff)
	{
		/* incoming tuple becomes firstright */
		itemsz = newitemsz;
		firstright = newitem;
	}
	else
	{
		/* existing item at firstrightoff becomes firstright */
		itemid = PageGetItemId(origpage, firstrightoff);
		itemsz = ItemIdGetLength(itemid);
		firstright = (IndexTuple) PageGetItem(origpage, itemid);
		if (firstrightoff == origpagepostingoff)
			firstright = nposting;
	}

	if (isleaf)
	{
		IndexTuple	lastleft;

		/* Attempt suffix truncation for leaf page splits */
		if (newitemonleft && newitemoff == firstrightoff)
		{
			/* incoming tuple becomes lastleft */
			lastleft = newitem;
		}
		else
		{
			OffsetNumber lastleftoff;

			/* existing item before firstrightoff becomes lastleft */
			lastleftoff = OffsetNumberPrev(firstrightoff);
			Assert(lastleftoff >= P_FIRSTDATAKEY(oopaque));
			itemid = PageGetItemId(origpage, lastleftoff);
			lastleft = (IndexTuple) PageGetItem(origpage, itemid);
			if (lastleftoff == origpagepostingoff)
				lastleft = nposting;
		}

		lefthighkey = _bt_truncate(rel, lastleft, firstright, itup_key);
		itemsz = IndexTupleSize(lefthighkey);
	}
	else
	{
		/*
		 * Don't perform suffix truncation on a copy of firstright to make
		 * left page high key for internal page splits.  Must use firstright
		 * as new high key directly.
		 *
		 * Each distinct separator key value originates as a leaf level high
		 * key; all other separator keys/pivot tuples are copied from one
		 * level down.  A separator key in a grandparent page must be
		 * identical to high key in rightmost parent page of the subtree to
		 * its left, which must itself be identical to high key in rightmost
		 * child page of that same subtree (this even applies to separator
		 * from grandparent's high key).  There must always be an unbroken
		 * "seam" of identical separator keys that guide index scans at every
		 * level, starting from the grandparent.  That's why suffix truncation
		 * is unsafe here.
		 *
		 * Internal page splits will truncate firstright into a "negative
		 * infinity" data item when it gets inserted on the new right page
		 * below, though.  This happens during the call to _bt_pgaddtup() for
		 * the new first data item for right page.  Do not confuse this
		 * mechanism with suffix truncation.  It is just a convenient way of
		 * implementing page splits that split the internal page "inside"
		 * firstright.  The lefthighkey separator key cannot appear a second
		 * time in the right page (only firstright's downlink goes in right
		 * page).
		 */
		lefthighkey = firstright;
	}

	/*
	 * Add new high key to leftpage
	 */
	afterleftoff = P_HIKEY;

	Assert(BTreeTupleGetNAtts(lefthighkey, rel) > 0);
	Assert(BTreeTupleGetNAtts(lefthighkey, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(itemsz == MAXALIGN(IndexTupleSize(lefthighkey)));
	if (PageAddItem(leftpage, (Item) lefthighkey, itemsz, afterleftoff, false,
					false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add high key to the left sibling"
			 " while splitting block %u of index \"%s\"",
			 origpagenumber, RelationGetRelationName(rel));
	afterleftoff = OffsetNumberNext(afterleftoff);

	/*
	 * Acquire a new right page to split into, now that left page has a new
	 * high key.  From here on, it's not okay to throw an error without
	 * zeroing rightpage first.  This coding rule ensures that we won't
	 * confuse future VACUUM operations, which might otherwise try to re-find
	 * a downlink to a leftover junk page as the page undergoes deletion.
	 *
	 * It would be reasonable to start the critical section just after the new
	 * rightpage buffer is acquired instead; that would allow us to avoid
	 * leftover junk pages without bothering to zero rightpage.  We do it this
	 * way because it avoids an unnecessary PANIC when either origpage or its
	 * existing sibling page are corrupt.
	 */
	rbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	rightpage = BufferGetPage(rbuf);
	rightpagenumber = BufferGetBlockNumber(rbuf);
	/* rightpage was initialized by _bt_getbuf */
	ropaque = (BTPageOpaque) PageGetSpecialPointer(rightpage);

	/*
	 * Finish off remaining leftpage special area fields.  They cannot be set
	 * before both origpage (leftpage) and rightpage buffers are acquired and
	 * locked.
	 *
	 * btpo_cycleid is only used with leaf pages, though we set it here in all
	 * cases just to be consistent.
	 */
	lopaque->btpo_next = rightpagenumber;
	lopaque->btpo_cycleid = _bt_vacuum_cycleid(rel);

	/*
	 * rightpage won't be the root when we're done.  Also, clear the SPLIT_END
	 * and HAS_GARBAGE flags.
	 */
	ropaque->btpo_flags = oopaque->btpo_flags;
	ropaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
	ropaque->btpo_prev = origpagenumber;
	ropaque->btpo_next = oopaque->btpo_next;
	ropaque->btpo.level = oopaque->btpo.level;
	ropaque->btpo_cycleid = lopaque->btpo_cycleid;

	/*
	 * Add new high key to rightpage where necessary.
	 *
	 * If the page we're splitting is not the rightmost page at its level in
	 * the tree, then the first entry on the page is the high key from
	 * origpage.
	 */
	afterrightoff = P_HIKEY;

	if (!isrightmost)
	{
		IndexTuple	righthighkey;

		itemid = PageGetItemId(origpage, P_HIKEY);
		itemsz = ItemIdGetLength(itemid);
		righthighkey = (IndexTuple) PageGetItem(origpage, itemid);
		Assert(BTreeTupleGetNAtts(righthighkey, rel) > 0);
		Assert(BTreeTupleGetNAtts(righthighkey, rel) <=
			   IndexRelationGetNumberOfKeyAttributes(rel));
		if (PageAddItem(rightpage, (Item) righthighkey, itemsz, afterrightoff,
						false, false) == InvalidOffsetNumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add high key to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		afterrightoff = OffsetNumberNext(afterrightoff);
	}

	/*
	 * Internal page splits truncate first data item on right page -- it
	 * becomes "minus infinity" item for the page.  Set this up here.
	 */
	minusinfoff = InvalidOffsetNumber;
	if (!isleaf)
		minusinfoff = afterrightoff;

	/*
	 * Now transfer all the data items (non-pivot tuples in isleaf case, or
	 * additional pivot tuples in !isleaf case) to the appropriate page.
	 *
	 * Note: we *must* insert at least the right page's items in item-number
	 * order, for the benefit of _bt_restore_page().
	 */
	for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	dataitem;

		itemid = PageGetItemId(origpage, i);
		itemsz = ItemIdGetLength(itemid);
		dataitem = (IndexTuple) PageGetItem(origpage, itemid);

		/* replace original item with nposting due to posting split? */
		if (i == origpagepostingoff)
		{
			Assert(BTreeTupleIsPosting(dataitem));
			Assert(itemsz == MAXALIGN(IndexTupleSize(nposting)));
			dataitem = nposting;
		}

		/* does new item belong before this one? */
		else if (i == newitemoff)
		{
			if (newitemonleft)
			{
				Assert(newitemoff <= firstrightoff);
				if (!_bt_pgaddtup(leftpage, newitemsz, newitem, afterleftoff,
								  false))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the left sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				afterleftoff = OffsetNumberNext(afterleftoff);
			}
			else
			{
				Assert(newitemoff >= firstrightoff);
				if (!_bt_pgaddtup(rightpage, newitemsz, newitem, afterrightoff,
								  afterrightoff == minusinfoff))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the right sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				afterrightoff = OffsetNumberNext(afterrightoff);
			}
		}

		/* decide which page to put it on */
		if (i < firstrightoff)
		{
			if (!_bt_pgaddtup(leftpage, itemsz, dataitem, afterleftoff, false))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the left sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			afterleftoff = OffsetNumberNext(afterleftoff);
		}
		else
		{
			if (!_bt_pgaddtup(rightpage, itemsz, dataitem, afterrightoff,
							  afterrightoff == minusinfoff))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the right sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			afterrightoff = OffsetNumberNext(afterrightoff);
		}
	}

	/* Handle case where newitem goes at the end of rightpage */
	if (i <= newitemoff)
	{
		/*
		 * Can't have newitemonleft here; that would imply we were told to put
		 * *everything* on the left page, which cannot fit (if it could, we'd
		 * not be splitting the page).
		 */
		Assert(!newitemonleft && newitemoff == maxoff + 1);
		if (!_bt_pgaddtup(rightpage, newitemsz, newitem, afterrightoff,
						  afterrightoff == minusinfoff))
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add new item to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		afterrightoff = OffsetNumberNext(afterrightoff);
	}

	/*
	 * We have to grab the right sibling (if any) and fix the prev pointer
	 * there. We are guaranteed that this is deadlock-free since no other
	 * writer will be holding a lock on that page and trying to move left, and
	 * all readers release locks on a page before trying to fetch its
	 * neighbors.
	 */
	if (!isrightmost)
	{
		sbuf = _bt_getbuf(rel, oopaque->btpo_next, BT_WRITE);
		spage = BufferGetPage(sbuf);
		sopaque = (BTPageOpaque) PageGetSpecialPointer(spage);
		if (sopaque->btpo_prev != origpagenumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling's left-link doesn't match: "
									 "block %u links to %u instead of expected %u in index \"%s\"",
									 oopaque->btpo_next, sopaque->btpo_prev, origpagenumber,
									 RelationGetRelationName(rel))));
		}

		/*
		 * Check to see if we can set the SPLIT_END flag in the right-hand
		 * split page; this can save some I/O for vacuum since it need not
		 * proceed to the right sibling.  We can set the flag if the right
		 * sibling has a different cycleid: that means it could not be part of
		 * a group of pages that were all split off from the same ancestor
		 * page.  If you're confused, imagine that page A splits to A B and
		 * then again, yielding A C B, while vacuum is in progress.  Tuples
		 * originally in A could now be in either B or C, hence vacuum must
		 * examine both pages.  But if D, our right sibling, has a different
		 * cycleid then it could not contain any tuples that were in A when
		 * the vacuum started.
		 */
		if (sopaque->btpo_cycleid != ropaque->btpo_cycleid)
			ropaque->btpo_flags |= BTP_SPLIT_END;
	}

	/*
	 * Right sibling is locked, new siblings are prepared, but original page
	 * is not updated yet.
	 *
	 * NO EREPORT(ERROR) till right sibling is updated.  We can get away with
	 * not starting the critical section till here because we haven't been
	 * scribbling on the original page yet; see comments above.
	 */
	START_CRIT_SECTION();

	/*
	 * By here, the original data page has been split into two new halves, and
	 * these are correct.  The algorithm requires that the left page never
	 * move during a split, so we copy the new left page back on top of the
	 * original.  We need to do this before writing the WAL record, so that
	 * XLogInsert can WAL log an image of the page if necessary.
	 */
	PageRestoreTempPage(leftpage, origpage);
	/* leftpage, lopaque must not be used below here */

	MarkBufferDirty(buf);
	MarkBufferDirty(rbuf);

	if (!isrightmost)
	{
		sopaque->btpo_prev = rightpagenumber;
		MarkBufferDirty(sbuf);
	}

	/*
	 * Clear INCOMPLETE_SPLIT flag on child if inserting the new item finishes
	 * a split
	 */
	if (!isleaf)
	{
		Page		cpage = BufferGetPage(cbuf);
		BTPageOpaque cpageop = (BTPageOpaque) PageGetSpecialPointer(cpage);

		cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
		MarkBufferDirty(cbuf);
	}

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_split xlrec;
		uint8		xlinfo;
		XLogRecPtr	recptr;

		xlrec.level = ropaque->btpo.level;
		/* See comments below on newitem, orignewitem, and posting lists */
		xlrec.firstrightoff = firstrightoff;
		xlrec.newitemoff = newitemoff;
		xlrec.postingoff = 0;
		if (postingoff != 0 && origpagepostingoff < firstrightoff)
			xlrec.postingoff = postingoff;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBtreeSplit);

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, rbuf, REGBUF_WILL_INIT);
		/* Log original right sibling, since we've changed its prev-pointer */
		if (!isrightmost)
			XLogRegisterBuffer(2, sbuf, REGBUF_STANDARD);
		if (!isleaf)
			XLogRegisterBuffer(3, cbuf, REGBUF_STANDARD);

		/*
		 * Log the new item, if it was inserted on the left page. (If it was
		 * put on the right page, we don't need to explicitly WAL log it
		 * because it's included with all the other items on the right page.)
		 * Show the new item as belonging to the left page buffer, so that it
		 * is not stored if XLogInsert decides it needs a full-page image of
		 * the left page.  We always store newitemoff in the record, though.
		 *
		 * The details are sometimes slightly different for page splits that
		 * coincide with a posting list split.  If both the replacement
		 * posting list and newitem go on the right page, then we don't need
		 * to log anything extra, just like the simple !newitemonleft
		 * no-posting-split case (postingoff is set to zero in the WAL record,
		 * so recovery doesn't need to process a posting list split at all).
		 * Otherwise, we set postingoff and log orignewitem instead of
		 * newitem, despite having actually inserted newitem.  REDO routine
		 * must reconstruct nposting and newitem using _bt_swap_posting().
		 *
		 * Note: It's possible that our page split point is the point that
		 * makes the posting list lastleft and newitem firstright.  This is
		 * the only case where we log orignewitem/newitem despite newitem
		 * going on the right page.  If XLogInsert decides that it can omit
		 * orignewitem due to logging a full-page image of the left page,
		 * everything still works out, since recovery only needs to log
		 * orignewitem for items on the left page (just like the regular
		 * newitem-logged case).
		 */
		if (newitemonleft && xlrec.postingoff == 0)
			XLogRegisterBufData(0, (char *) newitem, newitemsz);
		else if (xlrec.postingoff != 0)
		{
			Assert(isleaf);
			Assert(newitemonleft || firstrightoff == newitemoff);
			Assert(newitemsz == IndexTupleSize(orignewitem));
			XLogRegisterBufData(0, (char *) orignewitem, newitemsz);
		}

		/* Log the left page's new high key */
		if (!isleaf)
		{
			/* lefthighkey isn't local copy, get current pointer */
			itemid = PageGetItemId(origpage, P_HIKEY);
			lefthighkey = (IndexTuple) PageGetItem(origpage, itemid);
		}
		XLogRegisterBufData(0, (char *) lefthighkey,
							MAXALIGN(IndexTupleSize(lefthighkey)));

		/*
		 * Log the contents of the right page in the format understood by
		 * _bt_restore_page().  The whole right page will be recreated.
		 *
		 * Direct access to page is not good but faster - we should implement
		 * some new func in page API.  Note we only store the tuples
		 * themselves, knowing that they were inserted in item-number order
		 * and so the line pointers can be reconstructed.  See comments for
		 * _bt_restore_page().
		 */
		XLogRegisterBufData(1,
							(char *) rightpage + ((PageHeader) rightpage)->pd_upper,
							((PageHeader) rightpage)->pd_special - ((PageHeader) rightpage)->pd_upper);

		xlinfo = newitemonleft ? XLOG_BTREE_SPLIT_L : XLOG_BTREE_SPLIT_R;
		recptr = XLogInsert(RM_BTREE_ID, xlinfo);

		PageSetLSN(origpage, recptr);
		PageSetLSN(rightpage, recptr);
		if (!isrightmost)
			PageSetLSN(spage, recptr);
		if (!isleaf)
			PageSetLSN(BufferGetPage(cbuf), recptr);
	}

	END_CRIT_SECTION();

	/* release the old right sibling */
	if (!isrightmost)
		_bt_relbuf(rel, sbuf);

	/* release the child */
	if (!isleaf)
		_bt_relbuf(rel, cbuf);

	/* be tidy */
	if (isleaf)
		pfree(lefthighkey);

	/* split's done */
	return rbuf;
}

/*
 * _bt_insert_parent() -- Insert downlink into parent, completing split.
 *
 * On entry, buf and rbuf are the left and right split pages, which we
 * still hold write locks on.  Both locks will be released here.  We
 * release the rbuf lock once we have a write lock on the page that we
 * intend to insert a downlink to rbuf on (i.e. buf's current parent page).
 * The lock on buf is released at the same point as the lock on the parent
 * page, since buf's INCOMPLETE_SPLIT flag must be cleared by the same
 * atomic operation that completes the split by inserting a new downlink.
 *
 * stack - stack showing how we got here.  Will be NULL when splitting true
 *			root, or during concurrent root split, where we can be inefficient
 * is_root - we split the true root
 * is_only - we split a page alone on its level (might have been fast root)
 */
static void
_bt_insert_parent(Relation rel,
				  Buffer buf,
				  Buffer rbuf,
				  BTStack stack,
				  bool is_root,
				  bool is_only)
{
	/*
	 * Here we have to do something Lehman and Yao don't talk about: deal with
	 * a root split and construction of a new root.  If our stack is empty
	 * then we have just split a node on what had been the root level when we
	 * descended the tree.  If it was still the root then we perform a
	 * new-root construction.  If it *wasn't* the root anymore, search to find
	 * the next higher level that someone constructed meanwhile, and find the
	 * right place to insert as for the normal case.
	 *
	 * If we have to search for the parent level, we do so by re-descending
	 * from the root.  This is not super-efficient, but it's rare enough not
	 * to matter.
	 */
	if (is_root)
	{
		Buffer		rootbuf;

		Assert(stack == NULL);
		Assert(is_only);
		/* create a new root node and update the metapage */
		rootbuf = _bt_newroot(rel, buf, rbuf);
		/* release the split buffers */
		_bt_relbuf(rel, rootbuf);
		_bt_relbuf(rel, rbuf);
		_bt_relbuf(rel, buf);
	}
	else
	{
		BlockNumber bknum = BufferGetBlockNumber(buf);
		BlockNumber rbknum = BufferGetBlockNumber(rbuf);
		Page		page = BufferGetPage(buf);
		IndexTuple	new_item;
		BTStackData fakestack;
		IndexTuple	ritem;
		Buffer		pbuf;

		if (stack == NULL)
		{
			BTPageOpaque lpageop;

			elog(DEBUG2, "concurrent ROOT page split");
			lpageop = (BTPageOpaque) PageGetSpecialPointer(page);

			/*
			 * We should never reach here when a leaf page split takes place
			 * despite the insert of newitem being able to apply the fastpath
			 * optimization.  Make sure of that with an assertion.
			 *
			 * This is more of a performance issue than a correctness issue.
			 * The fastpath won't have a descent stack.  Using a phony stack
			 * here works, but never rely on that.  The fastpath should be
			 * rejected within _bt_search_insert() when the rightmost leaf
			 * page will split, since it's faster to go through _bt_search()
			 * and get a stack in the usual way.
			 */
			Assert(!(P_ISLEAF(lpageop) &&
					 BlockNumberIsValid(RelationGetTargetBlock(rel))));

			/* Find the leftmost page at the next level up */
			pbuf = _bt_get_endpoint(rel, lpageop->btpo.level + 1, false,
									NULL);
			/* Set up a phony stack entry pointing there */
			stack = &fakestack;
			stack->bts_blkno = BufferGetBlockNumber(pbuf);
			stack->bts_offset = InvalidOffsetNumber;
			stack->bts_parent = NULL;
			_bt_relbuf(rel, pbuf);
		}

		/* get high key from left, a strict lower bound for new right page */
		ritem = (IndexTuple) PageGetItem(page,
										 PageGetItemId(page, P_HIKEY));

		/* form an index tuple that points at the new right page */
		new_item = CopyIndexTuple(ritem);
		BTreeTupleSetDownLink(new_item, rbknum);

		/*
		 * Re-find and write lock the parent of buf.
		 *
		 * It's possible that the location of buf's downlink has changed since
		 * our initial _bt_search() descent.  _bt_getstackbuf() will detect
		 * and recover from this, updating the stack, which ensures that the
		 * new downlink will be inserted at the correct offset. Even buf's
		 * parent may have changed.
		 */
		pbuf = _bt_getstackbuf(rel, stack, bknum);

		/*
		 * Unlock the right child.  The left child will be unlocked in
		 * _bt_insertonpg().
		 *
		 * Unlocking the right child must be delayed until here to ensure that
		 * no concurrent VACUUM operation can become confused.  Page deletion
		 * cannot be allowed to fail to re-find a downlink for the rbuf page.
		 * (Actually, this is just a vestige of how things used to work.  The
		 * page deletion code is expected to check for the INCOMPLETE_SPLIT
		 * flag on the left child.  It won't attempt deletion of the right
		 * child until the split is complete.  Despite all this, we opt to
		 * conservatively delay unlocking the right child until here.)
		 */
		_bt_relbuf(rel, rbuf);

		if (pbuf == InvalidBuffer)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("failed to re-find parent key in index \"%s\" for split pages %u/%u",
									 RelationGetRelationName(rel), bknum, rbknum)));

		/* Recursively insert into the parent */
		_bt_insertonpg(rel, NULL, pbuf, buf, stack->bts_parent,
					   new_item, MAXALIGN(IndexTupleSize(new_item)),
					   stack->bts_offset + 1, 0, is_only);

		/* be tidy */
		pfree(new_item);
	}
}

/*
 * _bt_finish_split() -- Finish an incomplete split
 *
 * A crash or other failure can leave a split incomplete.  The insertion
 * routines won't allow to insert on a page that is incompletely split.
 * Before inserting on such a page, call _bt_finish_split().
 *
 * On entry, 'lbuf' must be locked in write-mode.  On exit, it is unlocked
 * and unpinned.
 */
void
_bt_finish_split(Relation rel, Buffer lbuf, BTStack stack)
{
	Page		lpage = BufferGetPage(lbuf);
	BTPageOpaque lpageop = (BTPageOpaque) PageGetSpecialPointer(lpage);
	Buffer		rbuf;
	Page		rpage;
	BTPageOpaque rpageop;
	bool		was_root;
	bool		was_only;

	Assert(P_INCOMPLETE_SPLIT(lpageop));

	/* Lock right sibling, the one missing the downlink */
	rbuf = _bt_getbuf(rel, lpageop->btpo_next, BT_WRITE);
	rpage = BufferGetPage(rbuf);
	rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);

	/* Could this be a root split? */
	if (!stack)
	{
		Buffer		metabuf;
		Page		metapg;
		BTMetaPageData *metad;

		/* acquire lock on the metapage */
		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
		metapg = BufferGetPage(metabuf);
		metad = BTPageGetMeta(metapg);

		was_root = (metad->btm_root == BufferGetBlockNumber(lbuf));

		_bt_relbuf(rel, metabuf);
	}
	else
		was_root = false;

	/* Was this the only page on the level before split? */
	was_only = (P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop));

	elog(DEBUG1, "finishing incomplete split of %u/%u",
		 BufferGetBlockNumber(lbuf), BufferGetBlockNumber(rbuf));

	_bt_insert_parent(rel, lbuf, rbuf, stack, was_root, was_only);
}

/*
 *	_bt_getstackbuf() -- Walk back up the tree one step, and find the pivot
 *						 tuple whose downlink points to child page.
 *
 *		Caller passes child's block number, which is used to identify
 *		associated pivot tuple in parent page using a linear search that
 *		matches on pivot's downlink/block number.  The expected location of
 *		the pivot tuple is taken from the stack one level above the child
 *		page.  This is used as a starting point.  Insertions into the
 *		parent level could cause the pivot tuple to move right; deletions
 *		could cause it to move left, but not left of the page we previously
 *		found it on.
 *
 *		Caller can use its stack to relocate the pivot tuple/downlink for
 *		any same-level page to the right of the page found by its initial
 *		descent.  This is necessary because of the possibility that caller
 *		moved right to recover from a concurrent page split.  It's also
 *		convenient for certain callers to be able to step right when there
 *		wasn't a concurrent page split, while still using their original
 *		stack.  For example, the checkingunique _bt_doinsert() case may
 *		have to step right when there are many physical duplicates, and its
 *		scantid forces an insertion to the right of the "first page the
 *		value could be on".  (This is also relied on by all of our callers
 *		when dealing with !heapkeyspace indexes.)
 *
 *		Returns write-locked parent page buffer, or InvalidBuffer if pivot
 *		tuple not found (should not happen).  Adjusts bts_blkno &
 *		bts_offset if changed.  Page split caller should insert its new
 *		pivot tuple for its new right sibling page on parent page, at the
 *		offset number bts_offset + 1.
 */
Buffer
_bt_getstackbuf(Relation rel, BTStack stack, BlockNumber child)
{
	BlockNumber blkno;
	OffsetNumber start;

	blkno = stack->bts_blkno;
	start = stack->bts_offset;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;

		buf = _bt_getbuf(rel, blkno, BT_WRITE);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		if (P_INCOMPLETE_SPLIT(opaque))
		{
			_bt_finish_split(rel, buf, stack->bts_parent);
			continue;
		}

		if (!P_IGNORE(opaque))
		{
			OffsetNumber offnum,
						minoff,
						maxoff;
			ItemId		itemid;
			IndexTuple	item;

			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);

			/*
			 * start = InvalidOffsetNumber means "search the whole page". We
			 * need this test anyway due to possibility that page has a high
			 * key now when it didn't before.
			 */
			if (start < minoff)
				start = minoff;

			/*
			 * Need this check too, to guard against possibility that page
			 * split since we visited it originally.
			 */
			if (start > maxoff)
				start = OffsetNumberNext(maxoff);

			/*
			 * These loops will check every item on the page --- but in an
			 * order that's attuned to the probability of where it actually
			 * is.  Scan to the right first, then to the left.
			 */
			for (offnum = start;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (BTreeTupleGetDownLink(item) == child)
				{
					/* Return accurate pointer to where link is now */
					stack->bts_blkno = blkno;
					stack->bts_offset = offnum;
					return buf;
				}
			}

			for (offnum = OffsetNumberPrev(start);
				 offnum >= minoff;
				 offnum = OffsetNumberPrev(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (BTreeTupleGetDownLink(item) == child)
				{
					/* Return accurate pointer to where link is now */
					stack->bts_blkno = blkno;
					stack->bts_offset = offnum;
					return buf;
				}
			}
		}

		/*
		 * The item we're looking for moved right at least one page.
		 *
		 * Lehman and Yao couple/chain locks when moving right here, which we
		 * can avoid.  See nbtree/README.
		 */
		if (P_RIGHTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			return InvalidBuffer;
		}
		blkno = opaque->btpo_next;
		start = InvalidOffsetNumber;
		_bt_relbuf(rel, buf);
	}
}

/*
 *	_bt_newroot() -- Create a new root page for the index.
 *
 *		We've just split the old root page and need to create a new one.
 *		In order to do this, we add a new root page to the file, then lock
 *		the metadata page and update it.  This is guaranteed to be deadlock-
 *		free, because all readers release their locks on the metadata page
 *		before trying to lock the root, and all writers lock the root before
 *		trying to lock the metadata page.  We have a write lock on the old
 *		root page, so we have not introduced any cycles into the waits-for
 *		graph.
 *
 *		On entry, lbuf (the old root) and rbuf (its new peer) are write-
 *		locked. On exit, a new root page exists with entries for the
 *		two new children, metapage is updated and unlocked/unpinned.
 *		The new root buffer is returned to caller which has to unlock/unpin
 *		lbuf, rbuf & rootbuf.
 */
static Buffer
_bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf)
{
	Buffer		rootbuf;
	Page		lpage,
				rootpage;
	BlockNumber lbkno,
				rbkno;
	BlockNumber rootblknum;
	BTPageOpaque rootopaque;
	BTPageOpaque lopaque;
	ItemId		itemid;
	IndexTuple	item;
	IndexTuple	left_item;
	Size		left_item_sz;
	IndexTuple	right_item;
	Size		right_item_sz;
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;

	lbkno = BufferGetBlockNumber(lbuf);
	rbkno = BufferGetBlockNumber(rbuf);
	lpage = BufferGetPage(lbuf);
	lopaque = (BTPageOpaque) PageGetSpecialPointer(lpage);

	/* get a new root page */
	rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
	rootpage = BufferGetPage(rootbuf);
	rootblknum = BufferGetBlockNumber(rootbuf);

	/* acquire lock on the metapage */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/*
	 * Create downlink item for left page (old root).  The key value used is
	 * "minus infinity", a sentinel value that's reliably less than any real
	 * key value that could appear in the left page.
	 */
	left_item_sz = sizeof(IndexTupleData);
	left_item = (IndexTuple) palloc(left_item_sz);
	left_item->t_info = left_item_sz;
	BTreeTupleSetDownLink(left_item, lbkno);
	BTreeTupleSetNAtts(left_item, 0, false);

	/*
	 * Create downlink item for right page.  The key for it is obtained from
	 * the "high key" position in the left page.
	 */
	itemid = PageGetItemId(lpage, P_HIKEY);
	right_item_sz = ItemIdGetLength(itemid);
	item = (IndexTuple) PageGetItem(lpage, itemid);
	right_item = CopyIndexTuple(item);
	BTreeTupleSetDownLink(right_item, rbkno);

	/* NO EREPORT(ERROR) from here till newroot op is logged */
	START_CRIT_SECTION();

	/* upgrade metapage if needed */
	if (metad->btm_version < BTREE_NOVAC_VERSION)
		_bt_upgrademetapage(metapg);

	/* set btree special data */
	rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
	rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
	rootopaque->btpo_flags = BTP_ROOT;
	rootopaque->btpo.level =
		((BTPageOpaque) PageGetSpecialPointer(lpage))->btpo.level + 1;
	rootopaque->btpo_cycleid = 0;

	/* update metapage data */
	metad->btm_root = rootblknum;
	metad->btm_level = rootopaque->btpo.level;
	metad->btm_fastroot = rootblknum;
	metad->btm_fastlevel = rootopaque->btpo.level;

	/*
	 * Insert the left page pointer into the new root page.  The root page is
	 * the rightmost page on its level so there is no "high key" in it; the
	 * two items will go into positions P_HIKEY and P_FIRSTKEY.
	 *
	 * Note: we *must* insert the two items in item-number order, for the
	 * benefit of _bt_restore_page().
	 */
	Assert(BTreeTupleGetNAtts(left_item, rel) == 0);
	if (PageAddItem(rootpage, (Item) left_item, left_item_sz, P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add leftkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/*
	 * insert the right page pointer into the new root page.
	 */
	Assert(BTreeTupleGetNAtts(right_item, rel) > 0);
	Assert(BTreeTupleGetNAtts(right_item, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	if (PageAddItem(rootpage, (Item) right_item, right_item_sz, P_FIRSTKEY,
					false, false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add rightkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/* Clear the incomplete-split flag in the left child */
	Assert(P_INCOMPLETE_SPLIT(lopaque));
	lopaque->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
	MarkBufferDirty(lbuf);

	MarkBufferDirty(rootbuf);
	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_newroot xlrec;
		XLogRecPtr	recptr;
		xl_btree_metadata md;

		xlrec.rootblk = rootblknum;
		xlrec.level = metad->btm_level;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBtreeNewroot);

		XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
		md.version = metad->btm_version;
		md.root = rootblknum;
		md.level = metad->btm_level;
		md.fastroot = rootblknum;
		md.fastlevel = metad->btm_level;
		md.oldest_btpo_xact = metad->btm_oldest_btpo_xact;
		md.last_cleanup_num_heap_tuples = metad->btm_last_cleanup_num_heap_tuples;
		md.allequalimage = metad->btm_allequalimage;

		XLogRegisterBufData(2, (char *) &md, sizeof(xl_btree_metadata));

		/*
		 * Direct access to page is not good but faster - we should implement
		 * some new func in page API.
		 */
		XLogRegisterBufData(0,
							(char *) rootpage + ((PageHeader) rootpage)->pd_upper,
							((PageHeader) rootpage)->pd_special -
							((PageHeader) rootpage)->pd_upper);

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT);

		PageSetLSN(lpage, recptr);
		PageSetLSN(rootpage, recptr);
		PageSetLSN(metapg, recptr);
	}

	END_CRIT_SECTION();

	/* done with metapage */
	_bt_relbuf(rel, metabuf);

	pfree(left_item);
	pfree(right_item);

	return rootbuf;
}

/*
 *	_bt_pgaddtup() -- add a data item to a particular page during split.
 *
 *		The difference between this routine and a bare PageAddItem call is
 *		that this code can deal with the first data item on an internal btree
 *		page in passing.  This data item (which is called "firstright" within
 *		_bt_split()) has a key that must be treated as minus infinity after
 *		the split.  Therefore, we truncate away all attributes when caller
 *		specifies it's the first data item on page (downlink is not changed,
 *		though).  This extra step is only needed for the right page of an
 *		internal page split.  There is no need to do this for the first data
 *		item on the existing/left page, since that will already have been
 *		truncated during an earlier page split.
 *
 *		See _bt_split() for a high level explanation of why we truncate here.
 *		Note that this routine has nothing to do with suffix truncation,
 *		despite using some of the same infrastructure.
 */
static inline bool
_bt_pgaddtup(Page page,
			 Size itemsize,
			 IndexTuple itup,
			 OffsetNumber itup_off,
			 bool newfirstdataitem)
{
	IndexTupleData trunctuple;

	if (newfirstdataitem)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (unlikely(PageAddItem(page, (Item) itup, itemsize, itup_off, false,
							 false) == InvalidOffsetNumber))
		return false;

	return true;
}

/*
 * _bt_vacuum_one_page - vacuum just one index page.
 *
 * Try to remove LP_DEAD items from the given page.  The passed buffer
 * must be exclusive-locked, but unlike a real VACUUM, we don't need a
 * super-exclusive "cleanup" lock (see nbtree/README).
 */
static void
_bt_vacuum_one_page(Relation rel, Buffer buffer, Relation heapRel)
{
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	int			ndeletable = 0;
	OffsetNumber offnum,
				minoff,
				maxoff;
	Page		page = BufferGetPage(buffer);
	BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(P_ISLEAF(opaque));

	/*
	 * Scan over all items to see which ones need to be deleted according to
	 * LP_DEAD flags.
	 */
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemId))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
		_bt_delitems_delete(rel, buffer, deletable, ndeletable, heapRel);

	/*
	 * Note: if we didn't find any LP_DEAD items, then the page's
	 * BTP_HAS_GARBAGE hint bit is falsely set.  We do not bother expending a
	 * separate write to clear it, however.  We will clear it when we split
	 * the page, or when deduplication runs.
	 */
}
