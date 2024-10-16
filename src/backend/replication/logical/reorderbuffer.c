/*-------------------------------------------------------------------------
 *
 * reorderbuffer.c
 *	  PostgreSQL logical replay/reorder buffer management
 *
 *
 * Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/reorderbuffer.c
 *
 * NOTES
 *	  This module gets handed individual pieces of transactions in the order
 *	  they are written to the WAL and is responsible to reassemble them into
 *	  toplevel transaction sized pieces. When a transaction is completely
 *	  reassembled - signalled by reading the transaction commit record - it
 *	  will then call the output plugin (cf. ReorderBufferCommit()) with the
 *	  individual changes. The output plugins rely on snapshots built by
 *	  snapbuild.c which hands them to us.
 *
 *	  Transactions and subtransactions/savepoints in postgres are not
 *	  immediately linked to each other from outside the performing
 *	  backend. Only at commit/abort (or special xact_assignment records) they
 *	  are linked together. Which means that we will have to splice together a
 *	  toplevel transaction from its subtransactions. To do that efficiently we
 *	  build a binary heap indexed by the smallest current lsn of the individual
 *	  subtransactions' changestreams. As the individual streams are inherently
 *	  ordered by LSN - since that is where we build them from - the transaction
 *	  can easily be reassembled by always using the subtransaction with the
 *	  smallest current LSN from the heap.
 *
 *	  In order to cope with large transactions - which can be several times as
 *	  big as the available memory - this module supports spooling the contents
 *	  of a large transactions to disk. When the transaction is replayed the
 *	  contents of individual (sub-)transactions will be read from disk in
 *	  chunks.
 *
 *	  This module also has to deal with reassembling toast records from the
 *	  individual chunks stored in WAL. When a new (or initial) version of a
 *	  tuple is stored in WAL it will always be preceded by the toast chunks
 *	  emitted for the columns stored out of line. Within a single toplevel
 *	  transaction there will be no other data carrying records between a row's
 *	  toast chunks and the row data itself. See ReorderBufferToast* for
 *	  details.
 *
 *	  ReorderBuffer uses two special memory context types - SlabContext for
 *	  allocations of fixed-length structures (changes and transactions), and
 *	  GenerationContext for the variable-length transaction data (allocated
 *	  and freed in groups with similar lifespan).
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"	/* just for SnapBuildSnapDecRefcount */
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relfilenodemap.h"


/* entry for a hash table we use to map from xid to our transaction state */
typedef struct ReorderBufferTXNByIdEnt
{
	TransactionId xid;
	ReorderBufferTXN *txn;
} ReorderBufferTXNByIdEnt;

/* data structures for (relfilenode, ctid) => (cmin, cmax) mapping */
typedef struct ReorderBufferTupleCidKey
{
	RelFileNode relnode;
	ItemPointerData tid;
} ReorderBufferTupleCidKey;

typedef struct ReorderBufferTupleCidEnt
{
	ReorderBufferTupleCidKey key;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* just for debugging */
} ReorderBufferTupleCidEnt;

/* Virtual file descriptor with file offset tracking */
typedef struct TXNEntryFile
{
	File		vfd;			/* -1 when the file is closed */
	off_t		curOffset;		/* offset for next write or read. Reset to 0
								 * when vfd is opened. */
} TXNEntryFile;

/* k-way in-order change iteration support structures */
typedef struct ReorderBufferIterTXNEntry
{
	XLogRecPtr	lsn;
	ReorderBufferChange *change;
	ReorderBufferTXN *txn;
	TXNEntryFile file;
	XLogSegNo	segno;
} ReorderBufferIterTXNEntry;

typedef struct ReorderBufferIterTXNState
{
	binaryheap *heap;
	Size		nr_txns;
	dlist_head	old_change;
	ReorderBufferIterTXNEntry entries[FLEXIBLE_ARRAY_MEMBER];
} ReorderBufferIterTXNState;

/* toast datastructures */
typedef struct ReorderBufferToastEnt
{
	Oid			chunk_id;		/* toast_table.chunk_id */
	int32		last_chunk_seq; /* toast_table.chunk_seq of the last chunk we
								 * have seen */
	Size		num_chunks;		/* number of chunks we've already seen */
	Size		size;			/* combined size of chunks seen */
	dlist_head	chunks;			/* linked list of chunks */
	struct varlena *reconstructed;	/* reconstructed varlena now pointed to in
									 * main tup */
} ReorderBufferToastEnt;

/* Disk serialization support datastructures */
typedef struct ReorderBufferDiskChange
{
	Size		size;
	ReorderBufferChange change;
	/* data follows */
} ReorderBufferDiskChange;

/*
 * Maximum number of changes kept in memory, per transaction. After that,
 * changes are spooled to disk.
 *
 * The current value should be sufficient to decode the entire transaction
 * without hitting disk in OLTP workloads, while starting to spool to disk in
 * other workloads reasonably fast.
 *
 * At some point in the future it probably makes sense to have a more elaborate
 * resource management here, but it's not entirely clear what that would look
 * like.
 */
static const Size max_changes_in_memory = 4096;

/* ---------------------------------------
 * primary reorderbuffer support routines
 * ---------------------------------------
 */
static ReorderBufferTXN *ReorderBufferGetTXN(ReorderBuffer *rb);
static void ReorderBufferReturnTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static ReorderBufferTXN *ReorderBufferTXNByXid(ReorderBuffer *rb,
											   TransactionId xid, bool create, bool *is_new,
											   XLogRecPtr lsn, bool create_as_top);
static void ReorderBufferTransferSnapToParent(ReorderBufferTXN *txn,
											  ReorderBufferTXN *subtxn);

static void AssertTXNLsnOrder(ReorderBuffer *rb);

/* ---------------------------------------
 * support functions for lsn-order iterating over the ->changes of a
 * transaction and its subtransactions
 *
 * used for iteration over the k-way heap merge of a transaction and its
 * subtransactions
 * ---------------------------------------
 */
static void ReorderBufferIterTXNInit(ReorderBuffer *rb, ReorderBufferTXN *txn,
									 ReorderBufferIterTXNState *volatile *iter_state);
static ReorderBufferChange *ReorderBufferIterTXNNext(ReorderBuffer *rb, ReorderBufferIterTXNState *state);
static void ReorderBufferIterTXNFinish(ReorderBuffer *rb,
									   ReorderBufferIterTXNState *state);
static void ReorderBufferExecuteInvalidations(ReorderBuffer *rb, ReorderBufferTXN *txn);

/*
 * ---------------------------------------
 * Disk serialization support functions
 * ---------------------------------------
 */
static void ReorderBufferCheckSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferSerializeChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
										 int fd, ReorderBufferChange *change);
static Size ReorderBufferRestoreChanges(ReorderBuffer *rb, ReorderBufferTXN *txn,
										TXNEntryFile *file, XLogSegNo *segno);
static void ReorderBufferRestoreChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
									   char *change);
static void ReorderBufferRestoreCleanup(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferCleanupSerializedTXNs(const char *slotname);
static void ReorderBufferSerializedPath(char *path, ReplicationSlot *slot,
										TransactionId xid, XLogSegNo segno);

static void ReorderBufferFreeSnap(ReorderBuffer *rb, Snapshot snap);
static Snapshot ReorderBufferCopySnap(ReorderBuffer *rb, Snapshot orig_snap,
									  ReorderBufferTXN *txn, CommandId cid);

/* ---------------------------------------
 * toast reassembly support
 * ---------------------------------------
 */
static void ReorderBufferToastInitHash(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferToastReset(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferToastReplace(ReorderBuffer *rb, ReorderBufferTXN *txn,
									  Relation relation, ReorderBufferChange *change);
static void ReorderBufferToastAppendChunk(ReorderBuffer *rb, ReorderBufferTXN *txn,
										  Relation relation, ReorderBufferChange *change);


/*
 * Allocate a new ReorderBuffer and clean out any old serialized state from
 * prior ReorderBuffer instances for the same slot.
 */
ReorderBuffer *
ReorderBufferAllocate(void)
{
	ReorderBuffer *buffer;
	HASHCTL		hash_ctl;
	MemoryContext new_ctx;

	Assert(MyReplicationSlot != NULL);

	/* allocate memory in own context, to have better accountability */
	new_ctx = AllocSetContextCreate(CurrentMemoryContext,
									"ReorderBuffer",
									ALLOCSET_DEFAULT_SIZES);

	buffer =
		(ReorderBuffer *) MemoryContextAlloc(new_ctx, sizeof(ReorderBuffer));

	memset(&hash_ctl, 0, sizeof(hash_ctl));

	buffer->context = new_ctx;

	buffer->change_context = SlabContextCreate(new_ctx,
											   "Change",
											   SLAB_DEFAULT_BLOCK_SIZE,
											   sizeof(ReorderBufferChange));

	buffer->txn_context = SlabContextCreate(new_ctx,
											"TXN",
											SLAB_DEFAULT_BLOCK_SIZE,
											sizeof(ReorderBufferTXN));

	/*
	 * To minimize memory fragmentation caused by long-running transactions
	 * with changes spanning multiple memory blocks, we use a single
	 * fixed-size memory block for decoded tuple storage. The performance
	 * testing showed that the default memory block size maintains logical
	 * decoding performance without causing fragmentation due to concurrent
	 * transactions.
	 */
	buffer->tup_context = GenerationContextCreate(new_ctx,
												  "Tuples",
												  SLAB_DEFAULT_BLOCK_SIZE);

	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(ReorderBufferTXNByIdEnt);
	hash_ctl.hcxt = buffer->context;

	buffer->by_txn = hash_create("ReorderBufferByXid", 1000, &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	buffer->by_txn_last_xid = InvalidTransactionId;
	buffer->by_txn_last_txn = NULL;

	buffer->outbuf = NULL;
	buffer->outbufsize = 0;

	buffer->current_restart_decoding_lsn = InvalidXLogRecPtr;

	dlist_init(&buffer->toplevel_by_lsn);
	dlist_init(&buffer->txns_by_base_snapshot_lsn);

	/*
	 * Ensure there's no stale data from prior uses of this slot, in case some
	 * prior exit avoided calling ReorderBufferFree. Failure to do this can
	 * produce duplicated txns, and it's very cheap if there's nothing there.
	 */
	ReorderBufferCleanupSerializedTXNs(NameStr(MyReplicationSlot->data.name));

	return buffer;
}

/*
 * Free a ReorderBuffer
 */
void
ReorderBufferFree(ReorderBuffer *rb)
{
	MemoryContext context = rb->context;

	/*
	 * We free separately allocated data by entirely scrapping reorderbuffer's
	 * memory context.
	 */
	MemoryContextDelete(context);

	/* Free disk space used by unconsumed reorder buffers */
	ReorderBufferCleanupSerializedTXNs(NameStr(MyReplicationSlot->data.name));
}

/*
 * Get an unused, possibly preallocated, ReorderBufferTXN.
 */
static ReorderBufferTXN *
ReorderBufferGetTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	txn = (ReorderBufferTXN *)
		MemoryContextAlloc(rb->txn_context, sizeof(ReorderBufferTXN));

	memset(txn, 0, sizeof(ReorderBufferTXN));

	dlist_init(&txn->changes);
	dlist_init(&txn->tuplecids);
	dlist_init(&txn->subtxns);

	return txn;
}

/*
 * Free a ReorderBufferTXN.
 */
static void
ReorderBufferReturnTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/* clean the lookup cache if we were cached (quite likely) */
	if (rb->by_txn_last_xid == txn->xid)
	{
		rb->by_txn_last_xid = InvalidTransactionId;
		rb->by_txn_last_txn = NULL;
	}

	/* free data that's contained */

	if (txn->tuplecid_hash != NULL)
	{
		hash_destroy(txn->tuplecid_hash);
		txn->tuplecid_hash = NULL;
	}

	if (txn->invalidations)
	{
		pfree(txn->invalidations);
		txn->invalidations = NULL;
	}

	/* Reset the toast hash */
	ReorderBufferToastReset(rb, txn);

	pfree(txn);
}

/*
 * Get an fresh ReorderBufferChange.
 */
ReorderBufferChange *
ReorderBufferGetChange(ReorderBuffer *rb)
{
	ReorderBufferChange *change;

	change = (ReorderBufferChange *)
		MemoryContextAlloc(rb->change_context, sizeof(ReorderBufferChange));

	memset(change, 0, sizeof(ReorderBufferChange));
	return change;
}

/*
 * Free an ReorderBufferChange.
 */
void
ReorderBufferReturnChange(ReorderBuffer *rb, ReorderBufferChange *change)
{
	/* free contained data */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			if (change->data.tp.newtuple)
			{
				ReorderBufferReturnTupleBuf(rb, change->data.tp.newtuple);
				change->data.tp.newtuple = NULL;
			}

			if (change->data.tp.oldtuple)
			{
				ReorderBufferReturnTupleBuf(rb, change->data.tp.oldtuple);
				change->data.tp.oldtuple = NULL;
			}
			break;
		case REORDER_BUFFER_CHANGE_MESSAGE:
			if (change->data.msg.prefix != NULL)
				pfree(change->data.msg.prefix);
			change->data.msg.prefix = NULL;
			if (change->data.msg.message != NULL)
				pfree(change->data.msg.message);
			change->data.msg.message = NULL;
			break;
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			if (change->data.snapshot)
			{
				ReorderBufferFreeSnap(rb, change->data.snapshot);
				change->data.snapshot = NULL;
			}
			break;
			/* no data in addition to the struct itself */
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			if (change->data.truncate.relids != NULL)
			{
				ReorderBufferReturnRelids(rb, change->data.truncate.relids);
				change->data.truncate.relids = NULL;
			}
			break;
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			break;
	}

	pfree(change);
}

/*
 * Get a fresh ReorderBufferTupleBuf fitting at least a tuple of size
 * tuple_len (excluding header overhead).
 */
ReorderBufferTupleBuf *
ReorderBufferGetTupleBuf(ReorderBuffer *rb, Size tuple_len)
{
	ReorderBufferTupleBuf *tuple;
	Size		alloc_len;

	alloc_len = tuple_len + SizeofHeapTupleHeader;

	tuple = (ReorderBufferTupleBuf *)
		MemoryContextAlloc(rb->tup_context,
						   sizeof(ReorderBufferTupleBuf) +
						   MAXIMUM_ALIGNOF + alloc_len);
	tuple->alloc_tuple_size = alloc_len;
	tuple->tuple.t_data = ReorderBufferTupleBufData(tuple);

	return tuple;
}

/*
 * Free an ReorderBufferTupleBuf.
 */
void
ReorderBufferReturnTupleBuf(ReorderBuffer *rb, ReorderBufferTupleBuf *tuple)
{
	pfree(tuple);
}

/*
 * Get an array for relids of truncated relations.
 *
 * We use the global memory context (for the whole reorder buffer), because
 * none of the existing ones seems like a good match (some are SLAB, so we
 * can't use those, and tup_context is meant for tuple data, not relids). We
 * could add yet another context, but it seems like an overkill - TRUNCATE is
 * not particularly common operation, so it does not seem worth it.
 */
Oid *
ReorderBufferGetRelids(ReorderBuffer *rb, int nrelids)
{
	Oid		   *relids;
	Size		alloc_len;

	alloc_len = sizeof(Oid) * nrelids;

	relids = (Oid *) MemoryContextAlloc(rb->context, alloc_len);

	return relids;
}

/*
 * Free an array of relids.
 */
void
ReorderBufferReturnRelids(ReorderBuffer *rb, Oid *relids)
{
	pfree(relids);
}

/*
 * Return the ReorderBufferTXN from the given buffer, specified by Xid.
 * If create is true, and a transaction doesn't already exist, create it
 * (with the given LSN, and as top transaction if that's specified);
 * when this happens, is_new is set to true.
 */
static ReorderBufferTXN *
ReorderBufferTXNByXid(ReorderBuffer *rb, TransactionId xid, bool create,
					  bool *is_new, XLogRecPtr lsn, bool create_as_top)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXNByIdEnt *ent;
	bool		found;

	Assert(TransactionIdIsValid(xid));

	/*
	 * Check the one-entry lookup cache first
	 */
	if (TransactionIdIsValid(rb->by_txn_last_xid) &&
		rb->by_txn_last_xid == xid)
	{
		txn = rb->by_txn_last_txn;

		if (txn != NULL)
		{
			/* found it, and it's valid */
			if (is_new)
				*is_new = false;
			return txn;
		}

		/*
		 * cached as non-existent, and asked not to create? Then nothing else
		 * to do.
		 */
		if (!create)
			return NULL;
		/* otherwise fall through to create it */
	}

	/*
	 * If the cache wasn't hit or it yielded an "does-not-exist" and we want
	 * to create an entry.
	 */

	/* search the lookup table */
	ent = (ReorderBufferTXNByIdEnt *)
		hash_search(rb->by_txn,
					(void *) &xid,
					create ? HASH_ENTER : HASH_FIND,
					&found);
	if (found)
		txn = ent->txn;
	else if (create)
	{
		/* initialize the new entry, if creation was requested */
		Assert(ent != NULL);
		Assert(lsn != InvalidXLogRecPtr);

		ent->txn = ReorderBufferGetTXN(rb);
		ent->txn->xid = xid;
		txn = ent->txn;
		txn->first_lsn = lsn;
		txn->restart_decoding_lsn = rb->current_restart_decoding_lsn;

		if (create_as_top)
		{
			dlist_push_tail(&rb->toplevel_by_lsn, &txn->node);
			AssertTXNLsnOrder(rb);
		}
	}
	else
		txn = NULL;				/* not found and not asked to create */

	/* update cache */
	rb->by_txn_last_xid = xid;
	rb->by_txn_last_txn = txn;

	if (is_new)
		*is_new = !found;

	Assert(!create || txn != NULL);
	return txn;
}

/*
 * Queue a change into a transaction so it can be replayed upon commit.
 */
void
ReorderBufferQueueChange(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
						 ReorderBufferChange *change)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	change->lsn = lsn;
	Assert(InvalidXLogRecPtr != lsn);
	dlist_push_tail(&txn->changes, &change->node);
	txn->nentries++;
	txn->nentries_mem++;

	ReorderBufferCheckSerializeTXN(rb, txn);
}

/*
 * Queue message into a transaction so it can be processed upon commit.
 */
void
ReorderBufferQueueMessage(ReorderBuffer *rb, TransactionId xid,
						  Snapshot snapshot, XLogRecPtr lsn,
						  bool transactional, const char *prefix,
						  Size message_size, const char *message)
{
	if (transactional)
	{
		MemoryContext oldcontext;
		ReorderBufferChange *change;

		Assert(xid != InvalidTransactionId);

		/*
		 * We don't expect snapshots for transactional changes - we'll use the
		 * snapshot derived later during apply (unless the change gets
		 * skipped).
		 */
		Assert(!snapshot);

		oldcontext = MemoryContextSwitchTo(rb->context);

		change = ReorderBufferGetChange(rb);
		change->action = REORDER_BUFFER_CHANGE_MESSAGE;
		change->data.msg.prefix = pstrdup(prefix);
		change->data.msg.message_size = message_size;
		change->data.msg.message = palloc(message_size);
		memcpy(change->data.msg.message, message, message_size);

		ReorderBufferQueueChange(rb, xid, lsn, change);

		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		ReorderBufferTXN *txn = NULL;
		volatile Snapshot snapshot_now = snapshot;

		/* Non-transactional changes require a valid snapshot. */
		Assert(snapshot_now);

		if (xid != InvalidTransactionId)
			txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

		/* setup snapshot to allow catalog access */
		SetupHistoricSnapshot(snapshot_now, NULL);
		PG_TRY();
		{
			rb->message(rb, txn, lsn, false, prefix, message_size, message);

			TeardownHistoricSnapshot(false);
		}
		PG_CATCH();
		{
			TeardownHistoricSnapshot(true);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
}

/*
 * AssertTXNLsnOrder
 *		Verify LSN ordering of transaction lists in the reorderbuffer
 *
 * Other LSN-related invariants are checked too.
 *
 * No-op if assertions are not in use.
 */
static void
AssertTXNLsnOrder(ReorderBuffer *rb)
{
#ifdef USE_ASSERT_CHECKING
	LogicalDecodingContext *ctx = rb->private_data;
	dlist_iter	iter;
	XLogRecPtr	prev_first_lsn = InvalidXLogRecPtr;
	XLogRecPtr	prev_base_snap_lsn = InvalidXLogRecPtr;

	/*
	 * Skip the verification if we don't reach the LSN at which we start
	 * decoding the contents of transactions yet because until we reach the
	 * LSN, we could have transactions that don't have the association between
	 * the top-level transaction and subtransaction yet and consequently have
	 * the same LSN.  We don't guarantee this association until we try to
	 * decode the actual contents of transaction. The ordering of the records
	 * prior to the start_decoding_at LSN should have been checked before the
	 * restart.
	 */
	if (SnapBuildXactNeedsSkip(ctx->snapshot_builder, ctx->reader->EndRecPtr))
		return;

	dlist_foreach(iter, &rb->toplevel_by_lsn)
	{
		ReorderBufferTXN *cur_txn = dlist_container(ReorderBufferTXN, node,
													iter.cur);

		/* start LSN must be set */
		Assert(cur_txn->first_lsn != InvalidXLogRecPtr);

		/* If there is an end LSN, it must be higher than start LSN */
		if (cur_txn->end_lsn != InvalidXLogRecPtr)
			Assert(cur_txn->first_lsn <= cur_txn->end_lsn);

		/* Current initial LSN must be strictly higher than previous */
		if (prev_first_lsn != InvalidXLogRecPtr)
			Assert(prev_first_lsn < cur_txn->first_lsn);

		/* known-as-subtxn txns must not be listed */
		Assert(!cur_txn->is_known_as_subxact);

		prev_first_lsn = cur_txn->first_lsn;
	}

	dlist_foreach(iter, &rb->txns_by_base_snapshot_lsn)
	{
		ReorderBufferTXN *cur_txn = dlist_container(ReorderBufferTXN,
													base_snapshot_node,
													iter.cur);

		/* base snapshot (and its LSN) must be set */
		Assert(cur_txn->base_snapshot != NULL);
		Assert(cur_txn->base_snapshot_lsn != InvalidXLogRecPtr);

		/* current LSN must be strictly higher than previous */
		if (prev_base_snap_lsn != InvalidXLogRecPtr)
			Assert(prev_base_snap_lsn < cur_txn->base_snapshot_lsn);

		/* known-as-subtxn txns must not be listed */
		Assert(!cur_txn->is_known_as_subxact);

		prev_base_snap_lsn = cur_txn->base_snapshot_lsn;
	}
#endif
}

/*
 * ReorderBufferGetOldestTXN
 *		Return oldest transaction in reorderbuffer
 */
ReorderBufferTXN *
ReorderBufferGetOldestTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	AssertTXNLsnOrder(rb);

	if (dlist_is_empty(&rb->toplevel_by_lsn))
		return NULL;

	txn = dlist_head_element(ReorderBufferTXN, node, &rb->toplevel_by_lsn);

	Assert(!txn->is_known_as_subxact);
	Assert(txn->first_lsn != InvalidXLogRecPtr);
	return txn;
}

/*
 * ReorderBufferGetOldestXmin
 *		Return oldest Xmin in reorderbuffer
 *
 * Returns oldest possibly running Xid from the point of view of snapshots
 * used in the transactions kept by reorderbuffer, or InvalidTransactionId if
 * there are none.
 *
 * Since snapshots are assigned monotonically, this equals the Xmin of the
 * base snapshot with minimal base_snapshot_lsn.
 */
TransactionId
ReorderBufferGetOldestXmin(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	AssertTXNLsnOrder(rb);

	if (dlist_is_empty(&rb->txns_by_base_snapshot_lsn))
		return InvalidTransactionId;

	txn = dlist_head_element(ReorderBufferTXN, base_snapshot_node,
							 &rb->txns_by_base_snapshot_lsn);
	return txn->base_snapshot->xmin;
}

void
ReorderBufferSetRestartPoint(ReorderBuffer *rb, XLogRecPtr ptr)
{
	rb->current_restart_decoding_lsn = ptr;
}

/*
 * ReorderBufferAssignChild
 *
 * Make note that we know that subxid is a subtransaction of xid, seen as of
 * the given lsn.
 */
void
ReorderBufferAssignChild(ReorderBuffer *rb, TransactionId xid,
						 TransactionId subxid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXN *subtxn;
	bool		new_top;
	bool		new_sub;

	txn = ReorderBufferTXNByXid(rb, xid, true, &new_top, lsn, true);
	subtxn = ReorderBufferTXNByXid(rb, subxid, true, &new_sub, lsn, false);

	if (!new_sub)
	{
		if (subtxn->is_known_as_subxact)
		{
			/* already associated, nothing to do */
			return;
		}
		else
		{
			/*
			 * We already saw this transaction, but initially added it to the
			 * list of top-level txns.  Now that we know it's not top-level,
			 * remove it from there.
			 */
			dlist_delete(&subtxn->node);
		}
	}

	subtxn->is_known_as_subxact = true;
	subtxn->toplevel_xid = xid;
	Assert(subtxn->nsubtxns == 0);

	/* add to subtransaction list */
	dlist_push_tail(&txn->subtxns, &subtxn->node);
	txn->nsubtxns++;

	/* Possibly transfer the subtxn's snapshot to its top-level txn. */
	ReorderBufferTransferSnapToParent(txn, subtxn);

	/* Verify LSN-ordering invariant */
	AssertTXNLsnOrder(rb);
}

/*
 * ReorderBufferTransferSnapToParent
 *		Transfer base snapshot from subtxn to top-level txn, if needed
 *
 * This is done if the top-level txn doesn't have a base snapshot, or if the
 * subtxn's base snapshot has an earlier LSN than the top-level txn's base
 * snapshot's LSN.  This can happen if there are no changes in the toplevel
 * txn but there are some in the subtxn, or the first change in subtxn has
 * earlier LSN than first change in the top-level txn and we learned about
 * their kinship only now.
 *
 * The subtransaction's snapshot is cleared regardless of the transfer
 * happening, since it's not needed anymore in either case.
 *
 * We do this as soon as we become aware of their kinship, to avoid queueing
 * extra snapshots to txns known-as-subtxns -- only top-level txns will
 * receive further snapshots.
 */
static void
ReorderBufferTransferSnapToParent(ReorderBufferTXN *txn,
								  ReorderBufferTXN *subtxn)
{
	Assert(subtxn->toplevel_xid == txn->xid);

	if (subtxn->base_snapshot != NULL)
	{
		if (txn->base_snapshot == NULL ||
			subtxn->base_snapshot_lsn < txn->base_snapshot_lsn)
		{
			/*
			 * If the toplevel transaction already has a base snapshot but
			 * it's newer than the subxact's, purge it.
			 */
			if (txn->base_snapshot != NULL)
			{
				SnapBuildSnapDecRefcount(txn->base_snapshot);
				dlist_delete(&txn->base_snapshot_node);
			}

			/*
			 * The snapshot is now the top transaction's; transfer it, and
			 * adjust the list position of the top transaction in the list by
			 * moving it to where the subtransaction is.
			 */
			txn->base_snapshot = subtxn->base_snapshot;
			txn->base_snapshot_lsn = subtxn->base_snapshot_lsn;
			dlist_insert_before(&subtxn->base_snapshot_node,
								&txn->base_snapshot_node);

			/*
			 * The subtransaction doesn't have a snapshot anymore (so it
			 * mustn't be in the list.)
			 */
			subtxn->base_snapshot = NULL;
			subtxn->base_snapshot_lsn = InvalidXLogRecPtr;
			dlist_delete(&subtxn->base_snapshot_node);
		}
		else
		{
			/* Base snap of toplevel is fine, so subxact's is not needed */
			SnapBuildSnapDecRefcount(subtxn->base_snapshot);
			dlist_delete(&subtxn->base_snapshot_node);
			subtxn->base_snapshot = NULL;
			subtxn->base_snapshot_lsn = InvalidXLogRecPtr;
		}
	}
}

/*
 * Associate a subtransaction with its toplevel transaction at commit
 * time. There may be no further changes added after this.
 */
void
ReorderBufferCommitChild(ReorderBuffer *rb, TransactionId xid,
						 TransactionId subxid, XLogRecPtr commit_lsn,
						 XLogRecPtr end_lsn)
{
	ReorderBufferTXN *subtxn;

	subtxn = ReorderBufferTXNByXid(rb, subxid, false, NULL,
								   InvalidXLogRecPtr, false);

	/*
	 * No need to do anything if that subtxn didn't contain any changes
	 */
	if (!subtxn)
		return;

	subtxn->final_lsn = commit_lsn;
	subtxn->end_lsn = end_lsn;

	/*
	 * Assign this subxact as a child of the toplevel xact (no-op if already
	 * done.)
	 */
	ReorderBufferAssignChild(rb, xid, subxid, InvalidXLogRecPtr);
}


/*
 * Support for efficiently iterating over a transaction's and its
 * subtransactions' changes.
 *
 * We do by doing a k-way merge between transactions/subtransactions. For that
 * we model the current heads of the different transactions as a binary heap
 * so we easily know which (sub-)transaction has the change with the smallest
 * lsn next.
 *
 * We assume the changes in individual transactions are already sorted by LSN.
 */

/*
 * Binary heap comparison function.
 */
static int
ReorderBufferIterCompare(Datum a, Datum b, void *arg)
{
	ReorderBufferIterTXNState *state = (ReorderBufferIterTXNState *) arg;
	XLogRecPtr	pos_a = state->entries[DatumGetInt32(a)].lsn;
	XLogRecPtr	pos_b = state->entries[DatumGetInt32(b)].lsn;

	if (pos_a < pos_b)
		return 1;
	else if (pos_a == pos_b)
		return 0;
	return -1;
}

/*
 * Allocate & initialize an iterator which iterates in lsn order over a
 * transaction and all its subtransactions.
 *
 * Note: The iterator state is returned through iter_state parameter rather
 * than the function's return value.  This is because the state gets cleaned up
 * in a PG_CATCH block in the caller, so we want to make sure the caller gets
 * back the state even if this function throws an exception.
 */
static void
ReorderBufferIterTXNInit(ReorderBuffer *rb, ReorderBufferTXN *txn,
						 ReorderBufferIterTXNState *volatile *iter_state)
{
	Size		nr_txns = 0;
	ReorderBufferIterTXNState *state;
	dlist_iter	cur_txn_i;
	int32		off;

	*iter_state = NULL;

	/*
	 * Calculate the size of our heap: one element for every transaction that
	 * contains changes.  (Besides the transactions already in the reorder
	 * buffer, we count the one we were directly passed.)
	 */
	if (txn->nentries > 0)
		nr_txns++;

	dlist_foreach(cur_txn_i, &txn->subtxns)
	{
		ReorderBufferTXN *cur_txn;

		cur_txn = dlist_container(ReorderBufferTXN, node, cur_txn_i.cur);

		if (cur_txn->nentries > 0)
			nr_txns++;
	}

	/*
	 * TODO: Consider adding fastpath for the rather common nr_txns=1 case, no
	 * need to allocate/build a heap then.
	 */

	/* allocate iteration state */
	state = (ReorderBufferIterTXNState *)
		MemoryContextAllocZero(rb->context,
							   sizeof(ReorderBufferIterTXNState) +
							   sizeof(ReorderBufferIterTXNEntry) * nr_txns);

	state->nr_txns = nr_txns;
	dlist_init(&state->old_change);

	for (off = 0; off < state->nr_txns; off++)
	{
		state->entries[off].file.vfd = -1;
		state->entries[off].segno = 0;
	}

	/* allocate heap */
	state->heap = binaryheap_allocate(state->nr_txns,
									  ReorderBufferIterCompare,
									  state);

	/* Now that the state fields are initialized, it is safe to return it. */
	*iter_state = state;

	/*
	 * Now insert items into the binary heap, in an unordered fashion.  (We
	 * will run a heap assembly step at the end; this is more efficient.)
	 */

	off = 0;

	/* add toplevel transaction if it contains changes */
	if (txn->nentries > 0)
	{
		ReorderBufferChange *cur_change;

		if (txn->serialized)
		{
			/* serialize remaining changes */
			ReorderBufferSerializeTXN(rb, txn);
			ReorderBufferRestoreChanges(rb, txn, &state->entries[off].file,
										&state->entries[off].segno);
		}

		cur_change = dlist_head_element(ReorderBufferChange, node,
										&txn->changes);

		state->entries[off].lsn = cur_change->lsn;
		state->entries[off].change = cur_change;
		state->entries[off].txn = txn;

		binaryheap_add_unordered(state->heap, Int32GetDatum(off++));
	}

	/* add subtransactions if they contain changes */
	dlist_foreach(cur_txn_i, &txn->subtxns)
	{
		ReorderBufferTXN *cur_txn;

		cur_txn = dlist_container(ReorderBufferTXN, node, cur_txn_i.cur);

		if (cur_txn->nentries > 0)
		{
			ReorderBufferChange *cur_change;

			if (cur_txn->serialized)
			{
				/* serialize remaining changes */
				ReorderBufferSerializeTXN(rb, cur_txn);
				ReorderBufferRestoreChanges(rb, cur_txn,
											&state->entries[off].file,
											&state->entries[off].segno);
			}
			cur_change = dlist_head_element(ReorderBufferChange, node,
											&cur_txn->changes);

			state->entries[off].lsn = cur_change->lsn;
			state->entries[off].change = cur_change;
			state->entries[off].txn = cur_txn;

			binaryheap_add_unordered(state->heap, Int32GetDatum(off++));
		}
	}

	/* assemble a valid binary heap */
	binaryheap_build(state->heap);
}

/*
 * Return the next change when iterating over a transaction and its
 * subtransactions.
 *
 * Returns NULL when no further changes exist.
 */
static ReorderBufferChange *
ReorderBufferIterTXNNext(ReorderBuffer *rb, ReorderBufferIterTXNState *state)
{
	ReorderBufferChange *change;
	ReorderBufferIterTXNEntry *entry;
	int32		off;

	/* nothing there anymore */
	if (state->heap->bh_size == 0)
		return NULL;

	off = DatumGetInt32(binaryheap_first(state->heap));
	entry = &state->entries[off];

	/* free memory we might have "leaked" in the previous *Next call */
	if (!dlist_is_empty(&state->old_change))
	{
		change = dlist_container(ReorderBufferChange, node,
								 dlist_pop_head_node(&state->old_change));
		ReorderBufferReturnChange(rb, change);
		Assert(dlist_is_empty(&state->old_change));
	}

	change = entry->change;

	/*
	 * update heap with information about which transaction has the next
	 * relevant change in LSN order
	 */

	/* there are in-memory changes */
	if (dlist_has_next(&entry->txn->changes, &entry->change->node))
	{
		dlist_node *next = dlist_next_node(&entry->txn->changes, &change->node);
		ReorderBufferChange *next_change =
		dlist_container(ReorderBufferChange, node, next);

		/* txn stays the same */
		state->entries[off].lsn = next_change->lsn;
		state->entries[off].change = next_change;

		binaryheap_replace_first(state->heap, Int32GetDatum(off));
		return change;
	}

	/* try to load changes from disk */
	if (entry->txn->nentries != entry->txn->nentries_mem)
	{
		/*
		 * Ugly: restoring changes will reuse *Change records, thus delete the
		 * current one from the per-tx list and only free in the next call.
		 */
		dlist_delete(&change->node);
		dlist_push_tail(&state->old_change, &change->node);

		if (ReorderBufferRestoreChanges(rb, entry->txn, &entry->file,
										&state->entries[off].segno))
		{
			/* successfully restored changes from disk */
			ReorderBufferChange *next_change =
			dlist_head_element(ReorderBufferChange, node,
							   &entry->txn->changes);

			elog(DEBUG2, "restored %u/%u changes from disk",
				 (uint32) entry->txn->nentries_mem,
				 (uint32) entry->txn->nentries);

			Assert(entry->txn->nentries_mem);
			/* txn stays the same */
			state->entries[off].lsn = next_change->lsn;
			state->entries[off].change = next_change;
			binaryheap_replace_first(state->heap, Int32GetDatum(off));

			return change;
		}
	}

	/* ok, no changes there anymore, remove */
	binaryheap_remove_first(state->heap);

	return change;
}

/*
 * Deallocate the iterator
 */
static void
ReorderBufferIterTXNFinish(ReorderBuffer *rb,
						   ReorderBufferIterTXNState *state)
{
	int32		off;

	for (off = 0; off < state->nr_txns; off++)
	{
		if (state->entries[off].file.vfd != -1)
			FileClose(state->entries[off].file.vfd);
	}

	/* free memory we might have "leaked" in the last *Next call */
	if (!dlist_is_empty(&state->old_change))
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node,
								 dlist_pop_head_node(&state->old_change));
		ReorderBufferReturnChange(rb, change);
		Assert(dlist_is_empty(&state->old_change));
	}

	binaryheap_free(state->heap);
	pfree(state);
}

/*
 * Cleanup the contents of a transaction, usually after the transaction
 * committed or aborted.
 */
static void
ReorderBufferCleanupTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	bool		found;
	dlist_mutable_iter iter;

	/* cleanup subtransactions & their changes */
	dlist_foreach_modify(iter, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, iter.cur);

		/*
		 * Subtransactions are always associated to the toplevel TXN, even if
		 * they originally were happening inside another subtxn, so we won't
		 * ever recurse more than one level deep here.
		 */
		Assert(subtxn->is_known_as_subxact);
		Assert(subtxn->nsubtxns == 0);

		ReorderBufferCleanupTXN(rb, subtxn);
	}

	/* cleanup changes in the toplevel txn */
	dlist_foreach_modify(iter, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		ReorderBufferReturnChange(rb, change);
	}

	/*
	 * Cleanup the tuplecids we stored for decoding catalog snapshot access.
	 * They are always stored in the toplevel transaction.
	 */
	dlist_foreach_modify(iter, &txn->tuplecids)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);
		Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);
		ReorderBufferReturnChange(rb, change);
	}

	/*
	 * Cleanup the base snapshot, if set.
	 */
	if (txn->base_snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(txn->base_snapshot);
		dlist_delete(&txn->base_snapshot_node);
	}

	/*
	 * Remove TXN from its containing list.
	 *
	 * Note: if txn->is_known_as_subxact, we are deleting the TXN from its
	 * parent's list of known subxacts; this leaves the parent's nsubxacts
	 * count too high, but we don't care.  Otherwise, we are deleting the TXN
	 * from the LSN-ordered list of toplevel TXNs.
	 */
	dlist_delete(&txn->node);

	/* now remove reference from buffer */
	hash_search(rb->by_txn,
				(void *) &txn->xid,
				HASH_REMOVE,
				&found);
	Assert(found);

	/* remove entries spilled to disk */
	if (txn->serialized)
		ReorderBufferRestoreCleanup(rb, txn);

	/* deallocate */
	ReorderBufferReturnTXN(rb, txn);
}

/*
 * Build a hash with a (relfilenode, ctid) -> (cmin, cmax) mapping for use by
 * HeapTupleSatisfiesHistoricMVCC.
 */
static void
ReorderBufferBuildTupleCidHash(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	dlist_iter	iter;
	HASHCTL		hash_ctl;

	if (!txn->has_catalog_changes || dlist_is_empty(&txn->tuplecids))
		return;

	memset(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = sizeof(ReorderBufferTupleCidKey);
	hash_ctl.entrysize = sizeof(ReorderBufferTupleCidEnt);
	hash_ctl.hcxt = rb->context;

	/*
	 * create the hash with the exact number of to-be-stored tuplecids from
	 * the start
	 */
	txn->tuplecid_hash =
		hash_create("ReorderBufferTupleCid", txn->ntuplecids, &hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	dlist_foreach(iter, &txn->tuplecids)
	{
		ReorderBufferTupleCidKey key;
		ReorderBufferTupleCidEnt *ent;
		bool		found;
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

		/* be careful about padding */
		memset(&key, 0, sizeof(ReorderBufferTupleCidKey));

		key.relnode = change->data.tuplecid.node;

		ItemPointerCopy(&change->data.tuplecid.tid,
						&key.tid);

		ent = (ReorderBufferTupleCidEnt *)
			hash_search(txn->tuplecid_hash,
						(void *) &key,
						HASH_ENTER,
						&found);
		if (!found)
		{
			ent->cmin = change->data.tuplecid.cmin;
			ent->cmax = change->data.tuplecid.cmax;
			ent->combocid = change->data.tuplecid.combocid;
		}
		else
		{
			/*
			 * Maybe we already saw this tuple before in this transaction, but
			 * if so it must have the same cmin.
			 */
			Assert(ent->cmin == change->data.tuplecid.cmin);

			/*
			 * cmax may be initially invalid, but once set it can only grow,
			 * and never become invalid again.
			 */
			Assert((ent->cmax == InvalidCommandId) ||
				   ((change->data.tuplecid.cmax != InvalidCommandId) &&
					(change->data.tuplecid.cmax > ent->cmax)));
			ent->cmax = change->data.tuplecid.cmax;
		}
	}
}

/*
 * Copy a provided snapshot so we can modify it privately. This is needed so
 * that catalog modifying transactions can look into intermediate catalog
 * states.
 */
static Snapshot
ReorderBufferCopySnap(ReorderBuffer *rb, Snapshot orig_snap,
					  ReorderBufferTXN *txn, CommandId cid)
{
	Snapshot	snap;
	dlist_iter	iter;
	int			i = 0;
	Size		size;

	size = sizeof(SnapshotData) +
		sizeof(TransactionId) * orig_snap->xcnt +
		sizeof(TransactionId) * (txn->nsubtxns + 1);

	snap = MemoryContextAllocZero(rb->context, size);
	memcpy(snap, orig_snap, sizeof(SnapshotData));

	snap->copied = true;
	snap->active_count = 1;		/* mark as active so nobody frees it */
	snap->regd_count = 0;
	snap->xip = (TransactionId *) (snap + 1);

	memcpy(snap->xip, orig_snap->xip, sizeof(TransactionId) * snap->xcnt);

	/*
	 * snap->subxip contains all txids that belong to our transaction which we
	 * need to check via cmin/cmax. That's why we store the toplevel
	 * transaction in there as well.
	 */
	snap->subxip = snap->xip + snap->xcnt;
	snap->subxip[i++] = txn->xid;

	/*
	 * nsubxcnt isn't decreased when subtransactions abort, so count manually.
	 * Since it's an upper boundary it is safe to use it for the allocation
	 * above.
	 */
	snap->subxcnt = 1;

	dlist_foreach(iter, &txn->subtxns)
	{
		ReorderBufferTXN *sub_txn;

		sub_txn = dlist_container(ReorderBufferTXN, node, iter.cur);
		snap->subxip[i++] = sub_txn->xid;
		snap->subxcnt++;
	}

	/* sort so we can bsearch() later */
	qsort(snap->subxip, snap->subxcnt, sizeof(TransactionId), xidComparator);

	/* store the specified current CommandId */
	snap->curcid = cid;

	return snap;
}

/*
 * Free a previously ReorderBufferCopySnap'ed snapshot
 */
static void
ReorderBufferFreeSnap(ReorderBuffer *rb, Snapshot snap)
{
	if (snap->copied)
		pfree(snap);
	else
		SnapBuildSnapDecRefcount(snap);
}

/*
 * Perform the replay of a transaction and its non-aborted subtransactions.
 *
 * Subtransactions previously have to be processed by
 * ReorderBufferCommitChild(), even if previously assigned to the toplevel
 * transaction with ReorderBufferAssignChild.
 *
 * We currently can only decode a transaction's contents when its commit
 * record is read because that's the only place where we know about cache
 * invalidations. Thus, once a toplevel commit is read, we iterate over the top
 * and subtransactions (using a k-way merge) and replay the changes in lsn
 * order.
 */
void
ReorderBufferCommit(ReorderBuffer *rb, TransactionId xid,
					XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
					TimestampTz commit_time,
					RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	ReorderBufferTXN *txn;
	volatile Snapshot snapshot_now;
	volatile CommandId command_id = FirstCommandId;
	bool		using_subtxn;
	ReorderBufferIterTXNState *volatile iterstate = NULL;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown transaction, nothing to replay */
	if (txn == NULL)
		return;

	txn->final_lsn = commit_lsn;
	txn->end_lsn = end_lsn;
	txn->commit_time = commit_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	/*
	 * If this transaction has no snapshot, it didn't make any changes to the
	 * database, so there's nothing to decode.  Note that
	 * ReorderBufferCommitChild will have transferred any snapshots from
	 * subtransactions if there were any.
	 */
	if (txn->base_snapshot == NULL)
	{
		Assert(txn->ninvalidations == 0);
		ReorderBufferCleanupTXN(rb, txn);
		return;
	}

	snapshot_now = txn->base_snapshot;

	/* build data to be able to lookup the CommandIds of catalog tuples */
	ReorderBufferBuildTupleCidHash(rb, txn);

	/* setup the initial snapshot */
	SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);

	/*
	 * Decoding needs access to syscaches et al., which in turn use
	 * heavyweight locks and such. Thus we need to have enough state around to
	 * keep track of those.  The easiest way is to simply use a transaction
	 * internally.  That also allows us to easily enforce that nothing writes
	 * to the database by checking for xid assignments.
	 *
	 * When we're called via the SQL SRF there's already a transaction
	 * started, so start an explicit subtransaction there.
	 */
	using_subtxn = IsTransactionOrTransactionBlock();

	PG_TRY();
	{
		ReorderBufferChange *change;
		ReorderBufferChange *specinsert = NULL;

		if (using_subtxn)
			BeginInternalSubTransaction("replay");
		else
			StartTransactionCommand();

		rb->begin(rb, txn);

		ReorderBufferIterTXNInit(rb, txn, &iterstate);
		while ((change = ReorderBufferIterTXNNext(rb, iterstate)) != NULL)
		{
			Relation	relation = NULL;
			Oid			reloid;

			CHECK_FOR_INTERRUPTS();

			switch (change->action)
			{
				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:

					/*
					 * Confirmation for speculative insertion arrived. Simply
					 * use as a normal record. It'll be cleaned up at the end
					 * of INSERT processing.
					 */
					if (specinsert == NULL)
						elog(ERROR, "invalid ordering of speculative insertion changes");
					Assert(specinsert->data.tp.oldtuple == NULL);
					change = specinsert;
					change->action = REORDER_BUFFER_CHANGE_INSERT;

					/* intentionally fall through */
				case REORDER_BUFFER_CHANGE_INSERT:
				case REORDER_BUFFER_CHANGE_UPDATE:
				case REORDER_BUFFER_CHANGE_DELETE:
					Assert(snapshot_now);

					reloid = RelidByRelfilenode(change->data.tp.relnode.spcNode,
												change->data.tp.relnode.relNode);

					/*
					 * Mapped catalog tuple without data, emitted while
					 * catalog table was in the process of being rewritten. We
					 * can fail to look up the relfilenode, because the
					 * relmapper has no "historic" view, in contrast to normal
					 * the normal catalog during decoding. Thus repeated
					 * rewrites can cause a lookup failure. That's OK because
					 * we do not decode catalog changes anyway. Normally such
					 * tuples would be skipped over below, but we can't
					 * identify whether the table should be logically logged
					 * without mapping the relfilenode to the oid.
					 */
					if (reloid == InvalidOid &&
						change->data.tp.newtuple == NULL &&
						change->data.tp.oldtuple == NULL)
						goto change_done;
					else if (reloid == InvalidOid)
						elog(ERROR, "could not map filenode \"%s\" to relation OID",
							 relpathperm(change->data.tp.relnode,
										 MAIN_FORKNUM));

					relation = RelationIdGetRelation(reloid);

					if (!RelationIsValid(relation))
						elog(ERROR, "could not open relation with OID %u (for filenode \"%s\")",
							 reloid,
							 relpathperm(change->data.tp.relnode,
										 MAIN_FORKNUM));

					if (!RelationIsLogicallyLogged(relation))
						goto change_done;

					/*
					 * Ignore temporary heaps created during DDL unless the
					 * plugin has asked for them.
					 */
					if (relation->rd_rel->relrewrite && !rb->output_rewrites)
						goto change_done;

					/*
					 * For now ignore sequence changes entirely. Most of the
					 * time they don't log changes using records we
					 * understand, so it doesn't make sense to handle the few
					 * cases we do.
					 */
					if (relation->rd_rel->relkind == RELKIND_SEQUENCE)
						goto change_done;

					/* user-triggered change */
					if (!IsToastRelation(relation))
					{
						ReorderBufferToastReplace(rb, txn, relation, change);
						rb->apply_change(rb, txn, relation, change);

						/*
						 * Only clear reassembled toast chunks if we're sure
						 * they're not required anymore. The creator of the
						 * tuple tells us.
						 */
						if (change->data.tp.clear_toast_afterwards)
							ReorderBufferToastReset(rb, txn);
					}
					/* we're not interested in toast deletions */
					else if (change->action == REORDER_BUFFER_CHANGE_INSERT)
					{
						/*
						 * Need to reassemble the full toasted Datum in
						 * memory, to ensure the chunks don't get reused till
						 * we're done remove it from the list of this
						 * transaction's changes. Otherwise it will get
						 * freed/reused while restoring spooled data from
						 * disk.
						 */
						Assert(change->data.tp.newtuple != NULL);

						dlist_delete(&change->node);
						ReorderBufferToastAppendChunk(rb, txn, relation,
													  change);
					}

			change_done:

					/*
					 * If speculative insertion was confirmed, the record isn't
					 * needed anymore.
					 */
					if (specinsert != NULL)
					{
						ReorderBufferReturnChange(rb, specinsert);
						specinsert = NULL;
					}

					if (relation != NULL)
					{
						RelationClose(relation);
						relation = NULL;
					}
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:

					/*
					 * Speculative insertions are dealt with by delaying the
					 * processing of the insert until the confirmation record
					 * arrives. For that we simply unlink the record from the
					 * chain, so it does not get freed/reused while restoring
					 * spooled data from disk.
					 *
					 * This is safe in the face of concurrent catalog changes
					 * because the relevant relation can't be changed between
					 * speculative insertion and confirmation due to
					 * CheckTableNotInUse() and locking.
					 */

					/* clear out a pending (and thus failed) speculation */
					if (specinsert != NULL)
					{
						ReorderBufferReturnChange(rb, specinsert);
						specinsert = NULL;
					}

					/* and memorize the pending insertion */
					dlist_delete(&change->node);
					specinsert = change;
					break;

				case REORDER_BUFFER_CHANGE_TRUNCATE:
					{
						int			i;
						int			nrelids = change->data.truncate.nrelids;
						int			nrelations = 0;
						Relation   *relations;

						relations = palloc0(nrelids * sizeof(Relation));
						for (i = 0; i < nrelids; i++)
						{
							Oid			relid = change->data.truncate.relids[i];
							Relation	relation;

							relation = RelationIdGetRelation(relid);

							if (!RelationIsValid(relation))
								elog(ERROR, "could not open relation with OID %u", relid);

							if (!RelationIsLogicallyLogged(relation))
								continue;

							relations[nrelations++] = relation;
						}

						rb->apply_truncate(rb, txn, nrelations, relations, change);

						for (i = 0; i < nrelations; i++)
							RelationClose(relations[i]);

						break;
					}

				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:

					/*
					 * Abort for speculative insertion arrived. So cleanup the
					 * specinsert tuple and toast hash.
					 *
					 * Note that we get the spec abort change for each toast
					 * entry but we need to perform the cleanup only the first
					 * time we get it for the main table.
					 */
					if (specinsert != NULL)
					{
						/*
						 * We must clean the toast hash before processing a
						 * completely new tuple to avoid confusion about the
						 * previous tuple's toast chunks.
						 */
						Assert(change->data.tp.clear_toast_afterwards);
						ReorderBufferToastReset(rb, txn);

						/* We don't need this record anymore. */
						ReorderBufferReturnChange(rb, specinsert);
						specinsert = NULL;
					}
					break;

				case REORDER_BUFFER_CHANGE_MESSAGE:
					rb->message(rb, txn, change->lsn, true,
								change->data.msg.prefix,
								change->data.msg.message_size,
								change->data.msg.message);
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
					/* get rid of the old */
					TeardownHistoricSnapshot(false);

					if (snapshot_now->copied)
					{
						ReorderBufferFreeSnap(rb, snapshot_now);
						snapshot_now =
							ReorderBufferCopySnap(rb, change->data.snapshot,
												  txn, command_id);
					}

					/*
					 * Restored from disk, need to be careful not to double
					 * free. We could introduce refcounting for that, but for
					 * now this seems infrequent enough not to care.
					 */
					else if (change->data.snapshot->copied)
					{
						snapshot_now =
							ReorderBufferCopySnap(rb, change->data.snapshot,
												  txn, command_id);
					}
					else
					{
						snapshot_now = change->data.snapshot;
					}


					/* and continue with the new one */
					SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
					Assert(change->data.command_id != InvalidCommandId);

					if (command_id < change->data.command_id)
					{
						command_id = change->data.command_id;

						if (!snapshot_now->copied)
						{
							/* we don't use the global one anymore */
							snapshot_now = ReorderBufferCopySnap(rb, snapshot_now,
																 txn, command_id);
						}

						snapshot_now->curcid = command_id;

						TeardownHistoricSnapshot(false);
						SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);

						/*
						 * Every time the CommandId is incremented, we could
						 * see new catalog contents, so execute all
						 * invalidations.
						 */
						ReorderBufferExecuteInvalidations(rb, txn);
					}

					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
					elog(ERROR, "tuplecid value in changequeue");
					break;
			}
		}

		/* speculative insertion record must be freed by now */
		Assert(!specinsert);

		/* clean up the iterator */
		ReorderBufferIterTXNFinish(rb, iterstate);
		iterstate = NULL;

		/* call commit callback */
		rb->commit(rb, txn, commit_lsn);

		/* this is just a sanity check against bad output plugin behaviour */
		if (GetCurrentTransactionIdIfAny() != InvalidTransactionId)
			elog(ERROR, "output plugin used XID %u",
				 GetCurrentTransactionId());

		/* cleanup */
		TeardownHistoricSnapshot(false);

		/*
		 * Aborting the current (sub-)transaction as a whole has the right
		 * semantics. We want all locks acquired in here to be released, not
		 * reassigned to the parent and we do not want any database access
		 * have persistent effects.
		 */
		AbortCurrentTransaction();

		/* make sure there's no cache pollution */
		ReorderBufferExecuteInvalidations(rb, txn);

		if (using_subtxn)
			RollbackAndReleaseCurrentSubTransaction();

		if (snapshot_now->copied)
			ReorderBufferFreeSnap(rb, snapshot_now);

		/* remove potential on-disk data, and deallocate */
		ReorderBufferCleanupTXN(rb, txn);
	}
	PG_CATCH();
	{
		/* TODO: Encapsulate cleanup from the PG_TRY and PG_CATCH blocks */
		if (iterstate)
			ReorderBufferIterTXNFinish(rb, iterstate);

		TeardownHistoricSnapshot(true);

		/*
		 * Force cache invalidation to happen outside of a valid transaction
		 * to prevent catalog access as we just caught an error.
		 */
		AbortCurrentTransaction();

		/* make sure there's no cache pollution */
		ReorderBufferExecuteInvalidations(rb, txn);

		if (using_subtxn)
			RollbackAndReleaseCurrentSubTransaction();

		if (snapshot_now->copied)
			ReorderBufferFreeSnap(rb, snapshot_now);

		/* remove potential on-disk data, and deallocate */
		ReorderBufferCleanupTXN(rb, txn);

		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Abort a transaction that possibly has previous changes. Needs to be first
 * called for subtransactions and then for the toplevel xid.
 *
 * NB: Transactions handled here have to have actively aborted (i.e. have
 * produced an abort record). Implicitly aborted transactions are handled via
 * ReorderBufferAbortOld(); transactions we're just not interested in, but
 * which have committed are handled in ReorderBufferForget().
 *
 * This function purges this transaction and its contents from memory and
 * disk.
 */
void
ReorderBufferAbort(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown, nothing to remove */
	if (txn == NULL)
		return;

	/* cosmetic... */
	txn->final_lsn = lsn;

	/* remove potential on-disk data, and deallocate */
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * Abort all transactions that aren't actually running anymore because the
 * server restarted.
 *
 * NB: These really have to be transactions that have aborted due to a server
 * crash/immediate restart, as we don't deal with invalidations here.
 */
void
ReorderBufferAbortOld(ReorderBuffer *rb, TransactionId oldestRunningXid)
{
	dlist_mutable_iter it;

	/*
	 * Iterate through all (potential) toplevel TXNs and abort all that are
	 * older than what possibly can be running. Once we've found the first
	 * that is alive we stop, there might be some that acquired an xid earlier
	 * but started writing later, but it's unlikely and they will be cleaned
	 * up in a later call to this function.
	 */
	dlist_foreach_modify(it, &rb->toplevel_by_lsn)
	{
		ReorderBufferTXN *txn;

		txn = dlist_container(ReorderBufferTXN, node, it.cur);

		if (TransactionIdPrecedes(txn->xid, oldestRunningXid))
		{
			elog(DEBUG2, "aborting old transaction %u", txn->xid);

			/* remove potential on-disk data, and deallocate this tx */
			ReorderBufferCleanupTXN(rb, txn);
		}
		else
			return;
	}
}

/*
 * Forget the contents of a transaction if we aren't interested in its
 * contents. Needs to be first called for subtransactions and then for the
 * toplevel xid.
 *
 * This is significantly different to ReorderBufferAbort() because
 * transactions that have committed need to be treated differently from aborted
 * ones since they may have modified the catalog.
 *
 * Note that this is only allowed to be called in the moment a transaction
 * commit has just been read, not earlier; otherwise later records referring
 * to this xid might re-create the transaction incompletely.
 */
void
ReorderBufferForget(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown, nothing to forget */
	if (txn == NULL)
		return;

	/* cosmetic... */
	txn->final_lsn = lsn;

	/*
	 * Process cache invalidation messages if there are any. Even if we're not
	 * interested in the transaction's contents, it could have manipulated the
	 * catalog and we need to update the caches according to that.
	 */
	if (txn->base_snapshot != NULL && txn->ninvalidations > 0)
		ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
										   txn->invalidations);
	else
		Assert(txn->ninvalidations == 0);

	/* remove potential on-disk data, and deallocate */
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * Execute invalidations happening outside the context of a decoded
 * transaction. That currently happens either for xid-less commits
 * (cf. RecordTransactionCommit()) or for invalidations in uninteresting
 * transactions (via ReorderBufferForget()).
 */
void
ReorderBufferImmediateInvalidation(ReorderBuffer *rb, uint32 ninvalidations,
								   SharedInvalidationMessage *invalidations)
{
	bool		use_subtxn = IsTransactionOrTransactionBlock();
	int			i;

	if (use_subtxn)
		BeginInternalSubTransaction("replay");

	/*
	 * Force invalidations to happen outside of a valid transaction - that way
	 * entries will just be marked as invalid without accessing the catalog.
	 * That's advantageous because we don't need to setup the full state
	 * necessary for catalog access.
	 */
	if (use_subtxn)
		AbortCurrentTransaction();

	for (i = 0; i < ninvalidations; i++)
		LocalExecuteInvalidationMessage(&invalidations[i]);

	if (use_subtxn)
		RollbackAndReleaseCurrentSubTransaction();
}

/*
 * Tell reorderbuffer about an xid seen in the WAL stream. Has to be called at
 * least once for every xid in XLogRecord->xl_xid (other places in records
 * may, but do not have to be passed through here).
 *
 * Reorderbuffer keeps some datastructures about transactions in LSN order,
 * for efficiency. To do that it has to know about when transactions are seen
 * first in the WAL. As many types of records are not actually interesting for
 * logical decoding, they do not necessarily pass though here.
 */
void
ReorderBufferProcessXid(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	/* many records won't have an xid assigned, centralize check here */
	if (xid != InvalidTransactionId)
		ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);
}

/*
 * Add a new snapshot to this transaction that may only used after lsn 'lsn'
 * because the previous snapshot doesn't describe the catalog correctly for
 * following rows.
 */
void
ReorderBufferAddSnapshot(ReorderBuffer *rb, TransactionId xid,
						 XLogRecPtr lsn, Snapshot snap)
{
	ReorderBufferChange *change = ReorderBufferGetChange(rb);

	change->data.snapshot = snap;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT;

	ReorderBufferQueueChange(rb, xid, lsn, change);
}

/*
 * Set up the transaction's base snapshot.
 *
 * If we know that xid is a subtransaction, set the base snapshot on the
 * top-level transaction instead.
 */
void
ReorderBufferSetBaseSnapshot(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, Snapshot snap)
{
	ReorderBufferTXN *txn;
	bool		is_new;

	AssertArg(snap != NULL);

	/*
	 * Fetch the transaction to operate on.  If we know it's a subtransaction,
	 * operate on its top-level transaction instead.
	 */
	txn = ReorderBufferTXNByXid(rb, xid, true, &is_new, lsn, true);
	if (txn->is_known_as_subxact)
		txn = ReorderBufferTXNByXid(rb, txn->toplevel_xid, false,
									NULL, InvalidXLogRecPtr, false);
	Assert(txn->base_snapshot == NULL);

	txn->base_snapshot = snap;
	txn->base_snapshot_lsn = lsn;
	dlist_push_tail(&rb->txns_by_base_snapshot_lsn, &txn->base_snapshot_node);

	AssertTXNLsnOrder(rb);
}

/*
 * Access the catalog with this CommandId at this point in the changestream.
 *
 * May only be called for command ids > 1
 */
void
ReorderBufferAddNewCommandId(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, CommandId cid)
{
	ReorderBufferChange *change = ReorderBufferGetChange(rb);

	change->data.command_id = cid;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID;

	ReorderBufferQueueChange(rb, xid, lsn, change);
}


/*
 * Add new (relfilenode, tid) -> (cmin, cmax) mappings.
 */
void
ReorderBufferAddNewTupleCids(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, RelFileNode node,
							 ItemPointerData tid, CommandId cmin,
							 CommandId cmax, CommandId combocid)
{
	ReorderBufferChange *change = ReorderBufferGetChange(rb);
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	change->data.tuplecid.node = node;
	change->data.tuplecid.tid = tid;
	change->data.tuplecid.cmin = cmin;
	change->data.tuplecid.cmax = cmax;
	change->data.tuplecid.combocid = combocid;
	change->lsn = lsn;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID;

	dlist_push_tail(&txn->tuplecids, &change->node);
	txn->ntuplecids++;
}

/*
 * Setup the invalidation of the toplevel transaction.
 *
 * This needs to be done before ReorderBufferCommit is called!
 */
void
ReorderBufferAddInvalidations(ReorderBuffer *rb, TransactionId xid,
							  XLogRecPtr lsn, Size nmsgs,
							  SharedInvalidationMessage *msgs)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	if (txn->ninvalidations != 0)
		elog(ERROR, "only ever add one set of invalidations");

	Assert(nmsgs > 0);

	txn->ninvalidations = nmsgs;
	txn->invalidations = (SharedInvalidationMessage *)
		MemoryContextAlloc(rb->context,
						   sizeof(SharedInvalidationMessage) * nmsgs);
	memcpy(txn->invalidations, msgs,
		   sizeof(SharedInvalidationMessage) * nmsgs);
}

/*
 * Apply all invalidations we know. Possibly we only need parts at this point
 * in the changestream but we don't know which those are.
 */
static void
ReorderBufferExecuteInvalidations(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	int			i;

	for (i = 0; i < txn->ninvalidations; i++)
		LocalExecuteInvalidationMessage(&txn->invalidations[i]);
}

/*
 * Mark a transaction as containing catalog changes
 */
void
ReorderBufferXidSetCatalogChanges(ReorderBuffer *rb, TransactionId xid,
								  XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	txn->has_catalog_changes = true;
}

/*
 * Query whether a transaction is already *known* to contain catalog
 * changes. This can be wrong until directly before the commit!
 */
bool
ReorderBufferXidHasCatalogChanges(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);
	if (txn == NULL)
		return false;

	return txn->has_catalog_changes;
}

/*
 * ReorderBufferXidHasBaseSnapshot
 *		Have we already set the base snapshot for the given txn/subtxn?
 */
bool
ReorderBufferXidHasBaseSnapshot(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false,
								NULL, InvalidXLogRecPtr, false);

	/* transaction isn't known yet, ergo no snapshot */
	if (txn == NULL)
		return false;

	/* a known subtxn? operate on top-level txn instead */
	if (txn->is_known_as_subxact)
		txn = ReorderBufferTXNByXid(rb, txn->toplevel_xid, false,
									NULL, InvalidXLogRecPtr, false);

	return txn->base_snapshot != NULL;
}


/*
 * ---------------------------------------
 * Disk serialization support
 * ---------------------------------------
 */

/*
 * Ensure the IO buffer is >= sz.
 */
static void
ReorderBufferSerializeReserve(ReorderBuffer *rb, Size sz)
{
	if (!rb->outbufsize)
	{
		rb->outbuf = MemoryContextAlloc(rb->context, sz);
		rb->outbufsize = sz;
	}
	else if (rb->outbufsize < sz)
	{
		rb->outbuf = repalloc(rb->outbuf, sz);
		rb->outbufsize = sz;
	}
}

/*
 * Check whether the transaction tx should spill its data to disk.
 */
static void
ReorderBufferCheckSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/*
	 * TODO: improve accounting so we cheaply can take subtransactions into
	 * account here.
	 */
	if (txn->nentries_mem >= max_changes_in_memory)
	{
		ReorderBufferSerializeTXN(rb, txn);
		Assert(txn->nentries_mem == 0);
	}
}

/*
 * Spill data of a large transaction (and its subtransactions) to disk.
 */
static void
ReorderBufferSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	dlist_iter	subtxn_i;
	dlist_mutable_iter change_i;
	int			fd = -1;
	XLogSegNo	curOpenSegNo = 0;
	Size		spilled = 0;

	elog(DEBUG2, "spill %u changes in XID %u to disk",
		 (uint32) txn->nentries_mem, txn->xid);

	/* do the same to all child TXs */
	dlist_foreach(subtxn_i, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, subtxn_i.cur);
		ReorderBufferSerializeTXN(rb, subtxn);
	}

	/* serialize changestream */
	dlist_foreach_modify(change_i, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, change_i.cur);

		/*
		 * store in segment in which it belongs by start lsn, don't split over
		 * multiple segments tho
		 */
		if (fd == -1 ||
			!XLByteInSeg(change->lsn, curOpenSegNo, wal_segment_size))
		{
			char		path[MAXPGPATH];

			if (fd != -1)
				CloseTransientFile(fd);

			XLByteToSeg(change->lsn, curOpenSegNo, wal_segment_size);

			/*
			 * No need to care about TLIs here, only used during a single run,
			 * so each LSN only maps to a specific WAL record.
			 */
			ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid,
										curOpenSegNo);

			/* open segment, create it if necessary */
			fd = OpenTransientFile(path,
								   O_CREAT | O_WRONLY | O_APPEND | PG_BINARY);

			if (fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));
		}

		ReorderBufferSerializeChange(rb, txn, fd, change);
		dlist_delete(&change->node);
		ReorderBufferReturnChange(rb, change);

		spilled++;
	}

	Assert(spilled == txn->nentries_mem);
	Assert(dlist_is_empty(&txn->changes));
	txn->nentries_mem = 0;
	txn->serialized = true;

	if (fd != -1)
		CloseTransientFile(fd);
}

/*
 * Serialize individual change to disk.
 */
static void
ReorderBufferSerializeChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
							 int fd, ReorderBufferChange *change)
{
	ReorderBufferDiskChange *ondisk;
	Size		sz = sizeof(ReorderBufferDiskChange);

	ReorderBufferSerializeReserve(rb, sz);

	ondisk = (ReorderBufferDiskChange *) rb->outbuf;
	memcpy(&ondisk->change, change, sizeof(ReorderBufferChange));

	switch (change->action)
	{
			/* fall through these, they're all similar enough */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			{
				char	   *data;
				ReorderBufferTupleBuf *oldtup,
						   *newtup;
				Size		oldlen = 0;
				Size		newlen = 0;

				oldtup = change->data.tp.oldtuple;
				newtup = change->data.tp.newtuple;

				if (oldtup)
				{
					sz += sizeof(HeapTupleData);
					oldlen = oldtup->tuple.t_len;
					sz += oldlen;
				}

				if (newtup)
				{
					sz += sizeof(HeapTupleData);
					newlen = newtup->tuple.t_len;
					sz += newlen;
				}

				/* make sure we have enough space */
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* might have been reallocated above */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				if (oldlen)
				{
					memcpy(data, &oldtup->tuple, sizeof(HeapTupleData));
					data += sizeof(HeapTupleData);

					memcpy(data, oldtup->tuple.t_data, oldlen);
					data += oldlen;
				}

				if (newlen)
				{
					memcpy(data, &newtup->tuple, sizeof(HeapTupleData));
					data += sizeof(HeapTupleData);

					memcpy(data, newtup->tuple.t_data, newlen);
					data += newlen;
				}
				break;
			}
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				char	   *data;
				Size		prefix_size = strlen(change->data.msg.prefix) + 1;

				sz += prefix_size + change->data.msg.message_size +
					sizeof(Size) + sizeof(Size);
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);

				/* might have been reallocated above */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				/* write the prefix including the size */
				memcpy(data, &prefix_size, sizeof(Size));
				data += sizeof(Size);
				memcpy(data, change->data.msg.prefix,
					   prefix_size);
				data += prefix_size;

				/* write the message including the size */
				memcpy(data, &change->data.msg.message_size, sizeof(Size));
				data += sizeof(Size);
				memcpy(data, change->data.msg.message,
					   change->data.msg.message_size);
				data += change->data.msg.message_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	snap;
				char	   *data;

				snap = change->data.snapshot;

				sz += sizeof(SnapshotData) +
					sizeof(TransactionId) * snap->xcnt +
					sizeof(TransactionId) * snap->subxcnt;

				/* make sure we have enough space */
				ReorderBufferSerializeReserve(rb, sz);
				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* might have been reallocated above */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				memcpy(data, snap, sizeof(SnapshotData));
				data += sizeof(SnapshotData);

				if (snap->xcnt)
				{
					memcpy(data, snap->xip,
						   sizeof(TransactionId) * snap->xcnt);
					data += sizeof(TransactionId) * snap->xcnt;
				}

				if (snap->subxcnt)
				{
					memcpy(data, snap->subxip,
						   sizeof(TransactionId) * snap->subxcnt);
					data += sizeof(TransactionId) * snap->subxcnt;
				}
				break;
			}
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				Size		size;
				char	   *data;

				/* account for the OIDs of truncated relations */
				size = sizeof(Oid) * change->data.truncate.nrelids;
				sz += size;

				/* make sure we have enough space */
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* might have been reallocated above */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				memcpy(data, change->data.truncate.relids, size);
				data += size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			/* ReorderBufferChange contains everything important */
			break;
	}

	ondisk->size = sz;

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_REORDER_BUFFER_WRITE);
	if (write(fd, rb->outbuf, ondisk->size) != ondisk->size)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to data file for XID %u: %m",
						txn->xid)));
	}
	pgstat_report_wait_end();

	/*
	 * Keep the transaction's final_lsn up to date with each change we send to
	 * disk, so that ReorderBufferRestoreCleanup works correctly.  (We used to
	 * only do this on commit and abort records, but that doesn't work if a
	 * system crash leaves a transaction without its abort record).
	 *
	 * Make sure not to move it backwards.
	 */
	if (txn->final_lsn < change->lsn)
		txn->final_lsn = change->lsn;

	Assert(ondisk->change.action == change->action);
}

/*
 * Restore a number of changes spilled to disk back into memory.
 */
static Size
ReorderBufferRestoreChanges(ReorderBuffer *rb, ReorderBufferTXN *txn,
							TXNEntryFile *file, XLogSegNo *segno)
{
	Size		restored = 0;
	XLogSegNo	last_segno;
	dlist_mutable_iter cleanup_iter;
	File	   *fd = &file->vfd;

	Assert(txn->first_lsn != InvalidXLogRecPtr);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	/* free current entries, so we have memory for more */
	dlist_foreach_modify(cleanup_iter, &txn->changes)
	{
		ReorderBufferChange *cleanup =
		dlist_container(ReorderBufferChange, node, cleanup_iter.cur);

		dlist_delete(&cleanup->node);
		ReorderBufferReturnChange(rb, cleanup);
	}
	txn->nentries_mem = 0;
	Assert(dlist_is_empty(&txn->changes));

	XLByteToSeg(txn->final_lsn, last_segno, wal_segment_size);

	while (restored < max_changes_in_memory && *segno <= last_segno)
	{
		int			readBytes;
		ReorderBufferDiskChange *ondisk;

		CHECK_FOR_INTERRUPTS();

		if (*fd == -1)
		{
			char		path[MAXPGPATH];

			/* first time in */
			if (*segno == 0)
				XLByteToSeg(txn->first_lsn, *segno, wal_segment_size);

			Assert(*segno != 0 || dlist_is_empty(&txn->changes));

			/*
			 * No need to care about TLIs here, only used during a single run,
			 * so each LSN only maps to a specific WAL record.
			 */
			ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid,
										*segno);

			*fd = PathNameOpenFile(path, O_RDONLY | PG_BINARY);

			/* No harm in resetting the offset even in case of failure */
			file->curOffset = 0;

			if (*fd < 0 && errno == ENOENT)
			{
				*fd = -1;
				(*segno)++;
				continue;
			}
			else if (*fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								path)));
		}

		/*
		 * Read the statically sized part of a change which has information
		 * about the total size. If we couldn't read a record, we're at the
		 * end of this file.
		 */
		ReorderBufferSerializeReserve(rb, sizeof(ReorderBufferDiskChange));
		readBytes = FileRead(file->vfd, rb->outbuf,
							 sizeof(ReorderBufferDiskChange),
							 file->curOffset, WAIT_EVENT_REORDER_BUFFER_READ);

		/* eof */
		if (readBytes == 0)
		{
			FileClose(*fd);
			*fd = -1;
			(*segno)++;
			continue;
		}
		else if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: %m")));
		else if (readBytes != sizeof(ReorderBufferDiskChange))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: read %d instead of %u bytes",
							readBytes,
							(uint32) sizeof(ReorderBufferDiskChange))));

		file->curOffset += readBytes;

		ondisk = (ReorderBufferDiskChange *) rb->outbuf;

		ReorderBufferSerializeReserve(rb,
									  sizeof(ReorderBufferDiskChange) + ondisk->size);
		ondisk = (ReorderBufferDiskChange *) rb->outbuf;

		readBytes = FileRead(file->vfd,
							 rb->outbuf + sizeof(ReorderBufferDiskChange),
							 ondisk->size - sizeof(ReorderBufferDiskChange),
							 file->curOffset,
							 WAIT_EVENT_REORDER_BUFFER_READ);

		if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: %m")));
		else if (readBytes != ondisk->size - sizeof(ReorderBufferDiskChange))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: read %d instead of %u bytes",
							readBytes,
							(uint32) (ondisk->size - sizeof(ReorderBufferDiskChange)))));

		file->curOffset += readBytes;

		/*
		 * ok, read a full change from disk, now restore it into proper
		 * in-memory format
		 */
		ReorderBufferRestoreChange(rb, txn, rb->outbuf);
		restored++;
	}

	return restored;
}

/*
 * Convert change from its on-disk format to in-memory format and queue it onto
 * the TXN's ->changes list.
 *
 * Note: although "data" is declared char*, at entry it points to a
 * maxalign'd buffer, making it safe in most of this function to assume
 * that the pointed-to data is suitably aligned for direct access.
 */
static void
ReorderBufferRestoreChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
						   char *data)
{
	ReorderBufferDiskChange *ondisk;
	ReorderBufferChange *change;

	ondisk = (ReorderBufferDiskChange *) data;

	change = ReorderBufferGetChange(rb);

	/* copy static part */
	memcpy(change, &ondisk->change, sizeof(ReorderBufferChange));

	data += sizeof(ReorderBufferDiskChange);

	/* restore individual stuff */
	switch (change->action)
	{
			/* fall through these, they're all similar enough */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			if (change->data.tp.oldtuple)
			{
				uint32		tuplelen = ((HeapTuple) data)->t_len;

				change->data.tp.oldtuple =
					ReorderBufferGetTupleBuf(rb, tuplelen - SizeofHeapTupleHeader);

				/* restore ->tuple */
				memcpy(&change->data.tp.oldtuple->tuple, data,
					   sizeof(HeapTupleData));
				data += sizeof(HeapTupleData);

				/* reset t_data pointer into the new tuplebuf */
				change->data.tp.oldtuple->tuple.t_data =
					ReorderBufferTupleBufData(change->data.tp.oldtuple);

				/* restore tuple data itself */
				memcpy(change->data.tp.oldtuple->tuple.t_data, data, tuplelen);
				data += tuplelen;
			}

			if (change->data.tp.newtuple)
			{
				/* here, data might not be suitably aligned! */
				uint32		tuplelen;

				memcpy(&tuplelen, data + offsetof(HeapTupleData, t_len),
					   sizeof(uint32));

				change->data.tp.newtuple =
					ReorderBufferGetTupleBuf(rb, tuplelen - SizeofHeapTupleHeader);

				/* restore ->tuple */
				memcpy(&change->data.tp.newtuple->tuple, data,
					   sizeof(HeapTupleData));
				data += sizeof(HeapTupleData);

				/* reset t_data pointer into the new tuplebuf */
				change->data.tp.newtuple->tuple.t_data =
					ReorderBufferTupleBufData(change->data.tp.newtuple);

				/* restore tuple data itself */
				memcpy(change->data.tp.newtuple->tuple.t_data, data, tuplelen);
				data += tuplelen;
			}

			break;
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				Size		prefix_size;

				/* read prefix */
				memcpy(&prefix_size, data, sizeof(Size));
				data += sizeof(Size);
				change->data.msg.prefix = MemoryContextAlloc(rb->context,
															 prefix_size);
				memcpy(change->data.msg.prefix, data, prefix_size);
				Assert(change->data.msg.prefix[prefix_size - 1] == '\0');
				data += prefix_size;

				/* read the message */
				memcpy(&change->data.msg.message_size, data, sizeof(Size));
				data += sizeof(Size);
				change->data.msg.message = MemoryContextAlloc(rb->context,
															  change->data.msg.message_size);
				memcpy(change->data.msg.message, data,
					   change->data.msg.message_size);
				data += change->data.msg.message_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	oldsnap;
				Snapshot	newsnap;
				Size		size;

				oldsnap = (Snapshot) data;

				size = sizeof(SnapshotData) +
					sizeof(TransactionId) * oldsnap->xcnt +
					sizeof(TransactionId) * (oldsnap->subxcnt + 0);

				change->data.snapshot = MemoryContextAllocZero(rb->context, size);

				newsnap = change->data.snapshot;

				memcpy(newsnap, data, size);
				newsnap->xip = (TransactionId *)
					(((char *) newsnap) + sizeof(SnapshotData));
				newsnap->subxip = newsnap->xip + newsnap->xcnt;
				newsnap->copied = true;
				break;
			}
			/* the base struct contains all the data, easy peasy */
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				Oid		   *relids;

				relids = ReorderBufferGetRelids(rb,
												change->data.truncate.nrelids);
				memcpy(relids, data, change->data.truncate.nrelids * sizeof(Oid));
				change->data.truncate.relids = relids;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			break;
	}

	dlist_push_tail(&txn->changes, &change->node);
	txn->nentries_mem++;
}

/*
 * Remove all on-disk stored for the passed in transaction.
 */
static void
ReorderBufferRestoreCleanup(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	XLogSegNo	first;
	XLogSegNo	cur;
	XLogSegNo	last;

	Assert(txn->first_lsn != InvalidXLogRecPtr);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	XLByteToSeg(txn->first_lsn, first, wal_segment_size);
	XLByteToSeg(txn->final_lsn, last, wal_segment_size);

	/* iterate over all possible filenames, and delete them */
	for (cur = first; cur <= last; cur++)
	{
		char		path[MAXPGPATH];

		ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid, cur);
		if (unlink(path) != 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
	}
}

/*
 * Remove any leftover serialized reorder buffers from a slot directory after a
 * prior crash or decoding session exit.
 */
static void
ReorderBufferCleanupSerializedTXNs(const char *slotname)
{
	DIR		   *spill_dir;
	struct dirent *spill_de;
	struct stat statbuf;
	char		path[MAXPGPATH * 2 + 12];

	sprintf(path, "pg_replslot/%s", slotname);

	/* we're only handling directories here, skip if it's not ours */
	if (lstat(path, &statbuf) == 0 && !S_ISDIR(statbuf.st_mode))
		return;

	spill_dir = AllocateDir(path);
	while ((spill_de = ReadDirExtended(spill_dir, path, INFO)) != NULL)
	{
		/* only look at names that can be ours */
		if (strncmp(spill_de->d_name, "xid", 3) == 0)
		{
			snprintf(path, sizeof(path),
					 "pg_replslot/%s/%s", slotname,
					 spill_de->d_name);

			if (unlink(path) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\" during removal of pg_replslot/%s/xid*: %m",
								path, slotname)));
		}
	}
	FreeDir(spill_dir);
}

/*
 * Given a replication slot, transaction ID and segment number, fill in the
 * corresponding spill file into 'path', which is a caller-owned buffer of size
 * at least MAXPGPATH.
 */
static void
ReorderBufferSerializedPath(char *path, ReplicationSlot *slot, TransactionId xid,
							XLogSegNo segno)
{
	XLogRecPtr	recptr;

	XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, recptr);

	snprintf(path, MAXPGPATH, "pg_replslot/%s/xid-%u-lsn-%X-%X.spill",
			 NameStr(MyReplicationSlot->data.name),
			 xid,
			 (uint32) (recptr >> 32), (uint32) recptr);
}

/*
 * Delete all data spilled to disk after we've restarted/crashed. It will be
 * recreated when the respective slots are reused.
 */
void
StartupReorderBuffer(void)
{
	DIR		   *logical_dir;
	struct dirent *logical_de;

	logical_dir = AllocateDir("pg_replslot");
	while ((logical_de = ReadDir(logical_dir, "pg_replslot")) != NULL)
	{
		if (strcmp(logical_de->d_name, ".") == 0 ||
			strcmp(logical_de->d_name, "..") == 0)
			continue;

		/* if it cannot be a slot, skip the directory */
		if (!ReplicationSlotValidateName(logical_de->d_name, DEBUG2))
			continue;

		/*
		 * ok, has to be a surviving logical slot, iterate and delete
		 * everything starting with xid-*
		 */
		ReorderBufferCleanupSerializedTXNs(logical_de->d_name);
	}
	FreeDir(logical_dir);
}

/* ---------------------------------------
 * toast reassembly support
 * ---------------------------------------
 */

/*
 * Initialize per tuple toast reconstruction support.
 */
static void
ReorderBufferToastInitHash(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	HASHCTL		hash_ctl;

	Assert(txn->toast_hash == NULL);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(ReorderBufferToastEnt);
	hash_ctl.hcxt = rb->context;
	txn->toast_hash = hash_create("ReorderBufferToastHash", 5, &hash_ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Per toast-chunk handling for toast reconstruction
 *
 * Appends a toast chunk so we can reconstruct it when the tuple "owning" the
 * toasted Datum comes along.
 */
static void
ReorderBufferToastAppendChunk(ReorderBuffer *rb, ReorderBufferTXN *txn,
							  Relation relation, ReorderBufferChange *change)
{
	ReorderBufferToastEnt *ent;
	ReorderBufferTupleBuf *newtup;
	bool		found;
	int32		chunksize;
	bool		isnull;
	Pointer		chunk;
	TupleDesc	desc = RelationGetDescr(relation);
	Oid			chunk_id;
	int32		chunk_seq;

	if (txn->toast_hash == NULL)
		ReorderBufferToastInitHash(rb, txn);

	Assert(IsToastRelation(relation));

	newtup = change->data.tp.newtuple;
	chunk_id = DatumGetObjectId(fastgetattr(&newtup->tuple, 1, desc, &isnull));
	Assert(!isnull);
	chunk_seq = DatumGetInt32(fastgetattr(&newtup->tuple, 2, desc, &isnull));
	Assert(!isnull);

	ent = (ReorderBufferToastEnt *)
		hash_search(txn->toast_hash,
					(void *) &chunk_id,
					HASH_ENTER,
					&found);

	if (!found)
	{
		Assert(ent->chunk_id == chunk_id);
		ent->num_chunks = 0;
		ent->last_chunk_seq = 0;
		ent->size = 0;
		ent->reconstructed = NULL;
		dlist_init(&ent->chunks);

		if (chunk_seq != 0)
			elog(ERROR, "got sequence entry %d for toast chunk %u instead of seq 0",
				 chunk_seq, chunk_id);
	}
	else if (found && chunk_seq != ent->last_chunk_seq + 1)
		elog(ERROR, "got sequence entry %d for toast chunk %u instead of seq %d",
			 chunk_seq, chunk_id, ent->last_chunk_seq + 1);

	chunk = DatumGetPointer(fastgetattr(&newtup->tuple, 3, desc, &isnull));
	Assert(!isnull);

	/* calculate size so we can allocate the right size at once later */
	if (!VARATT_IS_EXTENDED(chunk))
		chunksize = VARSIZE(chunk) - VARHDRSZ;
	else if (VARATT_IS_SHORT(chunk))
		/* could happen due to heap_form_tuple doing its thing */
		chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
	else
		elog(ERROR, "unexpected type of toast chunk");

	ent->size += chunksize;
	ent->last_chunk_seq = chunk_seq;
	ent->num_chunks++;
	dlist_push_tail(&ent->chunks, &change->node);
}

/*
 * Rejigger change->newtuple to point to in-memory toast tuples instead to
 * on-disk toast tuples that may not longer exist (think DROP TABLE or VACUUM).
 *
 * We cannot replace unchanged toast tuples though, so those will still point
 * to on-disk toast data.
 */
static void
ReorderBufferToastReplace(ReorderBuffer *rb, ReorderBufferTXN *txn,
						  Relation relation, ReorderBufferChange *change)
{
	TupleDesc	desc;
	int			natt;
	Datum	   *attrs;
	bool	   *isnull;
	bool	   *free;
	HeapTuple	tmphtup;
	Relation	toast_rel;
	TupleDesc	toast_desc;
	MemoryContext oldcontext;
	ReorderBufferTupleBuf *newtup;

	/* no toast tuples changed */
	if (txn->toast_hash == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(rb->context);

	/* we should only have toast tuples in an INSERT or UPDATE */
	Assert(change->data.tp.newtuple);

	desc = RelationGetDescr(relation);

	toast_rel = RelationIdGetRelation(relation->rd_rel->reltoastrelid);
	if (!RelationIsValid(toast_rel))
		elog(ERROR, "could not open toast relation with OID %u (base relation \"%s\")",
			 relation->rd_rel->reltoastrelid, RelationGetRelationName(relation));

	toast_desc = RelationGetDescr(toast_rel);

	/* should we allocate from stack instead? */
	attrs = palloc0(sizeof(Datum) * desc->natts);
	isnull = palloc0(sizeof(bool) * desc->natts);
	free = palloc0(sizeof(bool) * desc->natts);

	newtup = change->data.tp.newtuple;

	heap_deform_tuple(&newtup->tuple, desc, attrs, isnull);

	for (natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, natt);
		ReorderBufferToastEnt *ent;
		struct varlena *varlena;

		/* va_rawsize is the size of the original datum -- including header */
		struct varatt_external toast_pointer;
		struct varatt_indirect redirect_pointer;
		struct varlena *new_datum = NULL;
		struct varlena *reconstructed;
		dlist_iter	it;
		Size		data_done = 0;

		/* system columns aren't toasted */
		if (attr->attnum < 0)
			continue;

		if (attr->attisdropped)
			continue;

		/* not a varlena datatype */
		if (attr->attlen != -1)
			continue;

		/* no data */
		if (isnull[natt])
			continue;

		/* ok, we know we have a toast datum */
		varlena = (struct varlena *) DatumGetPointer(attrs[natt]);

		/* no need to do anything if the tuple isn't external */
		if (!VARATT_IS_EXTERNAL(varlena))
			continue;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, varlena);

		/*
		 * Check whether the toast tuple changed, replace if so.
		 */
		ent = (ReorderBufferToastEnt *)
			hash_search(txn->toast_hash,
						(void *) &toast_pointer.va_valueid,
						HASH_FIND,
						NULL);
		if (ent == NULL)
			continue;

		new_datum =
			(struct varlena *) palloc0(INDIRECT_POINTER_SIZE);

		free[natt] = true;

		reconstructed = palloc0(toast_pointer.va_rawsize);

		ent->reconstructed = reconstructed;

		/* stitch toast tuple back together from its parts */
		dlist_foreach(it, &ent->chunks)
		{
			bool		isnull;
			ReorderBufferChange *cchange;
			ReorderBufferTupleBuf *ctup;
			Pointer		chunk;

			cchange = dlist_container(ReorderBufferChange, node, it.cur);
			ctup = cchange->data.tp.newtuple;
			chunk = DatumGetPointer(
									fastgetattr(&ctup->tuple, 3, toast_desc, &isnull));

			Assert(!isnull);
			Assert(!VARATT_IS_EXTERNAL(chunk));
			Assert(!VARATT_IS_SHORT(chunk));

			memcpy(VARDATA(reconstructed) + data_done,
				   VARDATA(chunk),
				   VARSIZE(chunk) - VARHDRSZ);
			data_done += VARSIZE(chunk) - VARHDRSZ;
		}
		Assert(data_done == toast_pointer.va_extsize);

		/* make sure its marked as compressed or not */
		if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			SET_VARSIZE_COMPRESSED(reconstructed, data_done + VARHDRSZ);
		else
			SET_VARSIZE(reconstructed, data_done + VARHDRSZ);

		memset(&redirect_pointer, 0, sizeof(redirect_pointer));
		redirect_pointer.pointer = reconstructed;

		SET_VARTAG_EXTERNAL(new_datum, VARTAG_INDIRECT);
		memcpy(VARDATA_EXTERNAL(new_datum), &redirect_pointer,
			   sizeof(redirect_pointer));

		attrs[natt] = PointerGetDatum(new_datum);
	}

	/*
	 * Build tuple in separate memory & copy tuple back into the tuplebuf
	 * passed to the output plugin. We can't directly heap_fill_tuple() into
	 * the tuplebuf because attrs[] will point back into the current content.
	 */
	tmphtup = heap_form_tuple(desc, attrs, isnull);
	Assert(newtup->tuple.t_len <= MaxHeapTupleSize);
	Assert(ReorderBufferTupleBufData(newtup) == newtup->tuple.t_data);

	memcpy(newtup->tuple.t_data, tmphtup->t_data, tmphtup->t_len);
	newtup->tuple.t_len = tmphtup->t_len;

	/*
	 * free resources we won't further need, more persistent stuff will be
	 * free'd in ReorderBufferToastReset().
	 */
	RelationClose(toast_rel);
	pfree(tmphtup);
	for (natt = 0; natt < desc->natts; natt++)
	{
		if (free[natt])
			pfree(DatumGetPointer(attrs[natt]));
	}
	pfree(attrs);
	pfree(free);
	pfree(isnull);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Free all resources allocated for toast reconstruction.
 */
static void
ReorderBufferToastReset(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	HASH_SEQ_STATUS hstat;
	ReorderBufferToastEnt *ent;

	if (txn->toast_hash == NULL)
		return;

	/* sequentially walk over the hash and free everything */
	hash_seq_init(&hstat, txn->toast_hash);
	while ((ent = (ReorderBufferToastEnt *) hash_seq_search(&hstat)) != NULL)
	{
		dlist_mutable_iter it;

		if (ent->reconstructed != NULL)
			pfree(ent->reconstructed);

		dlist_foreach_modify(it, &ent->chunks)
		{
			ReorderBufferChange *change =
			dlist_container(ReorderBufferChange, node, it.cur);

			dlist_delete(&change->node);
			ReorderBufferReturnChange(rb, change);
		}
	}

	hash_destroy(txn->toast_hash);
	txn->toast_hash = NULL;
}


/* ---------------------------------------
 * Visibility support for logical decoding
 *
 *
 * Lookup actual cmin/cmax values when using decoding snapshot. We can't
 * always rely on stored cmin/cmax values because of two scenarios:
 *
 * * A tuple got changed multiple times during a single transaction and thus
 *	 has got a combocid. Combocid's are only valid for the duration of a
 *	 single transaction.
 * * A tuple with a cmin but no cmax (and thus no combocid) got
 *	 deleted/updated in another transaction than the one which created it
 *	 which we are looking at right now. As only one of cmin, cmax or combocid
 *	 is actually stored in the heap we don't have access to the value we
 *	 need anymore.
 *
 * To resolve those problems we have a per-transaction hash of (cmin,
 * cmax) tuples keyed by (relfilenode, ctid) which contains the actual
 * (cmin, cmax) values. That also takes care of combocids by simply
 * not caring about them at all. As we have the real cmin/cmax values
 * combocids aren't interesting.
 *
 * As we only care about catalog tuples here the overhead of this
 * hashtable should be acceptable.
 *
 * Heap rewrites complicate this a bit, check rewriteheap.c for
 * details.
 * -------------------------------------------------------------------------
 */

/* struct for qsort()ing mapping files by lsn somewhat efficiently */
typedef struct RewriteMappingFile
{
	XLogRecPtr	lsn;
	char		fname[MAXPGPATH];
} RewriteMappingFile;

#if NOT_USED
static void
DisplayMapping(HTAB *tuplecid_data)
{
	HASH_SEQ_STATUS hstat;
	ReorderBufferTupleCidEnt *ent;

	hash_seq_init(&hstat, tuplecid_data);
	while ((ent = (ReorderBufferTupleCidEnt *) hash_seq_search(&hstat)) != NULL)
	{
		elog(DEBUG3, "mapping: node: %u/%u/%u tid: %u/%u cmin: %u, cmax: %u",
			 ent->key.relnode.dbNode,
			 ent->key.relnode.spcNode,
			 ent->key.relnode.relNode,
			 ItemPointerGetBlockNumber(&ent->key.tid),
			 ItemPointerGetOffsetNumber(&ent->key.tid),
			 ent->cmin,
			 ent->cmax
			);
	}
}
#endif

/*
 * Apply a single mapping file to tuplecid_data.
 *
 * The mapping file has to have been verified to be a) committed b) for our
 * transaction c) applied in LSN order.
 */
static void
ApplyLogicalMappingFile(HTAB *tuplecid_data, Oid relid, const char *fname)
{
	char		path[MAXPGPATH];
	int			fd;
	int			readBytes;
	LogicalRewriteMappingData map;

	sprintf(path, "pg_logical/mappings/%s", fname);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	while (true)
	{
		ReorderBufferTupleCidKey key;
		ReorderBufferTupleCidEnt *ent;
		ReorderBufferTupleCidEnt *new_ent;
		bool		found;

		/* be careful about padding */
		memset(&key, 0, sizeof(ReorderBufferTupleCidKey));

		/* read all mappings till the end of the file */
		pgstat_report_wait_start(WAIT_EVENT_REORDER_LOGICAL_MAPPING_READ);
		readBytes = read(fd, &map, sizeof(LogicalRewriteMappingData));
		pgstat_report_wait_end();

		if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else if (readBytes == 0)	/* EOF */
			break;
		else if (readBytes != sizeof(LogicalRewriteMappingData))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": read %d instead of %d bytes",
							path, readBytes,
							(int32) sizeof(LogicalRewriteMappingData))));

		key.relnode = map.old_node;
		ItemPointerCopy(&map.old_tid,
						&key.tid);


		ent = (ReorderBufferTupleCidEnt *)
			hash_search(tuplecid_data,
						(void *) &key,
						HASH_FIND,
						NULL);

		/* no existing mapping, no need to update */
		if (!ent)
			continue;

		key.relnode = map.new_node;
		ItemPointerCopy(&map.new_tid,
						&key.tid);

		new_ent = (ReorderBufferTupleCidEnt *)
			hash_search(tuplecid_data,
						(void *) &key,
						HASH_ENTER,
						&found);

		if (found)
		{
			/*
			 * Make sure the existing mapping makes sense. We sometime update
			 * old records that did not yet have a cmax (e.g. pg_class' own
			 * entry while rewriting it) during rewrites, so allow that.
			 */
			Assert(ent->cmin == InvalidCommandId || ent->cmin == new_ent->cmin);
			Assert(ent->cmax == InvalidCommandId || ent->cmax == new_ent->cmax);
		}
		else
		{
			/* update mapping */
			new_ent->cmin = ent->cmin;
			new_ent->cmax = ent->cmax;
			new_ent->combocid = ent->combocid;
		}
	}

	if (CloseTransientFile(fd))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}


/*
 * Check whether the TransactionOid 'xid' is in the pre-sorted array 'xip'.
 */
static bool
TransactionIdInArray(TransactionId xid, TransactionId *xip, Size num)
{
	return bsearch(&xid, xip, num,
				   sizeof(TransactionId), xidComparator) != NULL;
}

/*
 * qsort() comparator for sorting RewriteMappingFiles in LSN order.
 */
static int
file_sort_by_lsn(const void *a_p, const void *b_p)
{
	RewriteMappingFile *a = *(RewriteMappingFile **) a_p;
	RewriteMappingFile *b = *(RewriteMappingFile **) b_p;

	if (a->lsn < b->lsn)
		return -1;
	else if (a->lsn > b->lsn)
		return 1;
	return 0;
}

/*
 * Apply any existing logical remapping files if there are any targeted at our
 * transaction for relid.
 */
static void
UpdateLogicalMappings(HTAB *tuplecid_data, Oid relid, Snapshot snapshot)
{
	DIR		   *mapping_dir;
	struct dirent *mapping_de;
	List	   *files = NIL;
	ListCell   *file;
	RewriteMappingFile **files_a;
	size_t		off;
	Oid			dboid = IsSharedRelation(relid) ? InvalidOid : MyDatabaseId;

	mapping_dir = AllocateDir("pg_logical/mappings");
	while ((mapping_de = ReadDir(mapping_dir, "pg_logical/mappings")) != NULL)
	{
		Oid			f_dboid;
		Oid			f_relid;
		TransactionId f_mapped_xid;
		TransactionId f_create_xid;
		XLogRecPtr	f_lsn;
		uint32		f_hi,
					f_lo;
		RewriteMappingFile *f;

		if (strcmp(mapping_de->d_name, ".") == 0 ||
			strcmp(mapping_de->d_name, "..") == 0)
			continue;

		/* Ignore files that aren't ours */
		if (strncmp(mapping_de->d_name, "map-", 4) != 0)
			continue;

		if (sscanf(mapping_de->d_name, LOGICAL_REWRITE_FORMAT,
				   &f_dboid, &f_relid, &f_hi, &f_lo,
				   &f_mapped_xid, &f_create_xid) != 6)
			elog(ERROR, "could not parse filename \"%s\"", mapping_de->d_name);

		f_lsn = ((uint64) f_hi) << 32 | f_lo;

		/* mapping for another database */
		if (f_dboid != dboid)
			continue;

		/* mapping for another relation */
		if (f_relid != relid)
			continue;

		/* did the creating transaction abort? */
		if (!TransactionIdDidCommit(f_create_xid))
			continue;

		/* not for our transaction */
		if (!TransactionIdInArray(f_mapped_xid, snapshot->subxip, snapshot->subxcnt))
			continue;

		/* ok, relevant, queue for apply */
		f = palloc(sizeof(RewriteMappingFile));
		f->lsn = f_lsn;
		strcpy(f->fname, mapping_de->d_name);
		files = lappend(files, f);
	}
	FreeDir(mapping_dir);

	/* build array we can easily sort */
	files_a = palloc(list_length(files) * sizeof(RewriteMappingFile *));
	off = 0;
	foreach(file, files)
	{
		files_a[off++] = lfirst(file);
	}

	/* sort files so we apply them in LSN order */
	qsort(files_a, list_length(files), sizeof(RewriteMappingFile *),
		  file_sort_by_lsn);

	for (off = 0; off < list_length(files); off++)
	{
		RewriteMappingFile *f = files_a[off];

		elog(DEBUG1, "applying mapping: \"%s\" in %u", f->fname,
			 snapshot->subxip[0]);
		ApplyLogicalMappingFile(tuplecid_data, relid, f->fname);
		pfree(f);
	}
}

/*
 * Lookup cmin/cmax of a tuple, during logical decoding where we can't rely on
 * combocids.
 */
bool
ResolveCminCmaxDuringDecoding(HTAB *tuplecid_data,
							  Snapshot snapshot,
							  HeapTuple htup, Buffer buffer,
							  CommandId *cmin, CommandId *cmax)
{
	ReorderBufferTupleCidKey key;
	ReorderBufferTupleCidEnt *ent;
	ForkNumber	forkno;
	BlockNumber blockno;
	bool		updated_mapping = false;

	/* be careful about padding */
	memset(&key, 0, sizeof(key));

	Assert(!BufferIsLocal(buffer));

	/*
	 * get relfilenode from the buffer, no convenient way to access it other
	 * than that.
	 */
	BufferGetTag(buffer, &key.relnode, &forkno, &blockno);

	/* tuples can only be in the main fork */
	Assert(forkno == MAIN_FORKNUM);
	Assert(blockno == ItemPointerGetBlockNumber(&htup->t_self));

	ItemPointerCopy(&htup->t_self,
					&key.tid);

restart:
	ent = (ReorderBufferTupleCidEnt *)
		hash_search(tuplecid_data,
					(void *) &key,
					HASH_FIND,
					NULL);

	/*
	 * failed to find a mapping, check whether the table was rewritten and
	 * apply mapping if so, but only do that once - there can be no new
	 * mappings while we are in here since we have to hold a lock on the
	 * relation.
	 */
	if (ent == NULL && !updated_mapping)
	{
		UpdateLogicalMappings(tuplecid_data, htup->t_tableOid, snapshot);
		/* now check but don't update for a mapping again */
		updated_mapping = true;
		goto restart;
	}
	else if (ent == NULL)
		return false;

	if (cmin)
		*cmin = ent->cmin;
	if (cmax)
		*cmax = ent->cmax;
	return true;
}
