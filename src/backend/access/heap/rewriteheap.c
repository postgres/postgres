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
 * rebuilding indexes.  The caller must hold AccessExclusiveLock on the
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
 * or B first.  (Note that a tuple in the middle of a chain is both A and B
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
 * is encountered.  That helps to keep the memory usage down.  At the end,
 * both tables are usually empty; we should have encountered both A and B
 * of each pair.  However, it's possible for A to be RECENTLY_DEAD and B
 * entirely DEAD according to HeapTupleSatisfiesVacuum, because the test
 * for deadness using OldestXmin is not exact.  In such a case we might
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
 * in the whole table.  Note that if we do fail halfway through a CLUSTER,
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
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/rewriteheap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/heaptoast.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/slot.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * State associated with a rewrite operation. This is opaque to the user
 * of the rewrite facility.
 */
typedef struct RewriteStateData
{
	Relation	rs_old_rel;		/* source heap */
	Relation	rs_new_rel;		/* destination heap */
	Page		rs_buffer;		/* page currently being built */
	BlockNumber rs_blockno;		/* block where page will go */
	bool		rs_buffer_valid;	/* T if any tuples in buffer */
	bool		rs_logical_rewrite; /* do we need to do logical rewriting */
	TransactionId rs_oldest_xmin;	/* oldest xmin used by caller to determine
									 * tuple visibility */
	TransactionId rs_freeze_xid;	/* Xid that will be used as freeze cutoff
									 * point */
	TransactionId rs_logical_xmin;	/* Xid that will be used as cutoff point
									 * for logical rewrites */
	MultiXactId rs_cutoff_multi;	/* MultiXactId that will be used as cutoff
									 * point for multixacts */
	MemoryContext rs_cxt;		/* for hash tables and entries and tuples in
								 * them */
	XLogRecPtr	rs_begin_lsn;	/* XLogInsertLsn when starting the rewrite */
	HTAB	   *rs_unresolved_tups; /* unmatched A tuples */
	HTAB	   *rs_old_new_tid_map; /* unmatched B tuples */
	HTAB	   *rs_logical_mappings;	/* logical remapping files */
	uint32		rs_num_rewrite_mappings;	/* # in memory mappings */
}			RewriteStateData;

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

/*
 * In-Memory data for an xid that might need logical remapping entries
 * to be logged.
 */
typedef struct RewriteMappingFile
{
	TransactionId xid;			/* xid that might need to see the row */
	int			vfd;			/* fd of mappings file */
	off_t		off;			/* how far have we written yet */
	uint32		num_mappings;	/* number of in-memory mappings */
	dlist_head	mappings;		/* list of in-memory mappings */
	char		path[MAXPGPATH];	/* path, for error messages */
} RewriteMappingFile;

/*
 * A single In-Memory logical rewrite mapping, hanging off
 * RewriteMappingFile->mappings.
 */
typedef struct RewriteMappingDataEntry
{
	LogicalRewriteMappingData map;	/* map between old and new location of the
									 * tuple */
	dlist_node	node;
} RewriteMappingDataEntry;


/* prototypes for internal functions */
static void raw_heap_insert(RewriteState state, HeapTuple tup);

/* internal logical remapping prototypes */
static void logical_begin_heap_rewrite(RewriteState state);
static void logical_rewrite_heap_tuple(RewriteState state, ItemPointerData old_tid, HeapTuple new_tuple);
static void logical_end_heap_rewrite(RewriteState state);


/*
 * Begin a rewrite of a table
 *
 * old_heap		old, locked heap relation tuples will be read from
 * new_heap		new, locked heap relation to insert tuples to
 * oldest_xmin	xid used by the caller to determine which tuples are dead
 * freeze_xid	xid before which tuples will be frozen
 * cutoff_multi	multixact before which multis will be removed
 *
 * Returns an opaque RewriteState, allocated in current memory context,
 * to be used in subsequent calls to the other functions.
 */
RewriteState
begin_heap_rewrite(Relation old_heap, Relation new_heap, TransactionId oldest_xmin,
				   TransactionId freeze_xid, MultiXactId cutoff_multi)
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
								   ALLOCSET_DEFAULT_SIZES);
	old_cxt = MemoryContextSwitchTo(rw_cxt);

	/* Create and fill in the state struct */
	state = palloc0(sizeof(RewriteStateData));

	state->rs_old_rel = old_heap;
	state->rs_new_rel = new_heap;
	state->rs_buffer = (Page) palloc(BLCKSZ);
	/* new_heap needn't be empty, just locked */
	state->rs_blockno = RelationGetNumberOfBlocks(new_heap);
	state->rs_buffer_valid = false;
	state->rs_oldest_xmin = oldest_xmin;
	state->rs_freeze_xid = freeze_xid;
	state->rs_cutoff_multi = cutoff_multi;
	state->rs_cxt = rw_cxt;

	/* Initialize hash tables used to track update chains */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(TidHashKey);
	hash_ctl.entrysize = sizeof(UnresolvedTupData);
	hash_ctl.hcxt = state->rs_cxt;

	state->rs_unresolved_tups =
		hash_create("Rewrite / Unresolved ctids",
					128,		/* arbitrary initial size */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	hash_ctl.entrysize = sizeof(OldToNewMappingData);

	state->rs_old_new_tid_map =
		hash_create("Rewrite / Old to new tid map",
					128,		/* arbitrary initial size */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	MemoryContextSwitchTo(old_cxt);

	logical_begin_heap_rewrite(state);

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
		if (RelationNeedsWAL(state->rs_new_rel))
			log_newpage(&state->rs_new_rel->rd_node,
						MAIN_FORKNUM,
						state->rs_blockno,
						state->rs_buffer,
						true);
		RelationOpenSmgr(state->rs_new_rel);

		PageSetChecksumInplace(state->rs_buffer, state->rs_blockno);

		smgrextend(state->rs_new_rel->rd_smgr, MAIN_FORKNUM, state->rs_blockno,
				   (char *) state->rs_buffer, true);
	}

	/*
	 * When we WAL-logged rel pages, we must nonetheless fsync them.  The
	 * reason is the same as in storage.c's RelationCopyStorage(): we're
	 * writing data that's not in shared buffers, and so a CHECKPOINT
	 * occurring during the rewriteheap operation won't have fsync'd data we
	 * wrote before the checkpoint.
	 */
	if (RelationNeedsWAL(state->rs_new_rel))
		smgrimmedsync(state->rs_new_rel->rd_smgr, MAIN_FORKNUM);

	logical_end_heap_rewrite(state);

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
	 * eligible xmin or xmax, so that future VACUUM effort can be saved.
	 */
	heap_freeze_tuple(new_tuple->t_data,
					  state->rs_old_rel->rd_rel->relfrozenxid,
					  state->rs_old_rel->rd_rel->relminmxid,
					  state->rs_freeze_xid,
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
		!HeapTupleHeaderIndicatesMovedPartitions(old_tuple->t_data) &&
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

		logical_rewrite_heap_tuple(state, old_tid, new_tuple);

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
 * Insert a tuple to the new relation.  This has to track heap_insert
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
	{
		int			options = HEAP_INSERT_SKIP_FSM;

		/*
		 * While rewriting the heap for VACUUM FULL / CLUSTER, make sure data
		 * for the TOAST table are not logically decoded.  The main heap is
		 * WAL-logged as XLOG FPI records, which are not logically decoded.
		 */
		options |= HEAP_INSERT_NO_LOGICAL;

		heaptup = heap_toast_insert_or_update(state->rs_new_rel, tup, NULL,
											  options);
	}
	else
		heaptup = tup;

	len = MAXALIGN(heaptup->t_len); /* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, MaxHeapTupleSize)));

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
			if (RelationNeedsWAL(state->rs_new_rel))
				log_newpage(&state->rs_new_rel->rd_node,
							MAIN_FORKNUM,
							state->rs_blockno,
							page,
							true);

			/*
			 * Now write the page. We say skipFsync = true because there's no
			 * need for smgr to schedule an fsync for this write; we'll do it
			 * ourselves in end_heap_rewrite.
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

/* ------------------------------------------------------------------------
 * Logical rewrite support
 *
 * When doing logical decoding - which relies on using cmin/cmax of catalog
 * tuples, via xl_heap_new_cid records - heap rewrites have to log enough
 * information to allow the decoding backend to updates its internal mapping
 * of (relfilenode,ctid) => (cmin, cmax) to be correct for the rewritten heap.
 *
 * For that, every time we find a tuple that's been modified in a catalog
 * relation within the xmin horizon of any decoding slot, we log a mapping
 * from the old to the new location.
 *
 * To deal with rewrites that abort the filename of a mapping file contains
 * the xid of the transaction performing the rewrite, which then can be
 * checked before being read in.
 *
 * For efficiency we don't immediately spill every single map mapping for a
 * row to disk but only do so in batches when we've collected several of them
 * in memory or when end_heap_rewrite() has been called.
 *
 * Crash-Safety: This module diverts from the usual patterns of doing WAL
 * since it cannot rely on checkpoint flushing out all buffers and thus
 * waiting for exclusive locks on buffers. Usually the XLogInsert() covering
 * buffer modifications is performed while the buffer(s) that are being
 * modified are exclusively locked guaranteeing that both the WAL record and
 * the modified heap are on either side of the checkpoint. But since the
 * mapping files we log aren't in shared_buffers that interlock doesn't work.
 *
 * Instead we simply write the mapping files out to disk, *before* the
 * XLogInsert() is performed. That guarantees that either the XLogInsert() is
 * inserted after the checkpoint's redo pointer or that the checkpoint (via
 * CheckPointLogicalRewriteHeap()) has flushed the (partial) mapping file to
 * disk. That leaves the tail end that has not yet been flushed open to
 * corruption, which is solved by including the current offset in the
 * xl_heap_rewrite_mapping records and truncating the mapping file to it
 * during replay. Every time a rewrite is finished all generated mapping files
 * are synced to disk.
 *
 * Note that if we were only concerned about crash safety we wouldn't have to
 * deal with WAL logging at all - an fsync() at the end of a rewrite would be
 * sufficient for crash safety. Any mapping that hasn't been safely flushed to
 * disk has to be by an aborted (explicitly or via a crash) transaction and is
 * ignored by virtue of the xid in its name being subject to a
 * TransactionDidCommit() check. But we want to support having standbys via
 * physical replication, both for availability and to do logical decoding
 * there.
 * ------------------------------------------------------------------------
 */

/*
 * Do preparations for logging logical mappings during a rewrite if
 * necessary. If we detect that we don't need to log anything we'll prevent
 * any further action by the various logical rewrite functions.
 */
static void
logical_begin_heap_rewrite(RewriteState state)
{
	HASHCTL		hash_ctl;
	TransactionId logical_xmin;

	/*
	 * We only need to persist these mappings if the rewritten table can be
	 * accessed during logical decoding, if not, we can skip doing any
	 * additional work.
	 */
	state->rs_logical_rewrite =
		RelationIsAccessibleInLogicalDecoding(state->rs_old_rel);

	if (!state->rs_logical_rewrite)
		return;

	ProcArrayGetReplicationSlotXmin(NULL, &logical_xmin);

	/*
	 * If there are no logical slots in progress we don't need to do anything,
	 * there cannot be any remappings for relevant rows yet. The relation's
	 * lock protects us against races.
	 */
	if (logical_xmin == InvalidTransactionId)
	{
		state->rs_logical_rewrite = false;
		return;
	}

	state->rs_logical_xmin = logical_xmin;
	state->rs_begin_lsn = GetXLogInsertRecPtr();
	state->rs_num_rewrite_mappings = 0;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(RewriteMappingFile);
	hash_ctl.hcxt = state->rs_cxt;

	state->rs_logical_mappings =
		hash_create("Logical rewrite mapping",
					128,		/* arbitrary initial size */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Flush all logical in-memory mappings to disk, but don't fsync them yet.
 */
static void
logical_heap_rewrite_flush_mappings(RewriteState state)
{
	HASH_SEQ_STATUS seq_status;
	RewriteMappingFile *src;
	dlist_mutable_iter iter;

	Assert(state->rs_logical_rewrite);

	/* no logical rewrite in progress, no need to iterate over mappings */
	if (state->rs_num_rewrite_mappings == 0)
		return;

	elog(DEBUG1, "flushing %u logical rewrite mapping entries",
		 state->rs_num_rewrite_mappings);

	hash_seq_init(&seq_status, state->rs_logical_mappings);
	while ((src = (RewriteMappingFile *) hash_seq_search(&seq_status)) != NULL)
	{
		char	   *waldata;
		char	   *waldata_start;
		xl_heap_rewrite_mapping xlrec;
		Oid			dboid;
		uint32		len;
		int			written;

		/* this file hasn't got any new mappings */
		if (src->num_mappings == 0)
			continue;

		if (state->rs_old_rel->rd_rel->relisshared)
			dboid = InvalidOid;
		else
			dboid = MyDatabaseId;

		xlrec.num_mappings = src->num_mappings;
		xlrec.mapped_rel = RelationGetRelid(state->rs_old_rel);
		xlrec.mapped_xid = src->xid;
		xlrec.mapped_db = dboid;
		xlrec.offset = src->off;
		xlrec.start_lsn = state->rs_begin_lsn;

		/* write all mappings consecutively */
		len = src->num_mappings * sizeof(LogicalRewriteMappingData);
		waldata_start = waldata = palloc(len);

		/*
		 * collect data we need to write out, but don't modify ondisk data yet
		 */
		dlist_foreach_modify(iter, &src->mappings)
		{
			RewriteMappingDataEntry *pmap;

			pmap = dlist_container(RewriteMappingDataEntry, node, iter.cur);

			memcpy(waldata, &pmap->map, sizeof(pmap->map));
			waldata += sizeof(pmap->map);

			/* remove from the list and free */
			dlist_delete(&pmap->node);
			pfree(pmap);

			/* update bookkeeping */
			state->rs_num_rewrite_mappings--;
			src->num_mappings--;
		}

		Assert(src->num_mappings == 0);
		Assert(waldata == waldata_start + len);

		/*
		 * Note that we deviate from the usual WAL coding practices here,
		 * check the above "Logical rewrite support" comment for reasoning.
		 */
		written = FileWrite(src->vfd, waldata_start, len, src->off,
							WAIT_EVENT_LOGICAL_REWRITE_WRITE);
		if (written != len)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\", wrote %d of %d: %m", src->path,
							written, len)));
		src->off += len;

		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec), sizeof(xlrec));
		XLogRegisterData(waldata_start, len);

		/* write xlog record */
		XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_REWRITE);

		pfree(waldata_start);
	}
	Assert(state->rs_num_rewrite_mappings == 0);
}

/*
 * Logical remapping part of end_heap_rewrite().
 */
static void
logical_end_heap_rewrite(RewriteState state)
{
	HASH_SEQ_STATUS seq_status;
	RewriteMappingFile *src;

	/* done, no logical rewrite in progress */
	if (!state->rs_logical_rewrite)
		return;

	/* writeout remaining in-memory entries */
	if (state->rs_num_rewrite_mappings > 0)
		logical_heap_rewrite_flush_mappings(state);

	/* Iterate over all mappings we have written and fsync the files. */
	hash_seq_init(&seq_status, state->rs_logical_mappings);
	while ((src = (RewriteMappingFile *) hash_seq_search(&seq_status)) != NULL)
	{
		if (FileSync(src->vfd, WAIT_EVENT_LOGICAL_REWRITE_SYNC) != 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", src->path)));
		FileClose(src->vfd);
	}
	/* memory context cleanup will deal with the rest */
}

/*
 * Log a single (old->new) mapping for 'xid'.
 */
static void
logical_rewrite_log_mapping(RewriteState state, TransactionId xid,
							LogicalRewriteMappingData *map)
{
	RewriteMappingFile *src;
	RewriteMappingDataEntry *pmap;
	Oid			relid;
	bool		found;

	relid = RelationGetRelid(state->rs_old_rel);

	/* look for existing mappings for this 'mapped' xid */
	src = hash_search(state->rs_logical_mappings, &xid,
					  HASH_ENTER, &found);

	/*
	 * We haven't yet had the need to map anything for this xid, create
	 * per-xid data structures.
	 */
	if (!found)
	{
		char		path[MAXPGPATH];
		Oid			dboid;

		if (state->rs_old_rel->rd_rel->relisshared)
			dboid = InvalidOid;
		else
			dboid = MyDatabaseId;

		snprintf(path, MAXPGPATH,
				 "pg_logical/mappings/" LOGICAL_REWRITE_FORMAT,
				 dboid, relid,
				 (uint32) (state->rs_begin_lsn >> 32),
				 (uint32) state->rs_begin_lsn,
				 xid, GetCurrentTransactionId());

		dlist_init(&src->mappings);
		src->num_mappings = 0;
		src->off = 0;
		memcpy(src->path, path, sizeof(path));
		src->vfd = PathNameOpenFile(path,
									O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
		if (src->vfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path)));
	}

	pmap = MemoryContextAlloc(state->rs_cxt,
							  sizeof(RewriteMappingDataEntry));
	memcpy(&pmap->map, map, sizeof(LogicalRewriteMappingData));
	dlist_push_tail(&src->mappings, &pmap->node);
	src->num_mappings++;
	state->rs_num_rewrite_mappings++;

	/*
	 * Write out buffer every time we've too many in-memory entries across all
	 * mapping files.
	 */
	if (state->rs_num_rewrite_mappings >= 1000 /* arbitrary number */ )
		logical_heap_rewrite_flush_mappings(state);
}

/*
 * Perform logical remapping for a tuple that's mapped from old_tid to
 * new_tuple->t_self by rewrite_heap_tuple() if necessary for the tuple.
 */
static void
logical_rewrite_heap_tuple(RewriteState state, ItemPointerData old_tid,
						   HeapTuple new_tuple)
{
	ItemPointerData new_tid = new_tuple->t_self;
	TransactionId cutoff = state->rs_logical_xmin;
	TransactionId xmin;
	TransactionId xmax;
	bool		do_log_xmin = false;
	bool		do_log_xmax = false;
	LogicalRewriteMappingData map;

	/* no logical rewrite in progress, we don't need to log anything */
	if (!state->rs_logical_rewrite)
		return;

	xmin = HeapTupleHeaderGetXmin(new_tuple->t_data);
	/* use *GetUpdateXid to correctly deal with multixacts */
	xmax = HeapTupleHeaderGetUpdateXid(new_tuple->t_data);

	/*
	 * Log the mapping iff the tuple has been created recently.
	 */
	if (TransactionIdIsNormal(xmin) && !TransactionIdPrecedes(xmin, cutoff))
		do_log_xmin = true;

	if (!TransactionIdIsNormal(xmax))
	{
		/*
		 * no xmax is set, can't have any permanent ones, so this check is
		 * sufficient
		 */
	}
	else if (HEAP_XMAX_IS_LOCKED_ONLY(new_tuple->t_data->t_infomask))
	{
		/* only locked, we don't care */
	}
	else if (!TransactionIdPrecedes(xmax, cutoff))
	{
		/* tuple has been deleted recently, log */
		do_log_xmax = true;
	}

	/* if neither needs to be logged, we're done */
	if (!do_log_xmin && !do_log_xmax)
		return;

	/* fill out mapping information */
	map.old_node = state->rs_old_rel->rd_node;
	map.old_tid = old_tid;
	map.new_node = state->rs_new_rel->rd_node;
	map.new_tid = new_tid;

	/* ---
	 * Now persist the mapping for the individual xids that are affected. We
	 * need to log for both xmin and xmax if they aren't the same transaction
	 * since the mapping files are per "affected" xid.
	 * We don't muster all that much effort detecting whether xmin and xmax
	 * are actually the same transaction, we just check whether the xid is the
	 * same disregarding subtransactions. Logging too much is relatively
	 * harmless and we could never do the check fully since subtransaction
	 * data is thrown away during restarts.
	 * ---
	 */
	if (do_log_xmin)
		logical_rewrite_log_mapping(state, xmin, &map);
	/* separately log mapping for xmax unless it'd be redundant */
	if (do_log_xmax && !TransactionIdEquals(xmin, xmax))
		logical_rewrite_log_mapping(state, xmax, &map);
}

/*
 * Replay XLOG_HEAP2_REWRITE records
 */
void
heap_xlog_logical_rewrite(XLogReaderState *r)
{
	char		path[MAXPGPATH];
	int			fd;
	xl_heap_rewrite_mapping *xlrec;
	uint32		len;
	char	   *data;

	xlrec = (xl_heap_rewrite_mapping *) XLogRecGetData(r);

	snprintf(path, MAXPGPATH,
			 "pg_logical/mappings/" LOGICAL_REWRITE_FORMAT,
			 xlrec->mapped_db, xlrec->mapped_rel,
			 (uint32) (xlrec->start_lsn >> 32),
			 (uint32) xlrec->start_lsn,
			 xlrec->mapped_xid, XLogRecGetXid(r));

	fd = OpenTransientFile(path,
						   O_CREAT | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	/*
	 * Truncate all data that's not guaranteed to have been safely fsynced (by
	 * previous record or by the last checkpoint).
	 */
	pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_TRUNCATE);
	if (ftruncate(fd, xlrec->offset) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\" to %u: %m",
						path, (uint32) xlrec->offset)));
	pgstat_report_wait_end();

	data = XLogRecGetData(r) + sizeof(*xlrec);

	len = xlrec->num_mappings * sizeof(LogicalRewriteMappingData);

	/* write out tail end of mapping file (again) */
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_MAPPING_WRITE);
	if (pg_pwrite(fd, data, len, xlrec->offset) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", path)));
	}
	pgstat_report_wait_end();

	/*
	 * Now fsync all previously written data. We could improve things and only
	 * do this for the last write to a file, but the required bookkeeping
	 * doesn't seem worth the trouble.
	 */
	pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_MAPPING_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/* ---
 * Perform a checkpoint for logical rewrite mappings
 *
 * This serves two tasks:
 * 1) Remove all mappings not needed anymore based on the logical restart LSN
 * 2) Flush all remaining mappings to disk, so that replay after a checkpoint
 *	  only has to deal with the parts of a mapping that have been written out
 *	  after the checkpoint started.
 * ---
 */
void
CheckPointLogicalRewriteHeap(void)
{
	XLogRecPtr	cutoff;
	XLogRecPtr	redo;
	DIR		   *mappings_dir;
	struct dirent *mapping_de;
	char		path[MAXPGPATH + 20];

	/*
	 * We start of with a minimum of the last redo pointer. No new decoding
	 * slot will start before that, so that's a safe upper bound for removal.
	 */
	redo = GetRedoRecPtr();

	/* now check for the restart ptrs from existing slots */
	cutoff = ReplicationSlotsComputeLogicalRestartLSN();

	/* don't start earlier than the restart lsn */
	if (cutoff != InvalidXLogRecPtr && redo < cutoff)
		cutoff = redo;

	mappings_dir = AllocateDir("pg_logical/mappings");
	while ((mapping_de = ReadDir(mappings_dir, "pg_logical/mappings")) != NULL)
	{
		struct stat statbuf;
		Oid			dboid;
		Oid			relid;
		XLogRecPtr	lsn;
		TransactionId rewrite_xid;
		TransactionId create_xid;
		uint32		hi,
					lo;

		if (strcmp(mapping_de->d_name, ".") == 0 ||
			strcmp(mapping_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "pg_logical/mappings/%s", mapping_de->d_name);
		if (lstat(path, &statbuf) == 0 && !S_ISREG(statbuf.st_mode))
			continue;

		/* Skip over files that cannot be ours. */
		if (strncmp(mapping_de->d_name, "map-", 4) != 0)
			continue;

		if (sscanf(mapping_de->d_name, LOGICAL_REWRITE_FORMAT,
				   &dboid, &relid, &hi, &lo, &rewrite_xid, &create_xid) != 6)
			elog(ERROR, "could not parse filename \"%s\"", mapping_de->d_name);

		lsn = ((uint64) hi) << 32 | lo;

		if (lsn < cutoff || cutoff == InvalidXLogRecPtr)
		{
			elog(DEBUG1, "removing logical rewrite file \"%s\"", path);
			if (unlink(path) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}
		else
		{
			/* on some operating systems fsyncing a file requires O_RDWR */
			int			fd = OpenTransientFile(path, O_RDWR | PG_BINARY);

			/*
			 * The file cannot vanish due to concurrency since this function
			 * is the only one removing logical mappings and it's run while
			 * CheckpointLock is held exclusively.
			 */
			if (fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));

			/*
			 * We could try to avoid fsyncing files that either haven't
			 * changed or have only been created since the checkpoint's start,
			 * but it's currently not deemed worth the effort.
			 */
			pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_CHECKPOINT_SYNC);
			if (pg_fsync(fd) != 0)
				ereport(data_sync_elevel(ERROR),
						(errcode_for_file_access(),
						 errmsg("could not fsync file \"%s\": %m", path)));
			pgstat_report_wait_end();

			if (CloseTransientFile(fd) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not close file \"%s\": %m", path)));
		}
	}
	FreeDir(mappings_dir);
}
