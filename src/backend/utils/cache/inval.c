/*-------------------------------------------------------------------------
 *
 * inval.c
 *	  POSTGRES cache invalidation dispatcher code.
 *
 *	This is subtle stuff, so pay attention:
 *
 *	When a tuple is updated or deleted, our time qualification rules consider
 *	that it is *still valid* so long as we are in the same command, ie,
 *	until the next CommandCounterIncrement() or transaction commit.
 *	(See utils/time/tqual.c.)  At the command boundary, the old tuple stops
 *	being valid and the new version, if any, becomes valid.  Therefore,
 *	we cannot simply flush a tuple from the system caches during heap_update()
 *	or heap_delete().  The tuple is still good at that point; what's more,
 *	even if we did flush it, it might be reloaded into the caches by a later
 *	request in the same command.  So the correct behavior is to keep a list
 *	of outdated (updated/deleted) tuples and then do the required cache
 *	flushes at the next command boundary.  Similarly, we need a list of
 *	inserted tuples (including new versions of updated tuples), which we will
 *	use to flush those tuples out of the caches if we abort the transaction.
 *	Notice that the first list lives only till command boundary, whereas the
 *	second lives till end of transaction.  Finally, we need a third list of
 *	all tuples outdated in the current transaction; if we commit, we send
 *	those invalidation events to all other backends (via the SI message queue)
 *	so that they can flush obsolete entries from their caches.  This list
 *	definitely can't be processed until after we commit, otherwise the other
 *	backends won't see our updated tuples as good.
 *
 *	We do not need to register EVERY tuple operation in this way, just those
 *	on tuples in relations that have associated catcaches.  We do, however,
 *	have to register every operation on every tuple that *could* be in a
 *	catcache, whether or not it currently is in our cache.  Also, if the
 *	tuple is in a relation that has multiple catcaches, we need to register
 *	an invalidation message for each such catcache.  catcache.c's
 *	PrepareToInvalidateCacheTuple() routine provides the knowledge of which
 *	catcaches may need invalidation for a given tuple.
 *
 *	Also, whenever we see an operation on a pg_class or pg_attribute tuple,
 *	we register a relcache flush operation for the relation described by that
 *	tuple.
 *
 *	We keep the relcache flush requests in lists separate from the catcache
 *	tuple flush requests.  This allows us to issue all the pending catcache
 *	flushes before we issue relcache flushes, which saves us from loading
 *	a catcache tuple during relcache load only to flush it again right away.
 *	Also, we avoid queuing multiple relcache flush requests for the same
 *	relation, since a relcache flush is relatively expensive to do.
 *	(XXX is it worth testing likewise for duplicate catcache flush entries?
 *	Probably not.)
 *
 *	All the request lists are kept in TopTransactionContext memory, since
 *	they need not live beyond the end of the current transaction.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/inval.c,v 1.45 2001/06/19 19:42:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"


/*
 * To minimize palloc traffic, we keep pending requests in successively-
 * larger chunks (a slightly more sophisticated version of an expansible
 * array).  All request types can be stored as SharedInvalidationMessage
 * records.
 */
typedef struct InvalidationChunk
{
	struct InvalidationChunk *next;	/* list link */
	int			nitems;			/* # items currently stored in chunk */
	int			maxitems;		/* size of allocated array in this chunk */
	SharedInvalidationMessage msgs[1]; /* VARIABLE LENGTH ARRAY */
} InvalidationChunk;			/* VARIABLE LENGTH STRUCTURE */

typedef struct InvalidationListHeader
{
	InvalidationChunk *cclist;	/* list of chunks holding catcache msgs */
	InvalidationChunk *rclist;	/* list of chunks holding relcache msgs */
} InvalidationListHeader;

/*
 * ----------------
 *	Invalidation info is divided into three parts.
 *	1) shared invalidation to be sent to all backends at commit
 *	2) local invalidation for the transaction itself (actually, just
 *	   for the current command within the transaction)
 *	3) rollback information for the transaction itself (in case we abort)
 * ----------------
 */

/*
 * head of invalidation message list for all backends
 * eaten by AtCommit_Cache() in CommitTransaction()
 */
static InvalidationListHeader GlobalInvalidMsgs;

/*
 * head of invalidation message list for the current command
 * eaten by AtCommit_LocalCache() in CommandCounterIncrement()
 */
static InvalidationListHeader LocalInvalidMsgs;

/*
 * head of rollback message list for abort-time processing
 * eaten by AtAbort_Cache() in AbortTransaction()
 */
static InvalidationListHeader RollbackMsgs;


/* ----------------------------------------------------------------
 *				Invalidation list support functions
 *
 * These three routines encapsulate processing of the "chunked"
 * representation of what is logically just a list of messages.
 * ----------------------------------------------------------------
 */

/*
 * AddInvalidationMessage
 *		Add an invalidation message to a list (of chunks).
 *
 * Note that we do not pay any great attention to maintaining the original
 * ordering of the messages.
 */
static void
AddInvalidationMessage(InvalidationChunk **listHdr,
					   SharedInvalidationMessage *msg)
{
	InvalidationChunk *chunk = *listHdr;

	if (chunk == NULL)
	{
		/* First time through; create initial chunk */
#define FIRSTCHUNKSIZE 16
		chunk = (InvalidationChunk *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(InvalidationChunk) +
							   (FIRSTCHUNKSIZE-1) * sizeof(SharedInvalidationMessage));
		chunk->nitems = 0;
		chunk->maxitems = FIRSTCHUNKSIZE;
		chunk->next = *listHdr;
		*listHdr = chunk;
	}
	else if (chunk->nitems >= chunk->maxitems)
	{
		/* Need another chunk; double size of last chunk */
		int		chunksize = 2 * chunk->maxitems;

		chunk = (InvalidationChunk *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(InvalidationChunk) +
							   (chunksize-1) * sizeof(SharedInvalidationMessage));
		chunk->nitems = 0;
		chunk->maxitems = chunksize;
		chunk->next = *listHdr;
		*listHdr = chunk;
	}
	/* Okay, add message to current chunk */
	chunk->msgs[chunk->nitems] = *msg;
	chunk->nitems++;
}

/*
 * Free a list of inval message chunks.
 *
 * NOTE: when we are about to commit or abort a transaction, it's
 * not really necessary to pfree the lists explicitly, since they will
 * go away anyway when TopTransactionContext is destroyed.
 */
static void
FreeInvalidationMessageList(InvalidationChunk **listHdr)
{
	InvalidationChunk *chunk = *listHdr;

	*listHdr = NULL;

	while (chunk != NULL)
	{
		InvalidationChunk *nextchunk = chunk->next;

		pfree(chunk);
		chunk = nextchunk;
	}
}

/*
 * Process a list of invalidation messages.
 *
 * This is a macro that executes the given code fragment for each message in
 * a message chunk list.  The fragment should refer to the message as *msg.
 */
#define ProcessMessageList(listHdr, codeFragment) \
	do { \
		InvalidationChunk *_chunk; \
		for (_chunk = (listHdr); _chunk != NULL; _chunk = _chunk->next) \
		{ \
			int		_cindex; \
			for (_cindex = 0; _cindex < _chunk->nitems; _cindex++) \
			{ \
				SharedInvalidationMessage *msg = &_chunk->msgs[_cindex]; \
				codeFragment; \
			} \
		} \
	} while (0)


/* ----------------------------------------------------------------
 *				Invalidation set support functions
 *
 * These routines understand about the division of a logical invalidation
 * list into separate physical lists for catcache and relcache entries.
 * ----------------------------------------------------------------
 */

/*
 * Add a catcache inval entry
 */
static void
AddCatcacheInvalidationMessage(InvalidationListHeader *hdr,
							   int id, Index hashIndex,
							   ItemPointer tuplePtr, Oid dbId)
{
	SharedInvalidationMessage msg;

	msg.cc.id = (int16) id;
	msg.cc.hashIndex = (uint16) hashIndex;
	msg.cc.dbId = dbId;
	msg.cc.tuplePtr = *tuplePtr;
	AddInvalidationMessage(&hdr->cclist, &msg);
}

/*
 * Add a relcache inval entry
 */
static void
AddRelcacheInvalidationMessage(InvalidationListHeader *hdr,
							   Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/* Don't add a duplicate item */
	/* We assume comparing relId is sufficient, needn't check dbId */
	ProcessMessageList(hdr->rclist,
					   if (msg->rc.relId == relId) return);

	/* OK, add the item */
	msg.rc.id = SHAREDINVALRELCACHE_ID;
	msg.rc.dbId = dbId;
	msg.rc.relId = relId;
	AddInvalidationMessage(&hdr->rclist, &msg);
}

/*
 * Reset an invalidation list to empty
 *
 * physicalFree may be set false if caller knows transaction is ending
 */
static void
DiscardInvalidationMessages(InvalidationListHeader *hdr, bool physicalFree)
{
	if (physicalFree)
	{
		/* Physically pfree the list data */
		FreeInvalidationMessageList(&hdr->cclist);
		FreeInvalidationMessageList(&hdr->rclist);
	}
	else
	{
		/* Assume the storage will go away at xact end, just reset pointers */
		hdr->cclist = NULL;
		hdr->rclist = NULL;
	}
}

/*
 * Execute the given function for all the messages in an invalidation list.
 * The list is not altered.
 *
 * catcache entries are processed first, for reasons mentioned above.
 */
static void
ProcessInvalidationMessages(InvalidationListHeader *hdr,
							void (*func) (SharedInvalidationMessage *msg))
{
	ProcessMessageList(hdr->cclist, func(msg));
	ProcessMessageList(hdr->rclist, func(msg));
}

/* ----------------------------------------------------------------
 *					  private support functions
 * ----------------------------------------------------------------
 */

/*
 * RegisterCatcacheInvalidation
 *
 * Register an invalidation event for an updated/deleted catcache entry.
 * We insert the event into both GlobalInvalidMsgs (for transmission
 * to other backends at transaction commit) and LocalInvalidMsgs (for
 * my local invalidation at end of command within xact).
 */
static void
RegisterCatcacheInvalidation(int cacheId,
							 Index hashIndex,
							 ItemPointer tuplePtr,
							 Oid dbId)
{
	AddCatcacheInvalidationMessage(&GlobalInvalidMsgs,
								   cacheId, hashIndex, tuplePtr, dbId);
	AddCatcacheInvalidationMessage(&LocalInvalidMsgs,
								   cacheId, hashIndex, tuplePtr, dbId);
}

/*
 * RegisterRelcacheInvalidation
 *
 * As above, but register a relcache invalidation event.
 */
static void
RegisterRelcacheInvalidation(Oid dbId, Oid relId)
{
	AddRelcacheInvalidationMessage(&GlobalInvalidMsgs,
								   dbId, relId);
	AddRelcacheInvalidationMessage(&LocalInvalidMsgs,
								   dbId, relId);
}

/*
 * RegisterCatcacheRollback
 *
 * Register an invalidation event for an inserted catcache entry.
 * This only needs to be flushed out of my local catcache, if I abort.
 */
static void
RegisterCatcacheRollback(int cacheId,
						 Index hashIndex,
						 ItemPointer tuplePtr,
						 Oid dbId)
{
	AddCatcacheInvalidationMessage(&RollbackMsgs,
								   cacheId, hashIndex, tuplePtr, dbId);
}

/*
 * RegisterRelcacheRollback
 *
 * As above, but register a relcache invalidation event.
 */
static void
RegisterRelcacheRollback(Oid dbId, Oid relId)
{
	AddRelcacheInvalidationMessage(&RollbackMsgs,
								   dbId, relId);
}

/*
 * LocalExecuteInvalidationMessage
 *
 * Process a single invalidation message (which could be either type).
 * Only the local caches are flushed; this does not transmit the message
 * to other backends.
 */
static void
LocalExecuteInvalidationMessage(SharedInvalidationMessage *msg)
{
	if (msg->id >= 0)
	{
		if (msg->cc.dbId == MyDatabaseId || msg->cc.dbId == 0)
			CatalogCacheIdInvalidate(msg->cc.id,
									 msg->cc.hashIndex,
									 &msg->cc.tuplePtr);
	}
	else if (msg->id == SHAREDINVALRELCACHE_ID)
	{
		if (msg->rc.dbId == MyDatabaseId || msg->rc.dbId == 0)
			RelationIdInvalidateRelationCacheByRelationId(msg->rc.relId);
	}
	else
	{
		elog(FATAL, "ExecuteInvalidationMessage: bogus message id %d",
			 msg->id);
	}
}

/*
 *		InvalidateSystemCaches
 *
 *		This blows away all tuples in the system catalog caches and
 *		all the cached relation descriptors (and closes their files too).
 *		Relation descriptors that have positive refcounts are then rebuilt.
 *
 *		We call this when we see a shared-inval-queue overflow signal,
 *		since that tells us we've lost some shared-inval messages and hence
 *		don't know what needs to be invalidated.
 */
static void
InvalidateSystemCaches(void)
{
	ResetCatalogCaches();
	RelationCacheInvalidate();
}

/*
 * PrepareForTupleInvalidation
 *		Invoke functions for the tuple which register invalidation
 *		of catalog/relation cache.
 */
static void
PrepareForTupleInvalidation(Relation relation, HeapTuple tuple,
							void (*CacheIdRegisterFunc) (int, Index,
														 ItemPointer, Oid),
							void (*RelationIdRegisterFunc) (Oid, Oid))
{
	Oid			tupleRelId;
	Oid			relationId;

	if (IsBootstrapProcessingMode())
		return;

	/*
	 * We only need to worry about invalidation for tuples that are in
	 * system relations; user-relation tuples are never in catcaches and
	 * can't affect the relcache either.
	 */
	if (!IsSystemRelationName(NameStr(RelationGetForm(relation)->relname)))
		return;

	/*
	 * First let the catcache do its thing
	 */
	PrepareToInvalidateCacheTuple(relation, tuple,
								  CacheIdRegisterFunc);

	/*
	 * Now, is this tuple one of the primary definers of a relcache entry?
	 */
	tupleRelId = RelationGetRelid(relation);

	if (tupleRelId == RelOid_pg_class)
		relationId = tuple->t_data->t_oid;
	else if (tupleRelId == RelOid_pg_attribute)
		relationId = ((Form_pg_attribute) GETSTRUCT(tuple))->attrelid;
	else
		return;

	/*
	 * Yes.  We need to register a relcache invalidation event for the
	 * relation identified by relationId.
	 *
	 * KLUGE ALERT: we always send the relcache event with MyDatabaseId,
	 * even if the rel in question is shared.  This essentially means that
	 * only backends in this same database will react to the relcache flush
	 * request.  This is in fact appropriate, since only those backends could
	 * see our pg_class or pg_attribute change anyway.  It looks a bit ugly
	 * though.
	 */
	(*RelationIdRegisterFunc) (MyDatabaseId, relationId);
}


/* ----------------------------------------------------------------
 *					  public functions
 * ----------------------------------------------------------------
 */

/*
 * AcceptInvalidationMessages
 *		Read and process invalidation messages from the shared invalidation
 *		message queue.
 *
 * Note:
 *		This should be called as the first step in processing a transaction.
 */
void
AcceptInvalidationMessages(void)
{
	ReceiveSharedInvalidMessages(LocalExecuteInvalidationMessage,
								 InvalidateSystemCaches);
}

/*
 * AtEOXactInvalidationMessages
 *		Process queued-up invalidation messages at end of transaction.
 *
 * If isCommit, we must send out the messages in our GlobalInvalidMsgs list
 * to the shared invalidation message queue.  Note that these will be read
 * not only by other backends, but also by our own backend at the next
 * transaction start (via AcceptInvalidationMessages).  Therefore, it's okay
 * to discard any pending LocalInvalidMsgs, since these will be redundant
 * with the global list.
 *
 * If not isCommit, we are aborting, and must locally process the messages
 * in our RollbackMsgs list.  No messages need be sent to other backends,
 * since they'll not have seen our changed tuples anyway.
 *
 * In any case, reset the various lists to empty.  We need not physically
 * free memory here, since TopTransactionContext is about to be emptied
 * anyway.
 *
 * Note:
 *		This should be called as the last step in processing a transaction.
 */
void
AtEOXactInvalidationMessages(bool isCommit)
{
	if (isCommit)
	{
		ProcessInvalidationMessages(&GlobalInvalidMsgs,
									SendSharedInvalidMessage);
	}
	else
	{
		ProcessInvalidationMessages(&RollbackMsgs,
									LocalExecuteInvalidationMessage);
	}

	DiscardInvalidationMessages(&GlobalInvalidMsgs, false);
	DiscardInvalidationMessages(&LocalInvalidMsgs, false);
	DiscardInvalidationMessages(&RollbackMsgs, false);
}

/*
 * CommandEndInvalidationMessages
 *		Process queued-up invalidation messages at end of one command
 *		in a transaction.
 *
 * Here, we send no messages to the shared queue, since we don't know yet if
 * we will commit.  But we do need to locally process the LocalInvalidMsgs
 * list, so as to flush our caches of any tuples we have outdated in the
 * current command.
 *
 * The isCommit = false case is not currently used, but may someday be
 * needed to support rollback to a savepoint within a transaction.
 * (I suspect it needs more work first --- tgl.)
 *
 * Note:
 *		This should be called during CommandCounterIncrement(),
 *		after we have advanced the command ID.
 */
void
CommandEndInvalidationMessages(bool isCommit)
{
	if (isCommit)
	{
		ProcessInvalidationMessages(&LocalInvalidMsgs,
									LocalExecuteInvalidationMessage);
	}
	else
	{
		ProcessInvalidationMessages(&RollbackMsgs,
									LocalExecuteInvalidationMessage);
	}
	/*
	 * LocalInvalidMsgs list is not interesting anymore, so flush it
	 * (for real).  Do *not* clear GlobalInvalidMsgs or RollbackMsgs.
	 */
	DiscardInvalidationMessages(&LocalInvalidMsgs, true);
}

/*
 * RelationInvalidateHeapTuple
 *		Register the given tuple for invalidation at end of command
 *		(ie, current command is outdating this tuple).
 */
void
RelationInvalidateHeapTuple(Relation relation, HeapTuple tuple)
{
	PrepareForTupleInvalidation(relation, tuple,
								RegisterCatcacheInvalidation,
								RegisterRelcacheInvalidation);
}

/*
 * RelationMark4RollbackHeapTuple
 *		Register the given tuple for invalidation in case of abort
 *		(ie, current command is creating this tuple).
 */
void
RelationMark4RollbackHeapTuple(Relation relation, HeapTuple tuple)
{
	PrepareForTupleInvalidation(relation, tuple,
								RegisterCatcacheRollback,
								RegisterRelcacheRollback);
}
