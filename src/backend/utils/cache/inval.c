/*-------------------------------------------------------------------------
 *
 * inval.c
 *	  POSTGRES cache invalidation dispatcher code.
 *
 *	This is subtle stuff, so pay attention:
 *
 *	When a tuple is updated or deleted, our standard time qualification rules
 *	consider that it is *still valid* so long as we are in the same command,
 *	ie, until the next CommandCounterIncrement() or transaction commit.
 *	(See utils/time/tqual.c, and note that system catalogs are generally
 *	scanned under SnapshotNow rules by the system, or plain user snapshots
 *	for user queries.)	At the command boundary, the old tuple stops
 *	being valid and the new version, if any, becomes valid.  Therefore,
 *	we cannot simply flush a tuple from the system caches during heap_update()
 *	or heap_delete().  The tuple is still good at that point; what's more,
 *	even if we did flush it, it might be reloaded into the caches by a later
 *	request in the same command.  So the correct behavior is to keep a list
 *	of outdated (updated/deleted) tuples and then do the required cache
 *	flushes at the next command boundary.  We must also keep track of
 *	inserted tuples so that we can flush "negative" cache entries that match
 *	the new tuples; again, that mustn't happen until end of command.
 *
 *	Once we have finished the command, we still need to remember inserted
 *	tuples (including new versions of updated tuples), so that we can flush
 *	them from the caches if we abort the transaction.  Similarly, we'd better
 *	be able to flush "negative" cache entries that may have been loaded in
 *	place of deleted tuples, so we still need the deleted ones too.
 *
 *	If we successfully complete the transaction, we have to broadcast all
 *	these invalidation events to other backends (via the SI message queue)
 *	so that they can flush obsolete entries from their caches.	Note we have
 *	to record the transaction commit before sending SI messages, otherwise
 *	the other backends won't see our updated tuples as good.
 *
 *	In short, we need to remember until xact end every insert or delete
 *	of a tuple that might be in the system caches.	Updates are treated as
 *	two events, delete + insert, for simplicity.  (There are cases where
 *	it'd be possible to record just one event, but we don't currently try.)
 *
 *	We do not need to register EVERY tuple operation in this way, just those
 *	on tuples in relations that have associated catcaches.	We do, however,
 *	have to register every operation on every tuple that *could* be in a
 *	catcache, whether or not it currently is in our cache.	Also, if the
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
 *	If a relcache flush is issued for a system relation that we preload
 *	from the relcache init file, we must also delete the init file so that
 *	it will be rebuilt during the next backend restart.  The actual work of
 *	manipulating the init file is in relcache.c, but we keep track of the
 *	need for it here.
 *
 *	All the request lists are kept in TopTransactionContext memory, since
 *	they need not live beyond the end of the current transaction.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/inval.c,v 1.58 2003/08/04 02:40:06 momjian Exp $
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
 * array).	All request types can be stored as SharedInvalidationMessage
 * records.
 */
typedef struct InvalidationChunk
{
	struct InvalidationChunk *next;		/* list link */
	int			nitems;			/* # items currently stored in chunk */
	int			maxitems;		/* size of allocated array in this chunk */
	SharedInvalidationMessage msgs[1];	/* VARIABLE LENGTH ARRAY */
} InvalidationChunk;			/* VARIABLE LENGTH STRUCTURE */

typedef struct InvalidationListHeader
{
	InvalidationChunk *cclist;	/* list of chunks holding catcache msgs */
	InvalidationChunk *rclist;	/* list of chunks holding relcache msgs */
} InvalidationListHeader;

/*----------------
 *	Invalidation info is divided into two lists:
 *	1) events so far in current command, not yet reflected to caches.
 *	2) events in previous commands of current transaction; these have
 *	   been reflected to local caches, and must be either broadcast to
 *	   other backends or rolled back from local cache when we commit
 *	   or abort the transaction.
 *
 * The relcache-file-invalidated flag can just be a simple boolean,
 * since we only act on it at transaction commit; we don't care which
 * command of the transaction set it.
 *----------------
 */

/* head of current-command event list */
static InvalidationListHeader CurrentCmdInvalidMsgs;

/* head of previous-commands event list */
static InvalidationListHeader PriorCmdInvalidMsgs;

static bool RelcacheInitFileInval;		/* init file must be invalidated? */

/*
 * Dynamically-registered callback functions.  Current implementation
 * assumes there won't be very many of these at once; could improve if needed.
 */

#define MAX_CACHE_CALLBACKS 20

static struct CACHECALLBACK
{
	int16		id;				/* cache number or SHAREDINVALRELCACHE_ID */
	CacheCallbackFunction function;
	Datum		arg;
}	cache_callback_list[MAX_CACHE_CALLBACKS];

static int	cache_callback_count = 0;


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
				(FIRSTCHUNKSIZE - 1) *sizeof(SharedInvalidationMessage));
		chunk->nitems = 0;
		chunk->maxitems = FIRSTCHUNKSIZE;
		chunk->next = *listHdr;
		*listHdr = chunk;
	}
	else if (chunk->nitems >= chunk->maxitems)
	{
		/* Need another chunk; double size of last chunk */
		int			chunksize = 2 * chunk->maxitems;

		chunk = (InvalidationChunk *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(InvalidationChunk) +
					 (chunksize - 1) *sizeof(SharedInvalidationMessage));
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
 * Append one list of invalidation message chunks to another, resetting
 * the source chunk-list pointer to NULL.
 */
static void
AppendInvalidationMessageList(InvalidationChunk **destHdr,
							  InvalidationChunk **srcHdr)
{
	InvalidationChunk *chunk = *srcHdr;

	if (chunk == NULL)
		return;					/* nothing to do */

	while (chunk->next != NULL)
		chunk = chunk->next;

	chunk->next = *destHdr;

	*destHdr = *srcHdr;

	*srcHdr = NULL;
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
							   int id, uint32 hashValue,
							   ItemPointer tuplePtr, Oid dbId)
{
	SharedInvalidationMessage msg;

	msg.cc.id = (int16) id;
	msg.cc.tuplePtr = *tuplePtr;
	msg.cc.dbId = dbId;
	msg.cc.hashValue = hashValue;
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
 * Append one list of invalidation messages to another, resetting
 * the source list to empty.
 */
static void
AppendInvalidationMessages(InvalidationListHeader *dest,
						   InvalidationListHeader *src)
{
	AppendInvalidationMessageList(&dest->cclist, &src->cclist);
	AppendInvalidationMessageList(&dest->rclist, &src->rclist);
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
		/*
		 * Assume the storage will go away at xact end, just reset
		 * pointers
		 */
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
 * Register an invalidation event for a catcache tuple entry.
 */
static void
RegisterCatcacheInvalidation(int cacheId,
							 uint32 hashValue,
							 ItemPointer tuplePtr,
							 Oid dbId)
{
	AddCatcacheInvalidationMessage(&CurrentCmdInvalidMsgs,
								   cacheId, hashValue, tuplePtr, dbId);
}

/*
 * RegisterRelcacheInvalidation
 *
 * As above, but register a relcache invalidation event.
 */
static void
RegisterRelcacheInvalidation(Oid dbId, Oid relId)
{
	AddRelcacheInvalidationMessage(&CurrentCmdInvalidMsgs,
								   dbId, relId);

	/*
	 * If the relation being invalidated is one of those cached in the
	 * relcache init file, mark that we need to zap that file at commit.
	 */
	if (RelationIdIsInInitFile(relId))
		RelcacheInitFileInval = true;
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
	int			i;

	if (msg->id >= 0)
	{
		if (msg->cc.dbId == MyDatabaseId || msg->cc.dbId == 0)
		{
			CatalogCacheIdInvalidate(msg->cc.id,
									 msg->cc.hashValue,
									 &msg->cc.tuplePtr);

			for (i = 0; i < cache_callback_count; i++)
			{
				struct CACHECALLBACK *ccitem = cache_callback_list + i;

				if (ccitem->id == msg->cc.id)
					(*ccitem->function) (ccitem->arg, InvalidOid);
			}
		}
	}
	else if (msg->id == SHAREDINVALRELCACHE_ID)
	{
		if (msg->rc.dbId == MyDatabaseId || msg->rc.dbId == 0)
		{
			RelationIdInvalidateRelationCacheByRelationId(msg->rc.relId);

			for (i = 0; i < cache_callback_count; i++)
			{
				struct CACHECALLBACK *ccitem = cache_callback_list + i;

				if (ccitem->id == SHAREDINVALRELCACHE_ID)
					(*ccitem->function) (ccitem->arg, msg->rc.relId);
			}
		}
	}
	else
		elog(FATAL, "unrecognized SI message id: %d", msg->id);
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
	int			i;

	ResetCatalogCaches();
	RelationCacheInvalidate();

	for (i = 0; i < cache_callback_count; i++)
	{
		struct CACHECALLBACK *ccitem = cache_callback_list + i;

		(*ccitem->function) (ccitem->arg, InvalidOid);
	}
}

/*
 * PrepareForTupleInvalidation
 *		Detect whether invalidation of this tuple implies invalidation
 *		of catalog/relation cache entries; if so, register inval events.
 */
static void
PrepareForTupleInvalidation(Relation relation, HeapTuple tuple,
							void (*CacheIdRegisterFunc) (int, uint32,
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
	if (!IsSystemRelation(relation))
		return;

	/*
	 * TOAST tuples can likewise be ignored here. Note that TOAST tables
	 * are considered system relations so they are not filtered by the
	 * above test.
	 */
	if (IsToastRelation(relation))
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
		relationId = HeapTupleGetOid(tuple);
	else if (tupleRelId == RelOid_pg_attribute)
		relationId = ((Form_pg_attribute) GETSTRUCT(tuple))->attrelid;
	else
		return;

	/*
	 * Yes.  We need to register a relcache invalidation event for the
	 * relation identified by relationId.
	 *
	 * KLUGE ALERT: we always send the relcache event with MyDatabaseId, even
	 * if the rel in question is shared.  This essentially means that only
	 * backends in this same database will react to the relcache flush
	 * request.  This is in fact appropriate, since only those backends
	 * could see our pg_class or pg_attribute change anyway.  It looks a
	 * bit ugly though.
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
 * If isCommit, we must send out the messages in our PriorCmdInvalidMsgs list
 * to the shared invalidation message queue.  Note that these will be read
 * not only by other backends, but also by our own backend at the next
 * transaction start (via AcceptInvalidationMessages).	This means that
 * we can skip immediate local processing of anything that's still in
 * CurrentCmdInvalidMsgs, and just send that list out too.
 *
 * If not isCommit, we are aborting, and must locally process the messages
 * in PriorCmdInvalidMsgs.	No messages need be sent to other backends,
 * since they'll not have seen our changed tuples anyway.  We can forget
 * about CurrentCmdInvalidMsgs too, since those changes haven't touched
 * the caches yet.
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
		/*
		 * Relcache init file invalidation requires processing both before
		 * and after we send the SI messages.  However, we need not do
		 * anything unless we committed.
		 */
		if (RelcacheInitFileInval)
			RelationCacheInitFileInvalidate(true);

		AppendInvalidationMessages(&PriorCmdInvalidMsgs,
								   &CurrentCmdInvalidMsgs);

		ProcessInvalidationMessages(&PriorCmdInvalidMsgs,
									SendSharedInvalidMessage);

		if (RelcacheInitFileInval)
			RelationCacheInitFileInvalidate(false);
	}
	else
	{
		ProcessInvalidationMessages(&PriorCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);
	}

	RelcacheInitFileInval = false;

	DiscardInvalidationMessages(&PriorCmdInvalidMsgs, false);
	DiscardInvalidationMessages(&CurrentCmdInvalidMsgs, false);
}

/*
 * CommandEndInvalidationMessages
 *		Process queued-up invalidation messages at end of one command
 *		in a transaction.
 *
 * Here, we send no messages to the shared queue, since we don't know yet if
 * we will commit.	We do need to locally process the CurrentCmdInvalidMsgs
 * list, so as to flush our caches of any entries we have outdated in the
 * current command.  We then move the current-cmd list over to become part
 * of the prior-cmds list.
 *
 * The isCommit = false case is not currently used, but may someday be
 * needed to support rollback to a savepoint within a transaction.
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
		ProcessInvalidationMessages(&CurrentCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);
		AppendInvalidationMessages(&PriorCmdInvalidMsgs,
								   &CurrentCmdInvalidMsgs);
	}
	else
	{
		/* XXX what needs to be done here? */
	}
}

/*
 * CacheInvalidateHeapTuple
 *		Register the given tuple for invalidation at end of command
 *		(ie, current command is outdating this tuple).
 */
void
CacheInvalidateHeapTuple(Relation relation, HeapTuple tuple)
{
	PrepareForTupleInvalidation(relation, tuple,
								RegisterCatcacheInvalidation,
								RegisterRelcacheInvalidation);
}

/*
 * CacheInvalidateRelcache
 *		Register invalidation of the specified relation's relcache entry
 *		at end of command.
 *
 * This is used in places that need to force relcache rebuild but aren't
 * changing any of the tuples recognized as contributors to the relcache
 * entry by PrepareForTupleInvalidation.  (An example is dropping an index.)
 */
void
CacheInvalidateRelcache(Oid relationId)
{
	/* See KLUGE ALERT in PrepareForTupleInvalidation */
	RegisterRelcacheInvalidation(MyDatabaseId, relationId);
}

/*
 * CacheRegisterSyscacheCallback
 *		Register the specified function to be called for all future
 *		invalidation events in the specified cache.
 *
 * NOTE: currently, the OID argument to the callback routine is not
 * provided for syscache callbacks; the routine doesn't really get any
 * useful info as to exactly what changed.	It should treat every call
 * as a "cache flush" request.
 */
void
CacheRegisterSyscacheCallback(int cacheid,
							  CacheCallbackFunction func,
							  Datum arg)
{
	if (cache_callback_count >= MAX_CACHE_CALLBACKS)
		elog(FATAL, "out of cache_callback_list slots");

	cache_callback_list[cache_callback_count].id = cacheid;
	cache_callback_list[cache_callback_count].function = func;
	cache_callback_list[cache_callback_count].arg = arg;

	++cache_callback_count;
}

/*
 * CacheRegisterRelcacheCallback
 *		Register the specified function to be called for all future
 *		relcache invalidation events.  The OID of the relation being
 *		invalidated will be passed to the function.
 *
 * NOTE: InvalidOid will be passed if a cache reset request is received.
 * In this case the called routines should flush all cached state.
 */
void
CacheRegisterRelcacheCallback(CacheCallbackFunction func,
							  Datum arg)
{
	if (cache_callback_count >= MAX_CACHE_CALLBACKS)
		elog(FATAL, "out of cache_callback_list slots");

	cache_callback_list[cache_callback_count].id = SHAREDINVALRELCACHE_ID;
	cache_callback_list[cache_callback_count].function = func;
	cache_callback_list[cache_callback_count].arg = arg;

	++cache_callback_count;
}
