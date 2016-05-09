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
 *	scanned under the most current snapshot available, rather than the
 *	transaction snapshot.)	At the command boundary, the old tuple stops
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
 *	so that they can flush obsolete entries from their caches.  Note we have
 *	to record the transaction commit before sending SI messages, otherwise
 *	the other backends won't see our updated tuples as good.
 *
 *	When a subtransaction aborts, we can process and discard any events
 *	it has queued.  When a subtransaction commits, we just add its events
 *	to the pending lists of the parent transaction.
 *
 *	In short, we need to remember until xact end every insert or delete
 *	of a tuple that might be in the system caches.  Updates are treated as
 *	two events, delete + insert, for simplicity.  (If the update doesn't
 *	change the tuple hash value, catcache.c optimizes this into one event.)
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
 *	If a relcache flush is issued for a system relation that we preload
 *	from the relcache init file, we must also delete the init file so that
 *	it will be rebuilt during the next backend restart.  The actual work of
 *	manipulating the init file is in relcache.c, but we keep track of the
 *	need for it here.
 *
 *	The request lists proper are kept in CurTransactionContext of their
 *	creating (sub)transaction, since they can be forgotten on abort of that
 *	transaction but must be kept till top-level commit otherwise.  For
 *	simplicity we keep the controlling list-of-lists in TopTransactionContext.
 *
 *	Currently, inval messages are sent without regard for the possibility
 *	that the object described by the catalog tuple might be a session-local
 *	object such as a temporary table.  This is because (1) this code has
 *	no practical way to tell the difference, and (2) it is not certain that
 *	other backends don't have catalog cache or even relcache entries for
 *	such tables, anyway; there is nothing that prevents that.  It might be
 *	worth trying to avoid sending such inval traffic in the future, if those
 *	problems can be overcome cheaply.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/inval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * To minimize palloc traffic, we keep pending requests in successively-
 * larger chunks (a slightly more sophisticated version of an expansible
 * array).  All request types can be stored as SharedInvalidationMessage
 * records.  The ordering of requests within a list is never significant.
 */
typedef struct InvalidationChunk
{
	struct InvalidationChunk *next;		/* list link */
	int			nitems;			/* # items currently stored in chunk */
	int			maxitems;		/* size of allocated array in this chunk */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} InvalidationChunk;

typedef struct InvalidationListHeader
{
	InvalidationChunk *cclist;	/* list of chunks holding catcache msgs */
	InvalidationChunk *rclist;	/* list of chunks holding relcache msgs */
} InvalidationListHeader;

/*----------------
 * Invalidation info is divided into two lists:
 *	1) events so far in current command, not yet reflected to caches.
 *	2) events in previous commands of current transaction; these have
 *	   been reflected to local caches, and must be either broadcast to
 *	   other backends or rolled back from local cache when we commit
 *	   or abort the transaction.
 * Actually, we need two such lists for each level of nested transaction,
 * so that we can discard events from an aborted subtransaction.  When
 * a subtransaction commits, we append its lists to the parent's lists.
 *
 * The relcache-file-invalidated flag can just be a simple boolean,
 * since we only act on it at transaction commit; we don't care which
 * command of the transaction set it.
 *----------------
 */

typedef struct TransInvalidationInfo
{
	/* Back link to parent transaction's info */
	struct TransInvalidationInfo *parent;

	/* Subtransaction nesting depth */
	int			my_level;

	/* head of current-command event list */
	InvalidationListHeader CurrentCmdInvalidMsgs;

	/* head of previous-commands event list */
	InvalidationListHeader PriorCmdInvalidMsgs;

	/* init file must be invalidated? */
	bool		RelcacheInitFileInval;
} TransInvalidationInfo;

static TransInvalidationInfo *transInvalInfo = NULL;

static SharedInvalidationMessage *SharedInvalidMessagesArray;
static int	numSharedInvalidMessagesArray;
static int	maxSharedInvalidMessagesArray;


/*
 * Dynamically-registered callback functions.  Current implementation
 * assumes there won't be very many of these at once; could improve if needed.
 */

#define MAX_SYSCACHE_CALLBACKS 32
#define MAX_RELCACHE_CALLBACKS 10

static struct SYSCACHECALLBACK
{
	int16		id;				/* cache number */
	SyscacheCallbackFunction function;
	Datum		arg;
}	syscache_callback_list[MAX_SYSCACHE_CALLBACKS];

static int	syscache_callback_count = 0;

static struct RELCACHECALLBACK
{
	RelcacheCallbackFunction function;
	Datum		arg;
}	relcache_callback_list[MAX_RELCACHE_CALLBACKS];

static int	relcache_callback_count = 0;

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
#define FIRSTCHUNKSIZE 32
		chunk = (InvalidationChunk *)
			MemoryContextAlloc(CurTransactionContext,
							   offsetof(InvalidationChunk, msgs) +
						 FIRSTCHUNKSIZE * sizeof(SharedInvalidationMessage));
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
			MemoryContextAlloc(CurTransactionContext,
							   offsetof(InvalidationChunk, msgs) +
							   chunksize * sizeof(SharedInvalidationMessage));
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

/*
 * Process a list of invalidation messages group-wise.
 *
 * As above, but the code fragment can handle an array of messages.
 * The fragment should refer to the messages as msgs[], with n entries.
 */
#define ProcessMessageListMulti(listHdr, codeFragment) \
	do { \
		InvalidationChunk *_chunk; \
		for (_chunk = (listHdr); _chunk != NULL; _chunk = _chunk->next) \
		{ \
			SharedInvalidationMessage *msgs = _chunk->msgs; \
			int		n = _chunk->nitems; \
			codeFragment; \
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
							   int id, uint32 hashValue, Oid dbId)
{
	SharedInvalidationMessage msg;

	Assert(id < CHAR_MAX);
	msg.cc.id = (int8) id;
	msg.cc.dbId = dbId;
	msg.cc.hashValue = hashValue;

	/*
	 * Define padding bytes in SharedInvalidationMessage structs to be
	 * defined. Otherwise the sinvaladt.c ringbuffer, which is accessed by
	 * multiple processes, will cause spurious valgrind warnings about
	 * undefined memory being used. That's because valgrind remembers the
	 * undefined bytes from the last local process's store, not realizing that
	 * another process has written since, filling the previously uninitialized
	 * bytes
	 */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(&hdr->cclist, &msg);
}

/*
 * Add a whole-catalog inval entry
 */
static void
AddCatalogInvalidationMessage(InvalidationListHeader *hdr,
							  Oid dbId, Oid catId)
{
	SharedInvalidationMessage msg;

	msg.cat.id = SHAREDINVALCATALOG_ID;
	msg.cat.dbId = dbId;
	msg.cat.catId = catId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

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
	/* We assume dbId need not be checked because it will never change */
	ProcessMessageList(hdr->rclist,
					   if (msg->rc.id == SHAREDINVALRELCACHE_ID &&
						   msg->rc.relId == relId)
					   return);

	/* OK, add the item */
	msg.rc.id = SHAREDINVALRELCACHE_ID;
	msg.rc.dbId = dbId;
	msg.rc.relId = relId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(&hdr->rclist, &msg);
}

/*
 * Add a snapshot inval entry
 */
static void
AddSnapshotInvalidationMessage(InvalidationListHeader *hdr,
							   Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/* Don't add a duplicate item */
	/* We assume dbId need not be checked because it will never change */
	ProcessMessageList(hdr->rclist,
					   if (msg->sn.id == SHAREDINVALSNAPSHOT_ID &&
						   msg->sn.relId == relId)
					   return);

	/* OK, add the item */
	msg.sn.id = SHAREDINVALSNAPSHOT_ID;
	msg.sn.dbId = dbId;
	msg.sn.relId = relId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

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

/*
 * As above, but the function is able to process an array of messages
 * rather than just one at a time.
 */
static void
ProcessInvalidationMessagesMulti(InvalidationListHeader *hdr,
				 void (*func) (const SharedInvalidationMessage *msgs, int n))
{
	ProcessMessageListMulti(hdr->cclist, func(msgs, n));
	ProcessMessageListMulti(hdr->rclist, func(msgs, n));
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
							 Oid dbId)
{
	AddCatcacheInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								   cacheId, hashValue, dbId);
}

/*
 * RegisterCatalogInvalidation
 *
 * Register an invalidation event for all catcache entries from a catalog.
 */
static void
RegisterCatalogInvalidation(Oid dbId, Oid catId)
{
	AddCatalogInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								  dbId, catId);
}

/*
 * RegisterRelcacheInvalidation
 *
 * As above, but register a relcache invalidation event.
 */
static void
RegisterRelcacheInvalidation(Oid dbId, Oid relId)
{
	AddRelcacheInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								   dbId, relId);

	/*
	 * Most of the time, relcache invalidation is associated with system
	 * catalog updates, but there are a few cases where it isn't.  Quick hack
	 * to ensure that the next CommandCounterIncrement() will think that we
	 * need to do CommandEndInvalidationMessages().
	 */
	(void) GetCurrentCommandId(true);

	/*
	 * If the relation being invalidated is one of those cached in the local
	 * relcache init file, mark that we need to zap that file at commit.
	 */
	if (OidIsValid(dbId) && RelationIdIsInInitFile(relId))
		transInvalInfo->RelcacheInitFileInval = true;
}

/*
 * RegisterSnapshotInvalidation
 *
 * Register an invalidation event for MVCC scans against a given catalog.
 * Only needed for catalogs that don't have catcaches.
 */
static void
RegisterSnapshotInvalidation(Oid dbId, Oid relId)
{
	AddSnapshotInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								   dbId, relId);
}

/*
 * LocalExecuteInvalidationMessage
 *
 * Process a single invalidation message (which could be of any type).
 * Only the local caches are flushed; this does not transmit the message
 * to other backends.
 */
void
LocalExecuteInvalidationMessage(SharedInvalidationMessage *msg)
{
	if (msg->id >= 0)
	{
		if (msg->cc.dbId == MyDatabaseId || msg->cc.dbId == InvalidOid)
		{
			InvalidateCatalogSnapshot();

			CatalogCacheIdInvalidate(msg->cc.id, msg->cc.hashValue);

			CallSyscacheCallbacks(msg->cc.id, msg->cc.hashValue);
		}
	}
	else if (msg->id == SHAREDINVALCATALOG_ID)
	{
		if (msg->cat.dbId == MyDatabaseId || msg->cat.dbId == InvalidOid)
		{
			InvalidateCatalogSnapshot();

			CatalogCacheFlushCatalog(msg->cat.catId);

			/* CatalogCacheFlushCatalog calls CallSyscacheCallbacks as needed */
		}
	}
	else if (msg->id == SHAREDINVALRELCACHE_ID)
	{
		if (msg->rc.dbId == MyDatabaseId || msg->rc.dbId == InvalidOid)
		{
			int			i;

			RelationCacheInvalidateEntry(msg->rc.relId);

			for (i = 0; i < relcache_callback_count; i++)
			{
				struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

				(*ccitem->function) (ccitem->arg, msg->rc.relId);
			}
		}
	}
	else if (msg->id == SHAREDINVALSMGR_ID)
	{
		/*
		 * We could have smgr entries for relations of other databases, so no
		 * short-circuit test is possible here.
		 */
		RelFileNodeBackend rnode;

		rnode.node = msg->sm.rnode;
		rnode.backend = (msg->sm.backend_hi << 16) | (int) msg->sm.backend_lo;
		smgrclosenode(rnode);
	}
	else if (msg->id == SHAREDINVALRELMAP_ID)
	{
		/* We only care about our own database and shared catalogs */
		if (msg->rm.dbId == InvalidOid)
			RelationMapInvalidate(true);
		else if (msg->rm.dbId == MyDatabaseId)
			RelationMapInvalidate(false);
	}
	else if (msg->id == SHAREDINVALSNAPSHOT_ID)
	{
		/* We only care about our own database and shared catalogs */
		if (msg->rm.dbId == InvalidOid)
			InvalidateCatalogSnapshot();
		else if (msg->rm.dbId == MyDatabaseId)
			InvalidateCatalogSnapshot();
	}
	else
		elog(FATAL, "unrecognized SI message ID: %d", msg->id);
}

/*
 *		InvalidateSystemCaches
 *
 *		This blows away all tuples in the system catalog caches and
 *		all the cached relation descriptors and smgr cache entries.
 *		Relation descriptors that have positive refcounts are then rebuilt.
 *
 *		We call this when we see a shared-inval-queue overflow signal,
 *		since that tells us we've lost some shared-inval messages and hence
 *		don't know what needs to be invalidated.
 */
void
InvalidateSystemCaches(void)
{
	int			i;

	InvalidateCatalogSnapshot();
	ResetCatalogCaches();
	RelationCacheInvalidate();	/* gets smgr and relmap too */

	for (i = 0; i < syscache_callback_count; i++)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		(*ccitem->function) (ccitem->arg, ccitem->id, 0);
	}

	for (i = 0; i < relcache_callback_count; i++)
	{
		struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

		(*ccitem->function) (ccitem->arg, InvalidOid);
	}
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

	/*
	 * Test code to force cache flushes anytime a flush could happen.
	 *
	 * If used with CLOBBER_FREED_MEMORY, CLOBBER_CACHE_ALWAYS provides a
	 * fairly thorough test that the system contains no cache-flush hazards.
	 * However, it also makes the system unbelievably slow --- the regression
	 * tests take about 100 times longer than normal.
	 *
	 * If you're a glutton for punishment, try CLOBBER_CACHE_RECURSIVELY. This
	 * slows things by at least a factor of 10000, so I wouldn't suggest
	 * trying to run the entire regression tests that way.  It's useful to try
	 * a few simple tests, to make sure that cache reload isn't subject to
	 * internal cache-flush hazards, but after you've done a few thousand
	 * recursive reloads it's unlikely you'll learn more.
	 */
#if defined(CLOBBER_CACHE_ALWAYS)
	{
		static bool in_recursion = false;

		if (!in_recursion)
		{
			in_recursion = true;
			InvalidateSystemCaches();
			in_recursion = false;
		}
	}
#elif defined(CLOBBER_CACHE_RECURSIVELY)
	InvalidateSystemCaches();
#endif
}

/*
 * PrepareInvalidationState
 *		Initialize inval lists for the current (sub)transaction.
 */
static void
PrepareInvalidationState(void)
{
	TransInvalidationInfo *myInfo;

	if (transInvalInfo != NULL &&
		transInvalInfo->my_level == GetCurrentTransactionNestLevel())
		return;

	myInfo = (TransInvalidationInfo *)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(TransInvalidationInfo));
	myInfo->parent = transInvalInfo;
	myInfo->my_level = GetCurrentTransactionNestLevel();

	/*
	 * If there's any previous entry, this one should be for a deeper nesting
	 * level.
	 */
	Assert(transInvalInfo == NULL ||
		   myInfo->my_level > transInvalInfo->my_level);

	transInvalInfo = myInfo;
}

/*
 * PostPrepare_Inval
 *		Clean up after successful PREPARE.
 *
 * Here, we want to act as though the transaction aborted, so that we will
 * undo any syscache changes it made, thereby bringing us into sync with the
 * outside world, which doesn't believe the transaction committed yet.
 *
 * If the prepared transaction is later aborted, there is nothing more to
 * do; if it commits, we will receive the consequent inval messages just
 * like everyone else.
 */
void
PostPrepare_Inval(void)
{
	AtEOXact_Inval(false);
}

/*
 * Collect invalidation messages into SharedInvalidMessagesArray array.
 */
static void
MakeSharedInvalidMessagesArray(const SharedInvalidationMessage *msgs, int n)
{
	/*
	 * Initialise array first time through in each commit
	 */
	if (SharedInvalidMessagesArray == NULL)
	{
		maxSharedInvalidMessagesArray = FIRSTCHUNKSIZE;
		numSharedInvalidMessagesArray = 0;

		/*
		 * Although this is being palloc'd we don't actually free it directly.
		 * We're so close to EOXact that we now we're going to lose it anyhow.
		 */
		SharedInvalidMessagesArray = palloc(maxSharedInvalidMessagesArray
										* sizeof(SharedInvalidationMessage));
	}

	if ((numSharedInvalidMessagesArray + n) > maxSharedInvalidMessagesArray)
	{
		while ((numSharedInvalidMessagesArray + n) > maxSharedInvalidMessagesArray)
			maxSharedInvalidMessagesArray *= 2;

		SharedInvalidMessagesArray = repalloc(SharedInvalidMessagesArray,
											  maxSharedInvalidMessagesArray
										* sizeof(SharedInvalidationMessage));
	}

	/*
	 * Append the next chunk onto the array
	 */
	memcpy(SharedInvalidMessagesArray + numSharedInvalidMessagesArray,
		   msgs, n * sizeof(SharedInvalidationMessage));
	numSharedInvalidMessagesArray += n;
}

/*
 * xactGetCommittedInvalidationMessages() is executed by
 * RecordTransactionCommit() to add invalidation messages onto the
 * commit record. This applies only to commit message types, never to
 * abort records. Must always run before AtEOXact_Inval(), since that
 * removes the data we need to see.
 *
 * Remember that this runs before we have officially committed, so we
 * must not do anything here to change what might occur *if* we should
 * fail between here and the actual commit.
 *
 * see also xact_redo_commit() and xact_desc_commit()
 */
int
xactGetCommittedInvalidationMessages(SharedInvalidationMessage **msgs,
									 bool *RelcacheInitFileInval)
{
	MemoryContext oldcontext;

	/* Quick exit if we haven't done anything with invalidation messages. */
	if (transInvalInfo == NULL)
	{
		*RelcacheInitFileInval = false;
		*msgs = NULL;
		return 0;
	}

	/* Must be at top of stack */
	Assert(transInvalInfo->my_level == 1 && transInvalInfo->parent == NULL);

	/*
	 * Relcache init file invalidation requires processing both before and
	 * after we send the SI messages.  However, we need not do anything unless
	 * we committed.
	 */
	*RelcacheInitFileInval = transInvalInfo->RelcacheInitFileInval;

	/*
	 * Walk through TransInvalidationInfo to collect all the messages into a
	 * single contiguous array of invalidation messages. It must be contiguous
	 * so we can copy directly into WAL message. Maintain the order that they
	 * would be processed in by AtEOXact_Inval(), to ensure emulated behaviour
	 * in redo is as similar as possible to original. We want the same bugs,
	 * if any, not new ones.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	ProcessInvalidationMessagesMulti(&transInvalInfo->CurrentCmdInvalidMsgs,
									 MakeSharedInvalidMessagesArray);
	ProcessInvalidationMessagesMulti(&transInvalInfo->PriorCmdInvalidMsgs,
									 MakeSharedInvalidMessagesArray);
	MemoryContextSwitchTo(oldcontext);

	Assert(!(numSharedInvalidMessagesArray > 0 &&
			 SharedInvalidMessagesArray == NULL));

	*msgs = SharedInvalidMessagesArray;

	return numSharedInvalidMessagesArray;
}

/*
 * ProcessCommittedInvalidationMessages is executed by xact_redo_commit() or
 * standby_redo() to process invalidation messages. Currently that happens
 * only at end-of-xact.
 *
 * Relcache init file invalidation requires processing both
 * before and after we send the SI messages. See AtEOXact_Inval()
 */
void
ProcessCommittedInvalidationMessages(SharedInvalidationMessage *msgs,
									 int nmsgs, bool RelcacheInitFileInval,
									 Oid dbid, Oid tsid)
{
	if (nmsgs <= 0)
		return;

	elog(trace_recovery(DEBUG4), "replaying commit with %d messages%s", nmsgs,
		 (RelcacheInitFileInval ? " and relcache file invalidation" : ""));

	if (RelcacheInitFileInval)
	{
		/*
		 * RelationCacheInitFilePreInvalidate requires DatabasePath to be set,
		 * but we should not use SetDatabasePath during recovery, since it is
		 * intended to be used only once by normal backends.  Hence, a quick
		 * hack: set DatabasePath directly then unset after use.
		 */
		DatabasePath = GetDatabasePath(dbid, tsid);
		elog(trace_recovery(DEBUG4), "removing relcache init file in \"%s\"",
			 DatabasePath);
		RelationCacheInitFilePreInvalidate();
		pfree(DatabasePath);
		DatabasePath = NULL;
	}

	SendSharedInvalidMessages(msgs, nmsgs);

	if (RelcacheInitFileInval)
		RelationCacheInitFilePostInvalidate();
}

/*
 * AtEOXact_Inval
 *		Process queued-up invalidation messages at end of main transaction.
 *
 * If isCommit, we must send out the messages in our PriorCmdInvalidMsgs list
 * to the shared invalidation message queue.  Note that these will be read
 * not only by other backends, but also by our own backend at the next
 * transaction start (via AcceptInvalidationMessages).  This means that
 * we can skip immediate local processing of anything that's still in
 * CurrentCmdInvalidMsgs, and just send that list out too.
 *
 * If not isCommit, we are aborting, and must locally process the messages
 * in PriorCmdInvalidMsgs.  No messages need be sent to other backends,
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
AtEOXact_Inval(bool isCommit)
{
	/* Quick exit if no messages */
	if (transInvalInfo == NULL)
		return;

	/* Must be at top of stack */
	Assert(transInvalInfo->my_level == 1 && transInvalInfo->parent == NULL);

	if (isCommit)
	{
		/*
		 * Relcache init file invalidation requires processing both before and
		 * after we send the SI messages.  However, we need not do anything
		 * unless we committed.
		 */
		if (transInvalInfo->RelcacheInitFileInval)
			RelationCacheInitFilePreInvalidate();

		AppendInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
								   &transInvalInfo->CurrentCmdInvalidMsgs);

		ProcessInvalidationMessagesMulti(&transInvalInfo->PriorCmdInvalidMsgs,
										 SendSharedInvalidMessages);

		if (transInvalInfo->RelcacheInitFileInval)
			RelationCacheInitFilePostInvalidate();
	}
	else
	{
		ProcessInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);
	}

	/* Need not free anything explicitly */
	transInvalInfo = NULL;
	SharedInvalidMessagesArray = NULL;
	numSharedInvalidMessagesArray = 0;
}

/*
 * AtEOSubXact_Inval
 *		Process queued-up invalidation messages at end of subtransaction.
 *
 * If isCommit, process CurrentCmdInvalidMsgs if any (there probably aren't),
 * and then attach both CurrentCmdInvalidMsgs and PriorCmdInvalidMsgs to the
 * parent's PriorCmdInvalidMsgs list.
 *
 * If not isCommit, we are aborting, and must locally process the messages
 * in PriorCmdInvalidMsgs.  No messages need be sent to other backends.
 * We can forget about CurrentCmdInvalidMsgs too, since those changes haven't
 * touched the caches yet.
 *
 * In any case, pop the transaction stack.  We need not physically free memory
 * here, since CurTransactionContext is about to be emptied anyway
 * (if aborting).  Beware of the possibility of aborting the same nesting
 * level twice, though.
 */
void
AtEOSubXact_Inval(bool isCommit)
{
	int			my_level;
	TransInvalidationInfo *myInfo = transInvalInfo;

	/* Quick exit if no messages. */
	if (myInfo == NULL)
		return;

	/* Also bail out quickly if messages are not for this level. */
	my_level = GetCurrentTransactionNestLevel();
	if (myInfo->my_level != my_level)
	{
		Assert(myInfo->my_level < my_level);
		return;
	}

	if (isCommit)
	{
		/* If CurrentCmdInvalidMsgs still has anything, fix it */
		CommandEndInvalidationMessages();

		/*
		 * We create invalidation stack entries lazily, so the parent might
		 * not have one.  Instead of creating one, moving all the data over,
		 * and then freeing our own, we can just adjust the level of our own
		 * entry.
		 */
		if (myInfo->parent == NULL || myInfo->parent->my_level < my_level - 1)
		{
			myInfo->my_level--;
			return;
		}

		/* Pass up my inval messages to parent */
		AppendInvalidationMessages(&myInfo->parent->PriorCmdInvalidMsgs,
								   &myInfo->PriorCmdInvalidMsgs);

		/* Pending relcache inval becomes parent's problem too */
		if (myInfo->RelcacheInitFileInval)
			myInfo->parent->RelcacheInitFileInval = true;

		/* Pop the transaction state stack */
		transInvalInfo = myInfo->parent;

		/* Need not free anything else explicitly */
		pfree(myInfo);
	}
	else
	{
		ProcessInvalidationMessages(&myInfo->PriorCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);

		/* Pop the transaction state stack */
		transInvalInfo = myInfo->parent;

		/* Need not free anything else explicitly */
		pfree(myInfo);
	}
}

/*
 * CommandEndInvalidationMessages
 *		Process queued-up invalidation messages at end of one command
 *		in a transaction.
 *
 * Here, we send no messages to the shared queue, since we don't know yet if
 * we will commit.  We do need to locally process the CurrentCmdInvalidMsgs
 * list, so as to flush our caches of any entries we have outdated in the
 * current command.  We then move the current-cmd list over to become part
 * of the prior-cmds list.
 *
 * Note:
 *		This should be called during CommandCounterIncrement(),
 *		after we have advanced the command ID.
 */
void
CommandEndInvalidationMessages(void)
{
	/*
	 * You might think this shouldn't be called outside any transaction, but
	 * bootstrap does it, and also ABORT issued when not in a transaction. So
	 * just quietly return if no state to work on.
	 */
	if (transInvalInfo == NULL)
		return;

	ProcessInvalidationMessages(&transInvalInfo->CurrentCmdInvalidMsgs,
								LocalExecuteInvalidationMessage);
	AppendInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
							   &transInvalInfo->CurrentCmdInvalidMsgs);
}


/*
 * CacheInvalidateHeapTuple
 *		Register the given tuple for invalidation at end of command
 *		(ie, current command is creating or outdating this tuple).
 *		Also, detect whether a relcache invalidation is implied.
 *
 * For an insert or delete, tuple is the target tuple and newtuple is NULL.
 * For an update, we are called just once, with tuple being the old tuple
 * version and newtuple the new version.  This allows avoidance of duplicate
 * effort during an update.
 */
void
CacheInvalidateHeapTuple(Relation relation,
						 HeapTuple tuple,
						 HeapTuple newtuple)
{
	Oid			tupleRelId;
	Oid			databaseId;
	Oid			relationId;

	/* Do nothing during bootstrap */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * We only need to worry about invalidation for tuples that are in system
	 * catalogs; user-relation tuples are never in catcaches and can't affect
	 * the relcache either.
	 */
	if (!IsCatalogRelation(relation))
		return;

	/*
	 * IsCatalogRelation() will return true for TOAST tables of system
	 * catalogs, but we don't care about those, either.
	 */
	if (IsToastRelation(relation))
		return;

	/*
	 * If we're not prepared to queue invalidation messages for this
	 * subtransaction level, get ready now.
	 */
	PrepareInvalidationState();

	/*
	 * First let the catcache do its thing
	 */
	tupleRelId = RelationGetRelid(relation);
	if (RelationInvalidatesSnapshotsOnly(tupleRelId))
	{
		databaseId = IsSharedRelation(tupleRelId) ? InvalidOid : MyDatabaseId;
		RegisterSnapshotInvalidation(databaseId, tupleRelId);
	}
	else
		PrepareToInvalidateCacheTuple(relation, tuple, newtuple,
									  RegisterCatcacheInvalidation);

	/*
	 * Now, is this tuple one of the primary definers of a relcache entry?
	 *
	 * Note we ignore newtuple here; we assume an update cannot move a tuple
	 * from being part of one relcache entry to being part of another.
	 */
	if (tupleRelId == RelationRelationId)
	{
		Form_pg_class classtup = (Form_pg_class) GETSTRUCT(tuple);

		relationId = HeapTupleGetOid(tuple);
		if (classtup->relisshared)
			databaseId = InvalidOid;
		else
			databaseId = MyDatabaseId;
	}
	else if (tupleRelId == AttributeRelationId)
	{
		Form_pg_attribute atttup = (Form_pg_attribute) GETSTRUCT(tuple);

		relationId = atttup->attrelid;

		/*
		 * KLUGE ALERT: we always send the relcache event with MyDatabaseId,
		 * even if the rel in question is shared (which we can't easily tell).
		 * This essentially means that only backends in this same database
		 * will react to the relcache flush request.  This is in fact
		 * appropriate, since only those backends could see our pg_attribute
		 * change anyway.  It looks a bit ugly though.  (In practice, shared
		 * relations can't have schema changes after bootstrap, so we should
		 * never come here for a shared rel anyway.)
		 */
		databaseId = MyDatabaseId;
	}
	else if (tupleRelId == IndexRelationId)
	{
		Form_pg_index indextup = (Form_pg_index) GETSTRUCT(tuple);

		/*
		 * When a pg_index row is updated, we should send out a relcache inval
		 * for the index relation.  As above, we don't know the shared status
		 * of the index, but in practice it doesn't matter since indexes of
		 * shared catalogs can't have such updates.
		 */
		relationId = indextup->indexrelid;
		databaseId = MyDatabaseId;
	}
	else
		return;

	/*
	 * Yes.  We need to register a relcache invalidation event.
	 */
	RegisterRelcacheInvalidation(databaseId, relationId);
}

/*
 * CacheInvalidateCatalog
 *		Register invalidation of the whole content of a system catalog.
 *
 * This is normally used in VACUUM FULL/CLUSTER, where we haven't so much
 * changed any tuples as moved them around.  Some uses of catcache entries
 * expect their TIDs to be correct, so we have to blow away the entries.
 *
 * Note: we expect caller to verify that the rel actually is a system
 * catalog.  If it isn't, no great harm is done, just a wasted sinval message.
 */
void
CacheInvalidateCatalog(Oid catalogId)
{
	Oid			databaseId;

	PrepareInvalidationState();

	if (IsSharedRelation(catalogId))
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;

	RegisterCatalogInvalidation(databaseId, catalogId);
}

/*
 * CacheInvalidateRelcache
 *		Register invalidation of the specified relation's relcache entry
 *		at end of command.
 *
 * This is used in places that need to force relcache rebuild but aren't
 * changing any of the tuples recognized as contributors to the relcache
 * entry by CacheInvalidateHeapTuple.  (An example is dropping an index.)
 */
void
CacheInvalidateRelcache(Relation relation)
{
	Oid			databaseId;
	Oid			relationId;

	PrepareInvalidationState();

	relationId = RelationGetRelid(relation);
	if (relation->rd_rel->relisshared)
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;

	RegisterRelcacheInvalidation(databaseId, relationId);
}

/*
 * CacheInvalidateRelcacheByTuple
 *		As above, but relation is identified by passing its pg_class tuple.
 */
void
CacheInvalidateRelcacheByTuple(HeapTuple classTuple)
{
	Form_pg_class classtup = (Form_pg_class) GETSTRUCT(classTuple);
	Oid			databaseId;
	Oid			relationId;

	PrepareInvalidationState();

	relationId = HeapTupleGetOid(classTuple);
	if (classtup->relisshared)
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;
	RegisterRelcacheInvalidation(databaseId, relationId);
}

/*
 * CacheInvalidateRelcacheByRelid
 *		As above, but relation is identified by passing its OID.
 *		This is the least efficient of the three options; use one of
 *		the above routines if you have a Relation or pg_class tuple.
 */
void
CacheInvalidateRelcacheByRelid(Oid relid)
{
	HeapTuple	tup;

	PrepareInvalidationState();

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	CacheInvalidateRelcacheByTuple(tup);
	ReleaseSysCache(tup);
}


/*
 * CacheInvalidateSmgr
 *		Register invalidation of smgr references to a physical relation.
 *
 * Sending this type of invalidation msg forces other backends to close open
 * smgr entries for the rel.  This should be done to flush dangling open-file
 * references when the physical rel is being dropped or truncated.  Because
 * these are nontransactional (i.e., not-rollback-able) operations, we just
 * send the inval message immediately without any queuing.
 *
 * Note: in most cases there will have been a relcache flush issued against
 * the rel at the logical level.  We need a separate smgr-level flush because
 * it is possible for backends to have open smgr entries for rels they don't
 * have a relcache entry for, e.g. because the only thing they ever did with
 * the rel is write out dirty shared buffers.
 *
 * Note: because these messages are nontransactional, they won't be captured
 * in commit/abort WAL entries.  Instead, calls to CacheInvalidateSmgr()
 * should happen in low-level smgr.c routines, which are executed while
 * replaying WAL as well as when creating it.
 *
 * Note: In order to avoid bloating SharedInvalidationMessage, we store only
 * three bytes of the backend ID using what would otherwise be padding space.
 * Thus, the maximum possible backend ID is 2^23-1.
 */
void
CacheInvalidateSmgr(RelFileNodeBackend rnode)
{
	SharedInvalidationMessage msg;

	msg.sm.id = SHAREDINVALSMGR_ID;
	msg.sm.backend_hi = rnode.backend >> 16;
	msg.sm.backend_lo = rnode.backend & 0xffff;
	msg.sm.rnode = rnode.node;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	SendSharedInvalidMessages(&msg, 1);
}

/*
 * CacheInvalidateRelmap
 *		Register invalidation of the relation mapping for a database,
 *		or for the shared catalogs if databaseId is zero.
 *
 * Sending this type of invalidation msg forces other backends to re-read
 * the indicated relation mapping file.  It is also necessary to send a
 * relcache inval for the specific relations whose mapping has been altered,
 * else the relcache won't get updated with the new filenode data.
 *
 * Note: because these messages are nontransactional, they won't be captured
 * in commit/abort WAL entries.  Instead, calls to CacheInvalidateRelmap()
 * should happen in low-level relmapper.c routines, which are executed while
 * replaying WAL as well as when creating it.
 */
void
CacheInvalidateRelmap(Oid databaseId)
{
	SharedInvalidationMessage msg;

	msg.rm.id = SHAREDINVALRELMAP_ID;
	msg.rm.dbId = databaseId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	SendSharedInvalidMessages(&msg, 1);
}


/*
 * CacheRegisterSyscacheCallback
 *		Register the specified function to be called for all future
 *		invalidation events in the specified cache.  The cache ID and the
 *		hash value of the tuple being invalidated will be passed to the
 *		function.
 *
 * NOTE: Hash value zero will be passed if a cache reset request is received.
 * In this case the called routines should flush all cached state.
 * Yes, there's a possibility of a false match to zero, but it doesn't seem
 * worth troubling over, especially since most of the current callees just
 * flush all cached state anyway.
 */
void
CacheRegisterSyscacheCallback(int cacheid,
							  SyscacheCallbackFunction func,
							  Datum arg)
{
	if (syscache_callback_count >= MAX_SYSCACHE_CALLBACKS)
		elog(FATAL, "out of syscache_callback_list slots");

	syscache_callback_list[syscache_callback_count].id = cacheid;
	syscache_callback_list[syscache_callback_count].function = func;
	syscache_callback_list[syscache_callback_count].arg = arg;

	++syscache_callback_count;
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
CacheRegisterRelcacheCallback(RelcacheCallbackFunction func,
							  Datum arg)
{
	if (relcache_callback_count >= MAX_RELCACHE_CALLBACKS)
		elog(FATAL, "out of relcache_callback_list slots");

	relcache_callback_list[relcache_callback_count].function = func;
	relcache_callback_list[relcache_callback_count].arg = arg;

	++relcache_callback_count;
}

/*
 * CallSyscacheCallbacks
 *
 * This is exported so that CatalogCacheFlushCatalog can call it, saving
 * this module from knowing which catcache IDs correspond to which catalogs.
 */
void
CallSyscacheCallbacks(int cacheid, uint32 hashvalue)
{
	int			i;

	for (i = 0; i < syscache_callback_count; i++)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		if (ccitem->id == cacheid)
			(*ccitem->function) (ccitem->arg, cacheid, hashvalue);
	}
}
