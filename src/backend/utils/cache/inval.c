/*-------------------------------------------------------------------------
 *
 * inval.c
 *	  POSTGRES cache invalidation dispatcher code.
 *
 *	This is subtle stuff, so pay attention:
 *
 *	When a tuple is updated or deleted, our standard visibility rules
 *	consider that it is *still valid* so long as we are in the same command,
 *	ie, until the next CommandCounterIncrement() or transaction commit.
 *	(See access/heap/heapam_visibility.c, and note that system catalogs are
 *  generally scanned under the most current snapshot available, rather than
 *  the transaction snapshot.)	At the command boundary, the old tuple stops
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
 *	Also, whenever we see an operation on a pg_class, pg_attribute, or
 *	pg_index tuple, we register a relcache flush operation for the relation
 *	described by that tuple (as specified in CacheInvalidateHeapTuple()).
 *	Likewise for pg_constraint tuples for foreign keys on relations.
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
 *	Many subsystems own higher-level caches that depend on relcache and/or
 *	catcache, and they register callbacks here to invalidate their caches.
 *	While building a higher-level cache entry, a backend may receive a
 *	callback for the being-built entry or one of its dependencies.  This
 *	implies the new higher-level entry would be born stale, and it might
 *	remain stale for the life of the backend.  Many caches do not prevent
 *	that.  They rely on DDL for can't-miss catalog changes taking
 *	AccessExclusiveLock on suitable objects.  (For a change made with less
 *	locking, backends might never read the change.)  The relation cache,
 *	however, needs to reflect changes from CREATE INDEX CONCURRENTLY no later
 *	than the beginning of the next transaction.  Hence, when a relevant
 *	invalidation callback arrives during a build, relcache.c reattempts that
 *	build.  Caches with similar needs could do likewise.
 *
 *	If a relcache flush is issued for a system relation that we preload
 *	from the relcache init file, we must also delete the init file so that
 *	it will be rebuilt during the next backend restart.  The actual work of
 *	manipulating the init file is in relcache.c, but we keep track of the
 *	need for it here.
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
 *	When making a nontransactional change to a cacheable object, we must
 *	likewise send the invalidation immediately, before ending the change's
 *	critical section.  This includes inplace heap updates, relmap, and smgr.
 *
 *	When wal_level=logical, write invalidations into WAL at each command end to
 *	support the decoding of the in-progress transactions.  See
 *	CommandEndInvalidationMessages.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/inval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/pg_constraint.h"
#include "miscadmin.h"
#include "storage/procnumber.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/catcache.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * Pending requests are stored as ready-to-send SharedInvalidationMessages.
 * We keep the messages themselves in arrays in TopTransactionContext (there
 * are separate arrays for catcache and relcache messages).  For transactional
 * messages, control information is kept in a chain of TransInvalidationInfo
 * structs, also allocated in TopTransactionContext.  (We could keep a
 * subtransaction's TransInvalidationInfo in its CurTransactionContext; but
 * that's more wasteful not less so, since in very many scenarios it'd be the
 * only allocation in the subtransaction's CurTransactionContext.)  For
 * inplace update messages, control information appears in an
 * InvalidationInfo, allocated in CurrentMemoryContext.
 *
 * We can store the message arrays densely, and yet avoid moving data around
 * within an array, because within any one subtransaction we need only
 * distinguish between messages emitted by prior commands and those emitted
 * by the current command.  Once a command completes and we've done local
 * processing on its messages, we can fold those into the prior-commands
 * messages just by changing array indexes in the TransInvalidationInfo
 * struct.  Similarly, we need distinguish messages of prior subtransactions
 * from those of the current subtransaction only until the subtransaction
 * completes, after which we adjust the array indexes in the parent's
 * TransInvalidationInfo to include the subtransaction's messages.  Inplace
 * invalidations don't need a concept of command or subtransaction boundaries,
 * since we send them during the WAL insertion critical section.
 *
 * The ordering of the individual messages within a command's or
 * subtransaction's output is not considered significant, although this
 * implementation happens to preserve the order in which they were queued.
 * (Previous versions of this code did not preserve it.)
 *
 * For notational convenience, control information is kept in two-element
 * arrays, the first for catcache messages and the second for relcache
 * messages.
 */
#define CatCacheMsgs 0
#define RelCacheMsgs 1

/* Pointers to main arrays in TopTransactionContext */
typedef struct InvalMessageArray
{
	SharedInvalidationMessage *msgs;	/* palloc'd array (can be expanded) */
	int			maxmsgs;		/* current allocated size of array */
} InvalMessageArray;

static InvalMessageArray InvalMessageArrays[2];

/* Control information for one logical group of messages */
typedef struct InvalidationMsgsGroup
{
	int			firstmsg[2];	/* first index in relevant array */
	int			nextmsg[2];		/* last+1 index */
} InvalidationMsgsGroup;

/* Macros to help preserve InvalidationMsgsGroup abstraction */
#define SetSubGroupToFollow(targetgroup, priorgroup, subgroup) \
	do { \
		(targetgroup)->firstmsg[subgroup] = \
			(targetgroup)->nextmsg[subgroup] = \
			(priorgroup)->nextmsg[subgroup]; \
	} while (0)

#define SetGroupToFollow(targetgroup, priorgroup) \
	do { \
		SetSubGroupToFollow(targetgroup, priorgroup, CatCacheMsgs); \
		SetSubGroupToFollow(targetgroup, priorgroup, RelCacheMsgs); \
	} while (0)

#define NumMessagesInSubGroup(group, subgroup) \
	((group)->nextmsg[subgroup] - (group)->firstmsg[subgroup])

#define NumMessagesInGroup(group) \
	(NumMessagesInSubGroup(group, CatCacheMsgs) + \
	 NumMessagesInSubGroup(group, RelCacheMsgs))


/*----------------
 * Transactional invalidation messages are divided into two groups:
 *	1) events so far in current command, not yet reflected to caches.
 *	2) events in previous commands of current transaction; these have
 *	   been reflected to local caches, and must be either broadcast to
 *	   other backends or rolled back from local cache when we commit
 *	   or abort the transaction.
 * Actually, we need such groups for each level of nested transaction,
 * so that we can discard events from an aborted subtransaction.  When
 * a subtransaction commits, we append its events to the parent's groups.
 *
 * The relcache-file-invalidated flag can just be a simple boolean,
 * since we only act on it at transaction commit; we don't care which
 * command of the transaction set it.
 *----------------
 */

/* fields common to both transactional and inplace invalidation */
typedef struct InvalidationInfo
{
	/* Events emitted by current command */
	InvalidationMsgsGroup CurrentCmdInvalidMsgs;

	/* init file must be invalidated? */
	bool		RelcacheInitFileInval;
} InvalidationInfo;

/* subclass adding fields specific to transactional invalidation */
typedef struct TransInvalidationInfo
{
	/* Base class */
	struct InvalidationInfo ii;

	/* Events emitted by previous commands of this (sub)transaction */
	InvalidationMsgsGroup PriorCmdInvalidMsgs;

	/* Back link to parent transaction's info */
	struct TransInvalidationInfo *parent;

	/* Subtransaction nesting depth */
	int			my_level;
} TransInvalidationInfo;

static TransInvalidationInfo *transInvalInfo = NULL;

static InvalidationInfo *inplaceInvalInfo = NULL;

/* GUC storage */
int			debug_discard_caches = 0;

/*
 * Dynamically-registered callback functions.  Current implementation
 * assumes there won't be enough of these to justify a dynamically resizable
 * array; it'd be easy to improve that if needed.
 *
 * To avoid searching in CallSyscacheCallbacks, all callbacks for a given
 * syscache are linked into a list pointed to by syscache_callback_links[id].
 * The link values are syscache_callback_list[] index plus 1, or 0 for none.
 */

#define MAX_SYSCACHE_CALLBACKS 64
#define MAX_RELCACHE_CALLBACKS 10
#define MAX_RELSYNC_CALLBACKS 10

static struct SYSCACHECALLBACK
{
	int16		id;				/* cache number */
	int16		link;			/* next callback index+1 for same cache */
	SyscacheCallbackFunction function;
	Datum		arg;
}			syscache_callback_list[MAX_SYSCACHE_CALLBACKS];

static int16 syscache_callback_links[SysCacheSize];

static int	syscache_callback_count = 0;

static struct RELCACHECALLBACK
{
	RelcacheCallbackFunction function;
	Datum		arg;
}			relcache_callback_list[MAX_RELCACHE_CALLBACKS];

static int	relcache_callback_count = 0;

static struct RELSYNCCALLBACK
{
	RelSyncCallbackFunction function;
	Datum		arg;
}			relsync_callback_list[MAX_RELSYNC_CALLBACKS];

static int	relsync_callback_count = 0;


/* ----------------------------------------------------------------
 *				Invalidation subgroup support functions
 * ----------------------------------------------------------------
 */

/*
 * AddInvalidationMessage
 *		Add an invalidation message to a (sub)group.
 *
 * The group must be the last active one, since we assume we can add to the
 * end of the relevant InvalMessageArray.
 *
 * subgroup must be CatCacheMsgs or RelCacheMsgs.
 */
static void
AddInvalidationMessage(InvalidationMsgsGroup *group, int subgroup,
					   const SharedInvalidationMessage *msg)
{
	InvalMessageArray *ima = &InvalMessageArrays[subgroup];
	int			nextindex = group->nextmsg[subgroup];

	if (nextindex >= ima->maxmsgs)
	{
		if (ima->msgs == NULL)
		{
			/* Create new storage array in TopTransactionContext */
			int			reqsize = 32;	/* arbitrary */

			ima->msgs = (SharedInvalidationMessage *)
				MemoryContextAlloc(TopTransactionContext,
								   reqsize * sizeof(SharedInvalidationMessage));
			ima->maxmsgs = reqsize;
			Assert(nextindex == 0);
		}
		else
		{
			/* Enlarge storage array */
			int			reqsize = 2 * ima->maxmsgs;

			ima->msgs = (SharedInvalidationMessage *)
				repalloc(ima->msgs,
						 reqsize * sizeof(SharedInvalidationMessage));
			ima->maxmsgs = reqsize;
		}
	}
	/* Okay, add message to current group */
	ima->msgs[nextindex] = *msg;
	group->nextmsg[subgroup]++;
}

/*
 * Append one subgroup of invalidation messages to another, resetting
 * the source subgroup to empty.
 */
static void
AppendInvalidationMessageSubGroup(InvalidationMsgsGroup *dest,
								  InvalidationMsgsGroup *src,
								  int subgroup)
{
	/* Messages must be adjacent in main array */
	Assert(dest->nextmsg[subgroup] == src->firstmsg[subgroup]);

	/* ... which makes this easy: */
	dest->nextmsg[subgroup] = src->nextmsg[subgroup];

	/*
	 * This is handy for some callers and irrelevant for others.  But we do it
	 * always, reasoning that it's bad to leave different groups pointing at
	 * the same fragment of the message array.
	 */
	SetSubGroupToFollow(src, dest, subgroup);
}

/*
 * Process a subgroup of invalidation messages.
 *
 * This is a macro that executes the given code fragment for each message in
 * a message subgroup.  The fragment should refer to the message as *msg.
 */
#define ProcessMessageSubGroup(group, subgroup, codeFragment) \
	do { \
		int		_msgindex = (group)->firstmsg[subgroup]; \
		int		_endmsg = (group)->nextmsg[subgroup]; \
		for (; _msgindex < _endmsg; _msgindex++) \
		{ \
			SharedInvalidationMessage *msg = \
				&InvalMessageArrays[subgroup].msgs[_msgindex]; \
			codeFragment; \
		} \
	} while (0)

/*
 * Process a subgroup of invalidation messages as an array.
 *
 * As above, but the code fragment can handle an array of messages.
 * The fragment should refer to the messages as msgs[], with n entries.
 */
#define ProcessMessageSubGroupMulti(group, subgroup, codeFragment) \
	do { \
		int		n = NumMessagesInSubGroup(group, subgroup); \
		if (n > 0) { \
			SharedInvalidationMessage *msgs = \
				&InvalMessageArrays[subgroup].msgs[(group)->firstmsg[subgroup]]; \
			codeFragment; \
		} \
	} while (0)


/* ----------------------------------------------------------------
 *				Invalidation group support functions
 *
 * These routines understand about the division of a logical invalidation
 * group into separate physical arrays for catcache and relcache entries.
 * ----------------------------------------------------------------
 */

/*
 * Add a catcache inval entry
 */
static void
AddCatcacheInvalidationMessage(InvalidationMsgsGroup *group,
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

	AddInvalidationMessage(group, CatCacheMsgs, &msg);
}

/*
 * Add a whole-catalog inval entry
 */
static void
AddCatalogInvalidationMessage(InvalidationMsgsGroup *group,
							  Oid dbId, Oid catId)
{
	SharedInvalidationMessage msg;

	msg.cat.id = SHAREDINVALCATALOG_ID;
	msg.cat.dbId = dbId;
	msg.cat.catId = catId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, CatCacheMsgs, &msg);
}

/*
 * Add a relcache inval entry
 */
static void
AddRelcacheInvalidationMessage(InvalidationMsgsGroup *group,
							   Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/*
	 * Don't add a duplicate item. We assume dbId need not be checked because
	 * it will never change. InvalidOid for relId means all relations so we
	 * don't need to add individual ones when it is present.
	 */
	ProcessMessageSubGroup(group, RelCacheMsgs,
						   if (msg->rc.id == SHAREDINVALRELCACHE_ID &&
							   (msg->rc.relId == relId ||
								msg->rc.relId == InvalidOid))
						   return);

	/* OK, add the item */
	msg.rc.id = SHAREDINVALRELCACHE_ID;
	msg.rc.dbId = dbId;
	msg.rc.relId = relId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, RelCacheMsgs, &msg);
}

/*
 * Add a relsync inval entry
 *
 * We put these into the relcache subgroup for simplicity. This message is the
 * same as AddRelcacheInvalidationMessage() except that it is for
 * RelationSyncCache maintained by decoding plugin pgoutput.
 */
static void
AddRelsyncInvalidationMessage(InvalidationMsgsGroup *group,
							  Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/* Don't add a duplicate item. */
	ProcessMessageSubGroup(group, RelCacheMsgs,
						   if (msg->rc.id == SHAREDINVALRELSYNC_ID &&
							   (msg->rc.relId == relId ||
								msg->rc.relId == InvalidOid))
						   return);

	/* OK, add the item */
	msg.rc.id = SHAREDINVALRELSYNC_ID;
	msg.rc.dbId = dbId;
	msg.rc.relId = relId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, RelCacheMsgs, &msg);
}

/*
 * Add a snapshot inval entry
 *
 * We put these into the relcache subgroup for simplicity.
 */
static void
AddSnapshotInvalidationMessage(InvalidationMsgsGroup *group,
							   Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/* Don't add a duplicate item */
	/* We assume dbId need not be checked because it will never change */
	ProcessMessageSubGroup(group, RelCacheMsgs,
						   if (msg->sn.id == SHAREDINVALSNAPSHOT_ID &&
							   msg->sn.relId == relId)
						   return);

	/* OK, add the item */
	msg.sn.id = SHAREDINVALSNAPSHOT_ID;
	msg.sn.dbId = dbId;
	msg.sn.relId = relId;
	/* check AddCatcacheInvalidationMessage() for an explanation */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, RelCacheMsgs, &msg);
}

/*
 * Append one group of invalidation messages to another, resetting
 * the source group to empty.
 */
static void
AppendInvalidationMessages(InvalidationMsgsGroup *dest,
						   InvalidationMsgsGroup *src)
{
	AppendInvalidationMessageSubGroup(dest, src, CatCacheMsgs);
	AppendInvalidationMessageSubGroup(dest, src, RelCacheMsgs);
}

/*
 * Execute the given function for all the messages in an invalidation group.
 * The group is not altered.
 *
 * catcache entries are processed first, for reasons mentioned above.
 */
static void
ProcessInvalidationMessages(InvalidationMsgsGroup *group,
							void (*func) (SharedInvalidationMessage *msg))
{
	ProcessMessageSubGroup(group, CatCacheMsgs, func(msg));
	ProcessMessageSubGroup(group, RelCacheMsgs, func(msg));
}

/*
 * As above, but the function is able to process an array of messages
 * rather than just one at a time.
 */
static void
ProcessInvalidationMessagesMulti(InvalidationMsgsGroup *group,
								 void (*func) (const SharedInvalidationMessage *msgs, int n))
{
	ProcessMessageSubGroupMulti(group, CatCacheMsgs, func(msgs, n));
	ProcessMessageSubGroupMulti(group, RelCacheMsgs, func(msgs, n));
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
							 Oid dbId,
							 void *context)
{
	InvalidationInfo *info = (InvalidationInfo *) context;

	AddCatcacheInvalidationMessage(&info->CurrentCmdInvalidMsgs,
								   cacheId, hashValue, dbId);
}

/*
 * RegisterCatalogInvalidation
 *
 * Register an invalidation event for all catcache entries from a catalog.
 */
static void
RegisterCatalogInvalidation(InvalidationInfo *info, Oid dbId, Oid catId)
{
	AddCatalogInvalidationMessage(&info->CurrentCmdInvalidMsgs, dbId, catId);
}

/*
 * RegisterRelcacheInvalidation
 *
 * As above, but register a relcache invalidation event.
 */
static void
RegisterRelcacheInvalidation(InvalidationInfo *info, Oid dbId, Oid relId)
{
	AddRelcacheInvalidationMessage(&info->CurrentCmdInvalidMsgs, dbId, relId);

	/*
	 * Most of the time, relcache invalidation is associated with system
	 * catalog updates, but there are a few cases where it isn't.  Quick hack
	 * to ensure that the next CommandCounterIncrement() will think that we
	 * need to do CommandEndInvalidationMessages().
	 */
	(void) GetCurrentCommandId(true);

	/*
	 * If the relation being invalidated is one of those cached in a relcache
	 * init file, mark that we need to zap that file at commit. For simplicity
	 * invalidations for a specific database always invalidate the shared file
	 * as well.  Also zap when we are invalidating whole relcache.
	 */
	if (relId == InvalidOid || RelationIdIsInInitFile(relId))
		info->RelcacheInitFileInval = true;
}

/*
 * RegisterRelsyncInvalidation
 *
 * As above, but register a relsynccache invalidation event.
 */
static void
RegisterRelsyncInvalidation(InvalidationInfo *info, Oid dbId, Oid relId)
{
	AddRelsyncInvalidationMessage(&info->CurrentCmdInvalidMsgs, dbId, relId);
}

/*
 * RegisterSnapshotInvalidation
 *
 * Register an invalidation event for MVCC scans against a given catalog.
 * Only needed for catalogs that don't have catcaches.
 */
static void
RegisterSnapshotInvalidation(InvalidationInfo *info, Oid dbId, Oid relId)
{
	AddSnapshotInvalidationMessage(&info->CurrentCmdInvalidMsgs, dbId, relId);
}

/*
 * PrepareInvalidationState
 *		Initialize inval data for the current (sub)transaction.
 */
static InvalidationInfo *
PrepareInvalidationState(void)
{
	TransInvalidationInfo *myInfo;

	/* PrepareToInvalidateCacheTuple() needs relcache */
	AssertCouldGetRelation();
	/* Can't queue transactional message while collecting inplace messages. */
	Assert(inplaceInvalInfo == NULL);

	if (transInvalInfo != NULL &&
		transInvalInfo->my_level == GetCurrentTransactionNestLevel())
		return (InvalidationInfo *) transInvalInfo;

	myInfo = (TransInvalidationInfo *)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(TransInvalidationInfo));
	myInfo->parent = transInvalInfo;
	myInfo->my_level = GetCurrentTransactionNestLevel();

	/* Now, do we have a previous stack entry? */
	if (transInvalInfo != NULL)
	{
		/* Yes; this one should be for a deeper nesting level. */
		Assert(myInfo->my_level > transInvalInfo->my_level);

		/*
		 * The parent (sub)transaction must not have any current (i.e.,
		 * not-yet-locally-processed) messages.  If it did, we'd have a
		 * semantic problem: the new subtransaction presumably ought not be
		 * able to see those events yet, but since the CommandCounter is
		 * linear, that can't work once the subtransaction advances the
		 * counter.  This is a convenient place to check for that, as well as
		 * being important to keep management of the message arrays simple.
		 */
		if (NumMessagesInGroup(&transInvalInfo->ii.CurrentCmdInvalidMsgs) != 0)
			elog(ERROR, "cannot start a subtransaction when there are unprocessed inval messages");

		/*
		 * MemoryContextAllocZero set firstmsg = nextmsg = 0 in each group,
		 * which is fine for the first (sub)transaction, but otherwise we need
		 * to update them to follow whatever is already in the arrays.
		 */
		SetGroupToFollow(&myInfo->PriorCmdInvalidMsgs,
						 &transInvalInfo->ii.CurrentCmdInvalidMsgs);
		SetGroupToFollow(&myInfo->ii.CurrentCmdInvalidMsgs,
						 &myInfo->PriorCmdInvalidMsgs);
	}
	else
	{
		/*
		 * Here, we need only clear any array pointers left over from a prior
		 * transaction.
		 */
		InvalMessageArrays[CatCacheMsgs].msgs = NULL;
		InvalMessageArrays[CatCacheMsgs].maxmsgs = 0;
		InvalMessageArrays[RelCacheMsgs].msgs = NULL;
		InvalMessageArrays[RelCacheMsgs].maxmsgs = 0;
	}

	transInvalInfo = myInfo;
	return (InvalidationInfo *) myInfo;
}

/*
 * PrepareInplaceInvalidationState
 *		Initialize inval data for an inplace update.
 *
 * See previous function for more background.
 */
static InvalidationInfo *
PrepareInplaceInvalidationState(void)
{
	InvalidationInfo *myInfo;

	AssertCouldGetRelation();
	/* limit of one inplace update under assembly */
	Assert(inplaceInvalInfo == NULL);

	/* gone after WAL insertion CritSection ends, so use current context */
	myInfo = (InvalidationInfo *) palloc0(sizeof(InvalidationInfo));

	/* Stash our messages past end of the transactional messages, if any. */
	if (transInvalInfo != NULL)
		SetGroupToFollow(&myInfo->CurrentCmdInvalidMsgs,
						 &transInvalInfo->ii.CurrentCmdInvalidMsgs);
	else
	{
		InvalMessageArrays[CatCacheMsgs].msgs = NULL;
		InvalMessageArrays[CatCacheMsgs].maxmsgs = 0;
		InvalMessageArrays[RelCacheMsgs].msgs = NULL;
		InvalMessageArrays[RelCacheMsgs].maxmsgs = 0;
	}

	inplaceInvalInfo = myInfo;
	return myInfo;
}

/* ----------------------------------------------------------------
 *					  public functions
 * ----------------------------------------------------------------
 */

void
InvalidateSystemCachesExtended(bool debug_discard)
{
	int			i;

	InvalidateCatalogSnapshot();
	ResetCatalogCachesExt(debug_discard);
	RelationCacheInvalidate(debug_discard); /* gets smgr and relmap too */

	for (i = 0; i < syscache_callback_count; i++)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		ccitem->function(ccitem->arg, ccitem->id, 0);
	}

	for (i = 0; i < relcache_callback_count; i++)
	{
		struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

		ccitem->function(ccitem->arg, InvalidOid);
	}

	for (i = 0; i < relsync_callback_count; i++)
	{
		struct RELSYNCCALLBACK *ccitem = relsync_callback_list + i;

		ccitem->function(ccitem->arg, InvalidOid);
	}
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

			SysCacheInvalidate(msg->cc.id, msg->cc.hashValue);

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

			if (msg->rc.relId == InvalidOid)
				RelationCacheInvalidate(false);
			else
				RelationCacheInvalidateEntry(msg->rc.relId);

			for (i = 0; i < relcache_callback_count; i++)
			{
				struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

				ccitem->function(ccitem->arg, msg->rc.relId);
			}
		}
	}
	else if (msg->id == SHAREDINVALSMGR_ID)
	{
		/*
		 * We could have smgr entries for relations of other databases, so no
		 * short-circuit test is possible here.
		 */
		RelFileLocatorBackend rlocator;

		rlocator.locator = msg->sm.rlocator;
		rlocator.backend = (msg->sm.backend_hi << 16) | (int) msg->sm.backend_lo;
		smgrreleaserellocator(rlocator);
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
		if (msg->sn.dbId == InvalidOid)
			InvalidateCatalogSnapshot();
		else if (msg->sn.dbId == MyDatabaseId)
			InvalidateCatalogSnapshot();
	}
	else if (msg->id == SHAREDINVALRELSYNC_ID)
	{
		/* We only care about our own database */
		if (msg->rs.dbId == MyDatabaseId)
			CallRelSyncCallbacks(msg->rs.relid);
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
	InvalidateSystemCachesExtended(false);
}

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
#ifdef USE_ASSERT_CHECKING
	/* message handlers shall access catalogs only during transactions */
	if (IsTransactionState())
		AssertCouldGetRelation();
#endif

	ReceiveSharedInvalidMessages(LocalExecuteInvalidationMessage,
								 InvalidateSystemCaches);

	/*----------
	 * Test code to force cache flushes anytime a flush could happen.
	 *
	 * This helps detect intermittent faults caused by code that reads a cache
	 * entry and then performs an action that could invalidate the entry, but
	 * rarely actually does so.  This can spot issues that would otherwise
	 * only arise with badly timed concurrent DDL, for example.
	 *
	 * The default debug_discard_caches = 0 does no forced cache flushes.
	 *
	 * If used with CLOBBER_FREED_MEMORY,
	 * debug_discard_caches = 1 (formerly known as CLOBBER_CACHE_ALWAYS)
	 * provides a fairly thorough test that the system contains no cache-flush
	 * hazards.  However, it also makes the system unbelievably slow --- the
	 * regression tests take about 100 times longer than normal.
	 *
	 * If you're a glutton for punishment, try
	 * debug_discard_caches = 3 (formerly known as CLOBBER_CACHE_RECURSIVELY).
	 * This slows things by at least a factor of 10000, so I wouldn't suggest
	 * trying to run the entire regression tests that way.  It's useful to try
	 * a few simple tests, to make sure that cache reload isn't subject to
	 * internal cache-flush hazards, but after you've done a few thousand
	 * recursive reloads it's unlikely you'll learn more.
	 *----------
	 */
#ifdef DISCARD_CACHES_ENABLED
	{
		static int	recursion_depth = 0;

		if (recursion_depth < debug_discard_caches)
		{
			recursion_depth++;
			InvalidateSystemCachesExtended(true);
			recursion_depth--;
		}
	}
#endif
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
 * xactGetCommittedInvalidationMessages() is called by
 * RecordTransactionCommit() to collect invalidation messages to add to the
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
	SharedInvalidationMessage *msgarray;
	int			nummsgs;
	int			nmsgs;

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
	*RelcacheInitFileInval = transInvalInfo->ii.RelcacheInitFileInval;

	/*
	 * Collect all the pending messages into a single contiguous array of
	 * invalidation messages, to simplify what needs to happen while building
	 * the commit WAL message.  Maintain the order that they would be
	 * processed in by AtEOXact_Inval(), to ensure emulated behaviour in redo
	 * is as similar as possible to original.  We want the same bugs, if any,
	 * not new ones.
	 */
	nummsgs = NumMessagesInGroup(&transInvalInfo->PriorCmdInvalidMsgs) +
		NumMessagesInGroup(&transInvalInfo->ii.CurrentCmdInvalidMsgs);

	*msgs = msgarray = (SharedInvalidationMessage *)
		MemoryContextAlloc(CurTransactionContext,
						   nummsgs * sizeof(SharedInvalidationMessage));

	nmsgs = 0;
	ProcessMessageSubGroupMulti(&transInvalInfo->PriorCmdInvalidMsgs,
								CatCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&transInvalInfo->ii.CurrentCmdInvalidMsgs,
								CatCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&transInvalInfo->PriorCmdInvalidMsgs,
								RelCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&transInvalInfo->ii.CurrentCmdInvalidMsgs,
								RelCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	Assert(nmsgs == nummsgs);

	return nmsgs;
}

/*
 * inplaceGetInvalidationMessages() is called by the inplace update to collect
 * invalidation messages to add to its WAL record.  Like the previous
 * function, we might still fail.
 */
int
inplaceGetInvalidationMessages(SharedInvalidationMessage **msgs,
							   bool *RelcacheInitFileInval)
{
	SharedInvalidationMessage *msgarray;
	int			nummsgs;
	int			nmsgs;

	/* Quick exit if we haven't done anything with invalidation messages. */
	if (inplaceInvalInfo == NULL)
	{
		*RelcacheInitFileInval = false;
		*msgs = NULL;
		return 0;
	}

	*RelcacheInitFileInval = inplaceInvalInfo->RelcacheInitFileInval;
	nummsgs = NumMessagesInGroup(&inplaceInvalInfo->CurrentCmdInvalidMsgs);
	*msgs = msgarray = (SharedInvalidationMessage *)
		palloc(nummsgs * sizeof(SharedInvalidationMessage));

	nmsgs = 0;
	ProcessMessageSubGroupMulti(&inplaceInvalInfo->CurrentCmdInvalidMsgs,
								CatCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&inplaceInvalInfo->CurrentCmdInvalidMsgs,
								RelCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	Assert(nmsgs == nummsgs);

	return nmsgs;
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

	elog(DEBUG4, "replaying commit with %d messages%s", nmsgs,
		 (RelcacheInitFileInval ? " and relcache file invalidation" : ""));

	if (RelcacheInitFileInval)
	{
		elog(DEBUG4, "removing relcache init files for database %u", dbid);

		/*
		 * RelationCacheInitFilePreInvalidate, when the invalidation message
		 * is for a specific database, requires DatabasePath to be set, but we
		 * should not use SetDatabasePath during recovery, since it is
		 * intended to be used only once by normal backends.  Hence, a quick
		 * hack: set DatabasePath directly then unset after use.
		 */
		if (OidIsValid(dbid))
			DatabasePath = GetDatabasePath(dbid, tsid);

		RelationCacheInitFilePreInvalidate();

		if (OidIsValid(dbid))
		{
			pfree(DatabasePath);
			DatabasePath = NULL;
		}
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
 * In any case, reset our state to empty.  We need not physically
 * free memory here, since TopTransactionContext is about to be emptied
 * anyway.
 *
 * Note:
 *		This should be called as the last step in processing a transaction.
 */
void
AtEOXact_Inval(bool isCommit)
{
	inplaceInvalInfo = NULL;

	/* Quick exit if no transactional messages */
	if (transInvalInfo == NULL)
		return;

	/* Must be at top of stack */
	Assert(transInvalInfo->my_level == 1 && transInvalInfo->parent == NULL);

	INJECTION_POINT("transaction-end-process-inval");

	if (isCommit)
	{
		/*
		 * Relcache init file invalidation requires processing both before and
		 * after we send the SI messages.  However, we need not do anything
		 * unless we committed.
		 */
		if (transInvalInfo->ii.RelcacheInitFileInval)
			RelationCacheInitFilePreInvalidate();

		AppendInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
								   &transInvalInfo->ii.CurrentCmdInvalidMsgs);

		ProcessInvalidationMessagesMulti(&transInvalInfo->PriorCmdInvalidMsgs,
										 SendSharedInvalidMessages);

		if (transInvalInfo->ii.RelcacheInitFileInval)
			RelationCacheInitFilePostInvalidate();
	}
	else
	{
		ProcessInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);
	}

	/* Need not free anything explicitly */
	transInvalInfo = NULL;
}

/*
 * PreInplace_Inval
 *		Process queued-up invalidation before inplace update critical section.
 *
 * Tasks belong here if they are safe even if the inplace update does not
 * complete.  Currently, this just unlinks a cache file, which can fail.  The
 * sum of this and AtInplace_Inval() mirrors AtEOXact_Inval(isCommit=true).
 */
void
PreInplace_Inval(void)
{
	Assert(CritSectionCount == 0);

	if (inplaceInvalInfo && inplaceInvalInfo->RelcacheInitFileInval)
		RelationCacheInitFilePreInvalidate();
}

/*
 * AtInplace_Inval
 *		Process queued-up invalidations after inplace update buffer mutation.
 */
void
AtInplace_Inval(void)
{
	Assert(CritSectionCount > 0);

	if (inplaceInvalInfo == NULL)
		return;

	ProcessInvalidationMessagesMulti(&inplaceInvalInfo->CurrentCmdInvalidMsgs,
									 SendSharedInvalidMessages);

	if (inplaceInvalInfo->RelcacheInitFileInval)
		RelationCacheInitFilePostInvalidate();

	inplaceInvalInfo = NULL;
}

/*
 * ForgetInplace_Inval
 *		Alternative to PreInplace_Inval()+AtInplace_Inval(): discard queued-up
 *		invalidations.  This lets inplace update enumerate invalidations
 *		optimistically, before locking the buffer.
 */
void
ForgetInplace_Inval(void)
{
	inplaceInvalInfo = NULL;
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
	TransInvalidationInfo *myInfo;

	/*
	 * Successful inplace update must clear this, but we clear it on abort.
	 * Inplace updates allocate this in CurrentMemoryContext, which has
	 * lifespan <= subtransaction lifespan.  Hence, don't free it explicitly.
	 */
	if (isCommit)
		Assert(inplaceInvalInfo == NULL);
	else
		inplaceInvalInfo = NULL;

	/* Quick exit if no transactional messages. */
	myInfo = transInvalInfo;
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

		/*
		 * Pass up my inval messages to parent.  Notice that we stick them in
		 * PriorCmdInvalidMsgs, not CurrentCmdInvalidMsgs, since they've
		 * already been locally processed.  (This would trigger the Assert in
		 * AppendInvalidationMessageSubGroup if the parent's
		 * CurrentCmdInvalidMsgs isn't empty; but we already checked that in
		 * PrepareInvalidationState.)
		 */
		AppendInvalidationMessages(&myInfo->parent->PriorCmdInvalidMsgs,
								   &myInfo->PriorCmdInvalidMsgs);

		/* Must readjust parent's CurrentCmdInvalidMsgs indexes now */
		SetGroupToFollow(&myInfo->parent->ii.CurrentCmdInvalidMsgs,
						 &myInfo->parent->PriorCmdInvalidMsgs);

		/* Pending relcache inval becomes parent's problem too */
		if (myInfo->ii.RelcacheInitFileInval)
			myInfo->parent->ii.RelcacheInitFileInval = true;

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

	ProcessInvalidationMessages(&transInvalInfo->ii.CurrentCmdInvalidMsgs,
								LocalExecuteInvalidationMessage);

	/* WAL Log per-command invalidation messages for wal_level=logical */
	if (XLogLogicalInfoActive())
		LogLogicalInvalidations();

	AppendInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
							   &transInvalInfo->ii.CurrentCmdInvalidMsgs);
}


/*
 * CacheInvalidateHeapTupleCommon
 *		Common logic for end-of-command and inplace variants.
 */
static void
CacheInvalidateHeapTupleCommon(Relation relation,
							   HeapTuple tuple,
							   HeapTuple newtuple,
							   InvalidationInfo *(*prepare_callback) (void))
{
	InvalidationInfo *info;
	Oid			tupleRelId;
	Oid			databaseId;
	Oid			relationId;

	/* PrepareToInvalidateCacheTuple() needs relcache */
	AssertCouldGetRelation();

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

	/* Allocate any required resources. */
	info = prepare_callback();

	/*
	 * First let the catcache do its thing
	 */
	tupleRelId = RelationGetRelid(relation);
	if (RelationInvalidatesSnapshotsOnly(tupleRelId))
	{
		databaseId = IsSharedRelation(tupleRelId) ? InvalidOid : MyDatabaseId;
		RegisterSnapshotInvalidation(info, databaseId, tupleRelId);
	}
	else
		PrepareToInvalidateCacheTuple(relation, tuple, newtuple,
									  RegisterCatcacheInvalidation,
									  (void *) info);

	/*
	 * Now, is this tuple one of the primary definers of a relcache entry? See
	 * comments in file header for deeper explanation.
	 *
	 * Note we ignore newtuple here; we assume an update cannot move a tuple
	 * from being part of one relcache entry to being part of another.
	 */
	if (tupleRelId == RelationRelationId)
	{
		Form_pg_class classtup = (Form_pg_class) GETSTRUCT(tuple);

		relationId = classtup->oid;
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
	else if (tupleRelId == ConstraintRelationId)
	{
		Form_pg_constraint constrtup = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * Foreign keys are part of relcache entries, too, so send out an
		 * inval for the table that the FK applies to.
		 */
		if (constrtup->contype == CONSTRAINT_FOREIGN &&
			OidIsValid(constrtup->conrelid))
		{
			relationId = constrtup->conrelid;
			databaseId = MyDatabaseId;
		}
		else
			return;
	}
	else
		return;

	/*
	 * Yes.  We need to register a relcache invalidation event.
	 */
	RegisterRelcacheInvalidation(info, databaseId, relationId);
}

/*
 * CacheInvalidateHeapTuple
 *		Register the given tuple for invalidation at end of command
 *		(ie, current command is creating or outdating this tuple) and end of
 *		transaction.  Also, detect whether a relcache invalidation is implied.
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
	CacheInvalidateHeapTupleCommon(relation, tuple, newtuple,
								   PrepareInvalidationState);
}

/*
 * CacheInvalidateHeapTupleInplace
 *		Register the given tuple for nontransactional invalidation pertaining
 *		to an inplace update.  Also, detect whether a relcache invalidation is
 *		implied.
 *
 * Like CacheInvalidateHeapTuple(), but for inplace updates.
 */
void
CacheInvalidateHeapTupleInplace(Relation relation,
								HeapTuple tuple,
								HeapTuple newtuple)
{
	CacheInvalidateHeapTupleCommon(relation, tuple, newtuple,
								   PrepareInplaceInvalidationState);
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

	if (IsSharedRelation(catalogId))
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;

	RegisterCatalogInvalidation(PrepareInvalidationState(),
								databaseId, catalogId);
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

	relationId = RelationGetRelid(relation);
	if (relation->rd_rel->relisshared)
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;

	RegisterRelcacheInvalidation(PrepareInvalidationState(),
								 databaseId, relationId);
}

/*
 * CacheInvalidateRelcacheAll
 *		Register invalidation of the whole relcache at the end of command.
 *
 * This is used by alter publication as changes in publications may affect
 * large number of tables.
 */
void
CacheInvalidateRelcacheAll(void)
{
	RegisterRelcacheInvalidation(PrepareInvalidationState(),
								 InvalidOid, InvalidOid);
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

	relationId = classtup->oid;
	if (classtup->relisshared)
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;
	RegisterRelcacheInvalidation(PrepareInvalidationState(),
								 databaseId, relationId);
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

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	CacheInvalidateRelcacheByTuple(tup);
	ReleaseSysCache(tup);
}

/*
 * CacheInvalidateRelSync
 *		Register invalidation of the cache in logical decoding output plugin
 *		for a database.
 *
 * This type of invalidation message is used for the specific purpose of output
 * plugins. Processes which do not decode WALs would do nothing even when it
 * receives the message.
 */
void
CacheInvalidateRelSync(Oid relid)
{
	RegisterRelsyncInvalidation(PrepareInvalidationState(),
								MyDatabaseId, relid);
}

/*
 * CacheInvalidateRelSyncAll
 *		Register invalidation of the whole cache in logical decoding output
 *		plugin.
 */
void
CacheInvalidateRelSyncAll(void)
{
	CacheInvalidateRelSync(InvalidOid);
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
 * three bytes of the ProcNumber using what would otherwise be padding space.
 * Thus, the maximum possible ProcNumber is 2^23-1.
 */
void
CacheInvalidateSmgr(RelFileLocatorBackend rlocator)
{
	SharedInvalidationMessage msg;

	/* verify optimization stated above stays valid */
	StaticAssertStmt(MAX_BACKENDS_BITS <= 23,
					 "MAX_BACKENDS_BITS is too big for inval.c");

	msg.sm.id = SHAREDINVALSMGR_ID;
	msg.sm.backend_hi = rlocator.backend >> 16;
	msg.sm.backend_lo = rlocator.backend & 0xffff;
	msg.sm.rlocator = rlocator.locator;
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
	if (cacheid < 0 || cacheid >= SysCacheSize)
		elog(FATAL, "invalid cache ID: %d", cacheid);
	if (syscache_callback_count >= MAX_SYSCACHE_CALLBACKS)
		elog(FATAL, "out of syscache_callback_list slots");

	if (syscache_callback_links[cacheid] == 0)
	{
		/* first callback for this cache */
		syscache_callback_links[cacheid] = syscache_callback_count + 1;
	}
	else
	{
		/* add to end of chain, so that older callbacks are called first */
		int			i = syscache_callback_links[cacheid] - 1;

		while (syscache_callback_list[i].link > 0)
			i = syscache_callback_list[i].link - 1;
		syscache_callback_list[i].link = syscache_callback_count + 1;
	}

	syscache_callback_list[syscache_callback_count].id = cacheid;
	syscache_callback_list[syscache_callback_count].link = 0;
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
 * CacheRegisterRelSyncCallback
 *		Register the specified function to be called for all future
 *		relsynccache invalidation events.
 *
 * This function is intended to be call from the logical decoding output
 * plugins.
 */
void
CacheRegisterRelSyncCallback(RelSyncCallbackFunction func,
							 Datum arg)
{
	if (relsync_callback_count >= MAX_RELSYNC_CALLBACKS)
		elog(FATAL, "out of relsync_callback_list slots");

	relsync_callback_list[relsync_callback_count].function = func;
	relsync_callback_list[relsync_callback_count].arg = arg;

	++relsync_callback_count;
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

	if (cacheid < 0 || cacheid >= SysCacheSize)
		elog(ERROR, "invalid cache ID: %d", cacheid);

	i = syscache_callback_links[cacheid] - 1;
	while (i >= 0)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		Assert(ccitem->id == cacheid);
		ccitem->function(ccitem->arg, cacheid, hashvalue);
		i = ccitem->link - 1;
	}
}

/*
 * CallSyscacheCallbacks
 */
void
CallRelSyncCallbacks(Oid relid)
{
	for (int i = 0; i < relsync_callback_count; i++)
	{
		struct RELSYNCCALLBACK *ccitem = relsync_callback_list + i;

		ccitem->function(ccitem->arg, relid);
	}
}

/*
 * LogLogicalInvalidations
 *
 * Emit WAL for invalidations caused by the current command.
 *
 * This is currently only used for logging invalidations at the command end
 * or at commit time if any invalidations are pending.
 */
void
LogLogicalInvalidations(void)
{
	xl_xact_invals xlrec;
	InvalidationMsgsGroup *group;
	int			nmsgs;

	/* Quick exit if we haven't done anything with invalidation messages. */
	if (transInvalInfo == NULL)
		return;

	group = &transInvalInfo->ii.CurrentCmdInvalidMsgs;
	nmsgs = NumMessagesInGroup(group);

	if (nmsgs > 0)
	{
		/* prepare record */
		memset(&xlrec, 0, MinSizeOfXactInvals);
		xlrec.nmsgs = nmsgs;

		/* perform insertion */
		XLogBeginInsert();
		XLogRegisterData(&xlrec, MinSizeOfXactInvals);
		ProcessMessageSubGroupMulti(group, CatCacheMsgs,
									XLogRegisterData(msgs,
													 n * sizeof(SharedInvalidationMessage)));
		ProcessMessageSubGroupMulti(group, RelCacheMsgs,
									XLogRegisterData(msgs,
													 n * sizeof(SharedInvalidationMessage)));
		XLogInsert(RM_XACT_ID, XLOG_XACT_INVALIDATIONS);
	}
}
