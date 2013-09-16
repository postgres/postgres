/*-------------------------------------------------------------------------
 *
 * rewriteheap.c
 *	  Support functions to rewrite tables.
 *
 * These functions provide a facility to completely rewrite a heap, while
 * preserving visibility information and update chains.
 *
 * INTERFACE
 *
 * The caller is responsible for creating the new heap, all catalog
 * changes, supplying the tuples to be written to the new heap, and
 * rebuilding indexes.	The caller must hold AccessExclusiveLock on the
 * target table, because we assume no one else is writing into it.
 *
 * To use the facility:
 *
 * begin_heap_rewrite
 * while (fetch next tuple)
 * {
 *	   if (tuple is dead)
 *		   rewrite_heap_dead_tuple
 *	   else
 *	   {
 *		   // do any transformations here if required
 *		   rewrite_heap_tuple
 *	   }
 * }
 * end_heap_rewrite
 *
 * The contents of the new relation shouldn't be relied on until after
 * end_heap_rewrite is called.
 *
 *
 * IMPLEMENTATION
 *
 * This would be a fairly trivial affair, except that we need to maintain
 * the ctid chains that link versions of an updated tuple together.
 * Since the newly stored tuples will have tids different from the original
 * ones, if we just copied t_ctid fields to the new table the links would
 * be wrong.  When we are required to copy a (presumably recently-dead or
 * delete-in-progress) tuple whose ctid doesn't point to itself, we have
 * to substitute the correct ctid instead.
 *
 * For each ctid reference from A -> B, we might encounter either A first
 * or B first.	(Note that a tuple in the middle of a chain is both A and B
 * of different pairs.)
 *
 * If we encounter A first, we'll store the tuple in the unresolved_tups
 * hash table. When we later encounter B, we remove A from the hash table,
 * fix the ctid to point to the new location of B, and insert both A and B
 * to the new heap.
 *
 * If we encounter B first, we can insert B to the new heap right away.
 * We then add an entry to the old_new_tid_map hash table showing B's
 * original tid (in the old heap) and new tid (in the new heap).
 * When we later encounter A, we get the new location of B from the table,
 * and can write A immediately with the correct ctid.
 *
 * Entries in the hash tables can be removed as soon as the later tuple
 * is encountered.	That helps to keep the memory usage down.  At the end,
 * both tables are usually empty; we should have encountered both A and B
 * of each pair.  However, it's possible for A to be RECENTLY_DEAD and B
 * entirely DEAD according to HeapTupleSatisfiesVacuum, because the test
 * for deadness using OldestXmin is not exact.	In such a case we might
 * encounter B first, and skip it, and find A later.  Then A would be added
 * to unresolved_tups, and stay there until end of the rewrite.  Since
 * this case is very unusual, we don't worry about the memory usage.
 *
 * Using in-memory hash tables means that we use some memory for each live
 * update chain in the table, from the time we find one end of the
 * reference until we find the other end.  That shouldn't be a problem in
 * practice, but if you do something like an UPDATE without a where-clause
 * on a large table, and then run CLUSTER in the same transaction, you
 * could run out of memory.  It doesn't seem worthwhile to add support for
 * spill-to-disk, as there shouldn't be that many RECENTLY_DEAD tuples in a
 * table under normal circumstances.  Furthermore, in the typical scenario
 * of CLUSTERing on an unchanging key column, we'll see all the versions
 * of a given tuple together anyway, and so the peak memory usage is only
 * proportional to the number of RECENTLY_DEAD versions of a single row, not
 * in the whole table.	Note that if we do fail halfway through a CLUSTER,
 * the old table is still valid, so failure is not catastrophic.
 *
 * We can't use the normal heap_insert function to insert into the new
 * heap, because heap_insert overwrites the visibility information.
 * We use a special-purpose raw_heap_insert function instead, which
 * is optimized for bulk inserting a lot of tuples, knowing that we have
 * exclusive access to the heap.  raw_heap_insert builds new pages in
 * local storage.  When a page is full, or at the end of the process,
 * we insert it to WAL as a single record and then write it to disk
 * directly through smgr.  Note, however, that any data sent to the new
 * heap's TOAST table will go through the normal bufmgr.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/rewriteheap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"


/*
 * State associated with a rewrite operation. This is opaque to the user
 * of the rewrite facility.
 */
typedef struct RewriteStateData
{
	Relation	rs_new_rel;		/* destination heap */
	Page		rs_buffer;		/* page currently being built */
	BlockNumber rs_blockno;		/* block where page will go */
	bool		rs_buffer_valid;	/* T if any tuples in buffer */
	bool		rs_use_wal;		/* must we WAL-log inserts? */
	TransactionId rs_oldest_xmin;		/* oldest xmin used by caller to
										 * determine tuple visibility */
	TransactionId rs_freeze_xid;/* Xid that will be used as freeze cutoff
								 * point */
	MultiXactId rs_cutoff_multi;/* MultiXactId that will be used as cutoff
								 * point for multixacts */
	MemoryContext rs_cxt;		/* for hash tables and entries and tuples in
								 * them */
	HTAB	   *rs_unresolved_tups;		/* unmatched A tuples */
	HTAB	   *rs_old_new_tid_map;		/* unmatched B tuples */
}	RewriteStateData;

/*
 * The lookup keys for the hash tables are tuple TID and xmin (we must check
 * both to avoid false matches from dead tuples).  Beware that there is
 * probably some padding space in this struct; it must be zeroed out for
 * correct hashtable operation.
 */
typedef struct
{
	TransactionId xmin;			/* tuple xmin */
	ItemPointerData tid;		/* tuple location in old heap */
} TidHashKey;

/*
 * Entry structures for the hash tables
 */
typedef struct
{
	TidHashKey	key;			/* expected xmin/old location of B tuple */
	ItemPointerData old_tid;	/* A's location in the old heap */
	HeapTuple	tuple;			/* A's tuple contents */
} UnresolvedTupData;

typedef UnresolvedTupData *UnresolvedTup;

typedef struct
{
	TidHashKey	key;			/* actual xmin/old location of B tuple */
	ItemPointerData new_tid;	/* where we put it in the new heap */
} OldToNewMappingData;

typedef OldToNewMappingData *OldToNewMapping;


/* prototypes for internal functions */
static void raw_heap_insert(RewriteState state, HeapTuple tup);


/*
 * Begin a rewrite of a table
 *
 * new_heap		new, locked heap relation to insert tuples to
 * oldest_xmin	xid used by the caller to determine which tuples are dead
 * freeze_xid	xid before which tuples will be frozen
 * min_multi	multixact before which multis will be removed
 * use_wal		should the inserts to the new heap be WAL-logged?
 *
 * Returns an opaque RewriteState, allocated in current memory context,
 * to be used in subsequent calls to the other functions.
 */
RewriteState
begin_heap_rewrite(Relation new_heap, TransactionId oldest_xmin,
				   TransactionId freeze_xid, MultiXactId cutoff_multi,
				   bool use_wal)
{
	RewriteState state;
	MemoryContext rw_cxt;
	MemoryContext old_cxt;
	HASHCTL		hash_ctl;

	/*
	 * To ease cleanup, make a separate context that will contain the
	 * RewriteState struct itself plus all subsidiary data.
	 */
	rw_cxt = AllocSetContextCreate(CurrentMemoryContext,
								   "Table rewrite",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	old_cxt = MemoryContextSwitchTo(rw_cxt);

	/* Create and fill in the state struct */
	state = palloc0(sizeof(RewriteStateData));

	state->rs_new_rel = new_heap;
	state->rs_buffer = (Page) palloc(BLCKSZ);
	/* new_heap needn't be empty, just locked */
	state->rs_blockno = RelationGetNumberOfBlocks(new_heap);
	state->rs_buffer_valid = false;
	state->rs_use_wal = use_wal;
	state->rs_oldest_xmin = oldest_xmin;
	state->rs_freeze_xid = freeze_xid;
	state->rs_cutoff_multi = cutoff_multi;
	state->rs_cxt = rw_cxt;

	/* Initialize hash tables used to track update chains */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(TidHashKey);
	hash_ctl.entrysize = sizeof(UnresolvedTupData);
	hash_ctl.hcxt = state->rs_cxt;
	hash_ctl.hash = tag_hash;

	state->rs_unresolved_tups =
		hash_create("Rewrite / Unresolved ctids",
					128,		/* arbitrary initial size */
					&hash_ctl,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	hash_ctl.entrysize = sizeof(OldToNewMappingData);

	state->rs_old_new_tid_map =
		hash_create("Rewrite / Old to new tid map",
					128,		/* arbitrary initial size */
					&hash_ctl,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	MemoryContextSwitchTo(old_cxt);

	return state;
}

/*
 * End a rewrite.
 *
 * state and any other resources are freed.
 */
void
end_heap_rewrite(RewriteState state)
{
	HASH_SEQ_STATUS seq_status;
	UnresolvedTup unresolved;

	/*
	 * Write any remaining tuples in the UnresolvedTups table. If we have any
	 * left, they should in fact be dead, but let's err on the safe side.
	 */
	hash_seq_init(&seq_status, state->rs_unresolved_tups);

	while ((unresolved = hash_seq_search(&seq_status)) != NULL)
	{
		ItemPointerSetInvalid(&unresolved->tuple->t_data->t_ctid);
		raw_heap_insert(state, unresolved->tuple);
	}

	/* Write the last page, if any */
	if (state->rs_buffer_valid)
	{
		if (state->rs_use_wal)
			log_newpage(&state->rs_new_rel->rd_node,
						MAIN_FORKNUM,
						state->rs_blockno,
						state->rs_buffer);
		RelationOpenSmgr(state->rs_new_rel);

		PageSetChecksumInplace(state->rs_buffer, state->rs_blockno);

		smgrextend(state->rs_new_rel->rd_smgr, MAIN_FORKNUM, state->rs_blockno,
				   (char *) state->rs_buffer, true);
	}

	/*
	 * If the rel is WAL-logged, must fsync before commit.	We use heap_sync
	 * to ensure that the toast table gets fsync'd too.
	 *
	 * It's obvious that we must do this when not WAL-logging. It's less
	 * obvious that we have to do it even if we did WAL-log the pages. The
	 * reason is the same as in tablecmds.c's copy_relation_data(): we're
	 * writing data that's not in shared buffers, and so a CHECKPOINT
	 * occurring during the rewriteheap operation won't have fsync'd data we
	 * wrote before the checkpoint.
	 */
	if (RelationNeedsWAL(state->rs_new_rel))
		heap_sync(state->rs_new_rel);

	/* Deleting the context frees everything */
	MemoryContextDelete(state->rs_cxt);
}

/*
 * Add a tuple to the new heap.
 *
 * Visibility information is copied from the original tuple, except that
 * we "freeze" very-old tuples.  Note that since we scribble on new_tuple,
 * it had better be temp storage not a pointer to the original tuple.
 *
 * state		opaque state as returned by begin_heap_rewrite
 * old_tuple	original tuple in the old heap
 * new_tuple	new, rewritten tuple to be inserted to new heap
 */
void
rewrite_heap_tuple(RewriteState state,
				   HeapTuple old_tuple, HeapTuple new_tuple)
{
	MemoryContext old_cxt;
	ItemPointerData old_tid;
	TidHashKey	hashkey;
	bool		found;
	bool		free_new;

	old_cxt = MemoryContextSwitchTo(state->rs_cxt);

	/*
	 * Copy the original tuple's visibility information into new_tuple.
	 *
	 * XXX we might later need to copy some t_infomask2 bits, too? Right now,
	 * we intentionally clear the HOT status bits.
	 */
	memcpy(&new_tuple->t_data->t_choice.t_heap,
		   &old_tuple->t_data->t_choice.t_heap,
		   sizeof(HeapTupleFields));

	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		old_tuple->t_data->t_infomask & HEAP_XACT_MASK;

	/*
	 * While we have our hands on the tuple, we may as well freeze any
	 * very-old xmin or xmax, so that future VACUUM effort can be saved.
	 */
	heap_freeze_tuple(new_tuple->t_data, state->rs_freeze_xid,
					  state->rs_cutoff_multi);

	/*
	 * Invalid ctid means that ctid should point to the tuple itself. We'll
	 * override it later if the tuple is part of an update chain.
	 */
	ItemPointerSetInvalid(&new_tuple->t_data->t_ctid);

	/*
	 * If the tuple has been updated, check the old-to-new mapping hash table.
	 */
	if (!((old_tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
		  HeapTupleHeaderIsOnlyLocked(old_tuple->t_data)) &&
		!(ItemPointerEquals(&(old_tuple->t_self),
							&(old_tuple->t_data->t_ctid))))
	{
		OldToNewMapping mapping;

		memset(&hashkey, 0, sizeof(hashkey));
		hashkey.xmin = HeapTupleHeaderGetUpdateXid(old_tuple->t_data);
		hashkey.tid = old_tuple->t_data->t_ctid;

		mapping = (OldToNewMapping)
			hash_search(state->rs_old_new_tid_map, &hashkey,
						HASH_FIND, NULL);

		if (mapping != NULL)
		{
			/*
			 * We've already copied the tuple that t_ctid points to, so we can
			 * set the ctid of this tuple to point to the new location, and
			 * insert it right away.
			 */
			new_tuple->t_data->t_ctid = mapping->new_tid;

			/* We don't need the mapping entry anymore */
			hash_search(state->rs_old_new_tid_map, &hashkey,
						HASH_REMOVE, &found);
			Assert(found);
		}
		else
		{
			/*
			 * We haven't seen the tuple t_ctid points to yet. Stash this
			 * tuple into unresolved_tups to be written later.
			 */
			UnresolvedTup unresolved;

			unresolved = hash_search(state->rs_unresolved_tups, &hashkey,
									 HASH_ENTER, &found);
			Assert(!found);

			unresolved->old_tid = old_tuple->t_self;
			unresolved->tuple = heap_copytuple(new_tuple);

			/*
			 * We can't do anything more now, since we don't know where the
			 * tuple will be written.
			 */
			MemoryContextSwitchTo(old_cxt);
			return;
		}
	}

	/*
	 * Now we will write the tuple, and then check to see if it is the B tuple
	 * in any new or known pair.  When we resolve a known pair, we will be
	 * able to write that pair's A tuple, and then we have to check if it
	 * resolves some other pair.  Hence, we need a loop here.
	 */
	old_tid = old_tuple->t_self;
	free_new = false;

	for (;;)
	{
		ItemPointerData new_tid;

		/* Insert the tuple and find out where it's put in new_heap */
		raw_heap_insert(state, new_tuple);
		new_tid = new_tuple->t_self;

		/*
		 * If the tuple is the updated version of a row, and the prior version
		 * wouldn't be DEAD yet, then we need to either resolve the prior
		 * version (if it's waiting in rs_unresolved_tups), or make an entry
		 * in rs_old_new_tid_map (so we can resolve it when we do see it). The
		 * previous tuple's xmax would equal this one's xmin, so it's
		 * RECENTLY_DEAD if and only if the xmin is not before OldestXmin.
		 */
		if ((new_tuple->t_data->t_infomask & HEAP_UPDATED) &&
			!TransactionIdPrecedes(HeapTupleHeaderGetXmin(new_tuple->t_data),
								   state->rs_oldest_xmin))
		{
			/*
			 * Okay, this is B in an update pair.  See if we've seen A.
			 */
			UnresolvedTup unresolved;

			memset(&hashkey, 0, sizeof(hashkey));
			hashkey.xmin = HeapTupleHeaderGetXmin(new_tuple->t_data);
			hashkey.tid = old_tid;

			unresolved = hash_search(state->rs_unresolved_tups, &hashkey,
									 HASH_FIND, NULL);

			if (unresolved != NULL)
			{
				/*
				 * We have seen and memorized the previous tuple already. Now
				 * that we know where we inserted the tuple its t_ctid points
				 * to, fix its t_ctid and insert it to the new heap.
				 */
				if (free_new)
					heap_freetuple(new_tuple);
				new_tuple = unresolved->tuple;
				free_new = true;
				old_tid = unresolved->old_tid;
				new_tuple->t_data->t_ctid = new_tid;

				/*
				 * We don't need the hash entry anymore, but don't free its
				 * tuple just yet.
				 */
				hash_search(state->rs_unresolved_tups, &hashkey,
							HASH_REMOVE, &found);
				Assert(found);

				/* loop back to insert the previous tuple in the chain */
				continue;
			}
			else
			{
				/*
				 * Remember the new tid of this tuple. We'll use it to set the
				 * ctid when we find the previous tuple in the chain.
				 */
				OldToNewMapping mapping;

				mapping = hash_search(state->rs_old_new_tid_map, &hashkey,
									  HASH_ENTER, &found);
				Assert(!found);

				mapping->new_tid = new_tid;
			}
		}

		/* Done with this (chain of) tuples, for now */
		if (free_new)
			heap_freetuple(new_tuple);
		break;
	}

	MemoryContextSwitchTo(old_cxt);
}

/*
 * Register a dead tuple with an ongoing rewrite. Dead tuples are not
 * copied to the new table, but we still make note of them so that we
 * can release some resources earlier.
 *
 * Returns true if a tuple was removed from the unresolved_tups table.
 * This indicates that that tuple, previously thought to be "recently dead",
 * is now known really dead and won't be written to the output.
 */
bool
rewrite_heap_dead_tuple(RewriteState state, HeapTuple old_tuple)
{
	/*
	 * If we have already seen an earlier tuple in the update chain that
	 * points to this tuple, let's forget about that earlier tuple. It's in
	 * fact dead as well, our simple xmax < OldestXmin test in
	 * HeapTupleSatisfiesVacuum just wasn't enough to detect it. It happens
	 * when xmin of a tuple is greater than xmax, which sounds
	 * counter-intuitive but is perfectly valid.
	 *
	 * We don't bother to try to detect the situation the other way round,
	 * when we encounter the dead tuple first and then the recently dead one
	 * that points to it. If that happens, we'll have some unmatched entries
	 * in the UnresolvedTups hash table at the end. That can happen anyway,
	 * because a vacuum might have removed the dead tuple in the chain before
	 * us.
	 */
	UnresolvedTup unresolved;
	TidHashKey	hashkey;
	bool		found;

	memset(&hashkey, 0, sizeof(hashkey));
	hashkey.xmin = HeapTupleHeaderGetXmin(old_tuple->t_data);
	hashkey.tid = old_tuple->t_self;

	unresolved = hash_search(state->rs_unresolved_tups, &hashkey,
							 HASH_FIND, NULL);

	if (unresolved != NULL)
	{
		/* Need to free the contained tuple as well as the hashtable entry */
		heap_freetuple(unresolved->tuple);
		hash_search(state->rs_unresolved_tups, &hashkey,
					HASH_REMOVE, &found);
		Assert(found);
		return true;
	}

	return false;
}

/*
 * Insert a tuple to the new relation.	This has to track heap_insert
 * and its subsidiary functions!
 *
 * t_self of the tuple is set to the new TID of the tuple. If t_ctid of the
 * tuple is invalid on entry, it's replaced with the new TID as well (in
 * the inserted data only, not in the caller's copy).
 */
static void
raw_heap_insert(RewriteState state, HeapTuple tup)
{
	Page		page = state->rs_buffer;
	Size		pageFreeSpace,
				saveFreeSpace;
	Size		len;
	OffsetNumber newoff;
	HeapTuple	heaptup;

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 *
	 * Note: below this point, heaptup is the data we actually intend to store
	 * into the relation; tup is the caller's original untoasted data.
	 */
	if (state->rs_new_rel->rd_rel->relkind == RELKIND_TOASTVALUE)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(tup));
		heaptup = tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		heaptup = toast_insert_or_update(state->rs_new_rel, tup, NULL,
										 HEAP_INSERT_SKIP_FSM |
										 (state->rs_use_wal ?
										  0 : HEAP_INSERT_SKIP_WAL));
	else
		heaptup = tup;

	len = MAXALIGN(heaptup->t_len);		/* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %lu, maximum size %lu",
						(unsigned long) len,
						(unsigned long) MaxHeapTupleSize)));

	/* Compute desired extra freespace due to fillfactor option */
	saveFreeSpace = RelationGetTargetPageFreeSpace(state->rs_new_rel,
												   HEAP_DEFAULT_FILLFACTOR);

	/* Now we can check to see if there's enough free space already. */
	if (state->rs_buffer_valid)
	{
		pageFreeSpace = PageGetHeapFreeSpace(page);

		if (len + saveFreeSpace > pageFreeSpace)
		{
			/* Doesn't fit, so write out the existing page */

			/* XLOG stuff */
			if (state->rs_use_wal)
				log_newpage(&state->rs_new_rel->rd_node,
							MAIN_FORKNUM,
							state->rs_blockno,
							page);

			/*
			 * Now write the page. We say isTemp = true even if it's not a
			 * temp table, because there's no need for smgr to schedule an
			 * fsync for this write; we'll do it ourselves in
			 * end_heap_rewrite.
			 */
			RelationOpenSmgr(state->rs_new_rel);

			PageSetChecksumInplace(page, state->rs_blockno);

			smgrextend(state->rs_new_rel->rd_smgr, MAIN_FORKNUM,
					   state->rs_blockno, (char *) page, true);

			state->rs_blockno++;
			state->rs_buffer_valid = false;
		}
	}

	if (!state->rs_buffer_valid)
	{
		/* Initialize a new empty page */
		PageInit(page, BLCKSZ, 0);
		state->rs_buffer_valid = true;
	}

	/* And now we can insert the tuple into the page */
	newoff = PageAddItem(page, (Item) heaptup->t_data, heaptup->t_len,
						 InvalidOffsetNumber, false, true);
	if (newoff == InvalidOffsetNumber)
		elog(ERROR, "failed to add tuple");

	/* Update caller's t_self to the actual position where it was stored */
	ItemPointerSet(&(tup->t_self), state->rs_blockno, newoff);

	/*
	 * Insert the correct position into CTID of the stored tuple, too, if the
	 * caller didn't supply a valid CTID.
	 */
	if (!ItemPointerIsValid(&tup->t_data->t_ctid))
	{
		ItemId		newitemid;
		HeapTupleHeader onpage_tup;

		newitemid = PageGetItemId(page, newoff);
		onpage_tup = (HeapTupleHeader) PageGetItem(page, newitemid);

		onpage_tup->t_ctid = tup->t_self;
	}

	/* If heaptup is a private copy, release it. */
	if (heaptup != tup)
		heap_freetuple(heaptup);
}
