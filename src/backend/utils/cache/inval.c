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
 *	so that they can flush obsolete entries from their caches.
 *
 *	We do not need to register EVERY tuple operation in this way, just those
 *	on tuples in relations that have associated catcaches.  Also, whenever
 *	we see an operation on a pg_class or pg_attribute tuple, we register
 *	a relcache flush operation for the relation described by that tuple.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/inval.c,v 1.39 2001/01/05 22:54:37 tgl Exp $
 *
 * Note - this code is real crufty... badly needs a rewrite to improve
 * readability and portability.  (Shouldn't assume Oid == Index, for example)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/relcache.h"


/* ----------------
 *		private invalidation structures
 * ----------------
 */

typedef struct InvalidationUserData
{
	struct InvalidationUserData *dataP[1];		/* VARIABLE LENGTH */
} InvalidationUserData;			/* VARIABLE LENGTH STRUCTURE */

typedef struct InvalidationEntryData
{
	InvalidationUserData *nextP;
	InvalidationUserData userData;		/* VARIABLE LENGTH ARRAY */
} InvalidationEntryData;		/* VARIABLE LENGTH STRUCTURE */

typedef Pointer InvalidationEntry;

typedef InvalidationEntry LocalInvalid;

#define EmptyLocalInvalid		NULL

typedef struct CatalogInvalidationData
{
	Index		cacheId;
	Index		hashIndex;
	ItemPointerData pointerData;
} CatalogInvalidationData;

typedef struct RelationInvalidationData
{
	Oid			relationId;
	Oid			objectId;
} RelationInvalidationData;

typedef union AnyInvalidation
{
	CatalogInvalidationData catalog;
	RelationInvalidationData relation;
} AnyInvalidation;

typedef struct InvalidationMessageData
{
	char		kind;
	AnyInvalidation any;
} InvalidationMessageData;

typedef InvalidationMessageData *InvalidationMessage;

/* ----------------
 *		variables and macros
 * ----------------
 */

/*
 * ----------------
 *	Invalidation info is divided into three parts.
 *	1) shared invalidation to be registered for all backends
 *	2) local invalidation for the transaction itself (actually, just
 *	   for the current command within the transaction)
 *	3) rollback information for the transaction itself (in case we abort)
 * ----------------
 */

/*
 * head of invalidation linked list for all backends
 * eaten by AtCommit_Cache() in CommitTransaction()
 */
static LocalInvalid InvalidForall = EmptyLocalInvalid;

/*
 * head of invalidation linked list for the backend itself
 * eaten by AtCommit_LocalCache() in CommandCounterIncrement()
 */
static LocalInvalid InvalidLocal = EmptyLocalInvalid;

/*
 * head of rollback linked list for the backend itself
 * eaten by AtAbort_Cache() in AbortTransaction()
 */
static LocalInvalid RollbackStack = EmptyLocalInvalid;


static InvalidationEntry InvalidationEntryAllocate(uint16 size);
static void LocalInvalidInvalidate(LocalInvalid invalid,
								   void (*function) (InvalidationMessage),
								   bool freemember);
static LocalInvalid LocalInvalidRegister(LocalInvalid invalid,
					 InvalidationEntry entry);
static void DiscardInvalidStack(LocalInvalid *invalid);
static void InvalidationMessageRegisterSharedInvalid(InvalidationMessage message);


/* ----------------------------------------------------------------
 *				"local" invalidation support functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		InvalidationEntryAllocate
 *				Allocates an invalidation entry.
 * --------------------------------
 */
static InvalidationEntry
InvalidationEntryAllocate(uint16 size)
{
	InvalidationEntryData *entryDataP;

	entryDataP = (InvalidationEntryData *)
		malloc(sizeof(char *) + size);	/* XXX alignment */
	entryDataP->nextP = NULL;
	return (Pointer) &entryDataP->userData;
}

/* --------------------------------
 *		LocalInvalidRegister
 *		   Link an invalidation entry into a chain of them.  Really ugly
 *		   coding here.
 * --------------------------------
 */
static LocalInvalid
LocalInvalidRegister(LocalInvalid invalid,
					 InvalidationEntry entry)
{
	Assert(PointerIsValid(entry));

	((InvalidationUserData *) entry)->dataP[-1] =
		(InvalidationUserData *) invalid;

	return entry;
}

/* --------------------------------
 *		LocalInvalidInvalidate
 *				Processes, then frees all entries in a local cache
 *				invalidation list unless freemember parameter is false.
 * --------------------------------
 */
static void
LocalInvalidInvalidate(LocalInvalid invalid,
					   void (*function) (InvalidationMessage),
					   bool freemember)
{
	InvalidationEntryData *entryDataP;

	while (PointerIsValid(invalid))
	{
		entryDataP = (InvalidationEntryData *)
			&((InvalidationUserData *) invalid)->dataP[-1];

		if (PointerIsValid(function))
			(*function) ((InvalidationMessage) &entryDataP->userData);

		invalid = (Pointer) entryDataP->nextP;

		if (!freemember)
			continue;
		/* help catch errors */
		entryDataP->nextP = (InvalidationUserData *) NULL;

		free((Pointer) entryDataP);
	}
}

static void
DiscardInvalidStack(LocalInvalid *invalid)
{
	LocalInvalid locinv;

	locinv = *invalid;
	*invalid = EmptyLocalInvalid;
	if (locinv)
		LocalInvalidInvalidate(locinv,
							   (void (*) (InvalidationMessage)) NULL,
							   true);
}

/* ----------------------------------------------------------------
 *					  private support functions
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		CacheIdRegister.......
 *		RelationIdRegister....
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define CacheIdRegisterSpecifiedLocalInvalid_DEBUG1 \
elog(DEBUG, "CacheIdRegisterSpecifiedLocalInvalid(%d, %d, [%d, %d])", \
	 cacheId, hashIndex, ItemPointerGetBlockNumber(pointer), \
	 ItemPointerGetOffsetNumber(pointer))
#define CacheIdRegisterLocalInvalid_DEBUG1 \
elog(DEBUG, "CacheIdRegisterLocalInvalid(%d, %d, [%d, %d])", \
	 cacheId, hashIndex, ItemPointerGetBlockNumber(pointer), \
	 ItemPointerGetOffsetNumber(pointer))
#define CacheIdRegisterLocalRollback_DEBUG1 \
elog(DEBUG, "CacheIdRegisterLocalRollback(%d, %d, [%d, %d])", \
	 cacheId, hashIndex, ItemPointerGetBlockNumber(pointer), \
	 ItemPointerGetOffsetNumber(pointer))
#else
#define CacheIdRegisterSpecifiedLocalInvalid_DEBUG1
#define CacheIdRegisterLocalInvalid_DEBUG1
#define CacheIdRegisterLocalRollback_DEBUG1
#endif	 /* INVALIDDEBUG */

/* --------------------------------
 *		CacheIdRegisterSpecifiedLocalInvalid
 * --------------------------------
 */
static LocalInvalid
CacheIdRegisterSpecifiedLocalInvalid(LocalInvalid invalid,
									 Index cacheId,
									 Index hashIndex,
									 ItemPointer pointer)
{
	InvalidationMessage message;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	CacheIdRegisterSpecifiedLocalInvalid_DEBUG1;

	/* ----------------
	 *	create a message describing the system catalog tuple
	 *	we wish to invalidate.
	 * ----------------
	 */
	message = (InvalidationMessage)
		InvalidationEntryAllocate(sizeof(InvalidationMessageData));

	message->kind = 'c';
	message->any.catalog.cacheId = cacheId;
	message->any.catalog.hashIndex = hashIndex;

	ItemPointerCopy(pointer, &message->any.catalog.pointerData);

	/* ----------------
	 *	Add message to linked list of unprocessed messages.
	 * ----------------
	 */
	invalid = LocalInvalidRegister(invalid, (InvalidationEntry) message);
	return invalid;
}

/* --------------------------------
 *		CacheIdRegisterLocalInvalid
 * --------------------------------
 */
static void
CacheIdRegisterLocalInvalid(int cacheId,
							Index hashIndex,
							ItemPointer pointer)
{
	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	CacheIdRegisterLocalInvalid_DEBUG1;

	/* ----------------
	 *	Add message to InvalidForall linked list.
	 * ----------------
	 */
	InvalidForall = CacheIdRegisterSpecifiedLocalInvalid(InvalidForall,
											cacheId, hashIndex, pointer);
	/* ----------------
	 *	Add message to InvalidLocal linked list.
	 * ----------------
	 */
	InvalidLocal = CacheIdRegisterSpecifiedLocalInvalid(InvalidLocal,
											cacheId, hashIndex, pointer);
}

/* --------------------------------
 *		CacheIdRegisterLocalRollback
 * --------------------------------
 */
static void
CacheIdRegisterLocalRollback(int cacheId,
							 Index hashIndex,
							 ItemPointer pointer)
{

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	CacheIdRegisterLocalRollback_DEBUG1;

	/* ----------------
	 *	Add message to RollbackStack linked list.
	 * ----------------
	 */
	RollbackStack = CacheIdRegisterSpecifiedLocalInvalid(
							 RollbackStack, cacheId, hashIndex, pointer);
}

/* --------------------------------
 *		RelationIdRegisterSpecifiedLocalInvalid
 * --------------------------------
 */
static LocalInvalid
RelationIdRegisterSpecifiedLocalInvalid(LocalInvalid invalid,
										Oid relationId, Oid objectId)
{
	InvalidationMessage message;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "RelationRegisterSpecifiedLocalInvalid(%u, %u)", relationId,
		 objectId);
#endif	 /* defined(INVALIDDEBUG) */

	/* ----------------
	 *	create a message describing the relation descriptor
	 *	we wish to invalidate.
	 * ----------------
	 */
	message = (InvalidationMessage)
		InvalidationEntryAllocate(sizeof(InvalidationMessageData));

	message->kind = 'r';
	message->any.relation.relationId = relationId;
	message->any.relation.objectId = objectId;

	/* ----------------
	 *	Add message to linked list of unprocessed messages.
	 * ----------------
	 */
	invalid = LocalInvalidRegister(invalid, (InvalidationEntry) message);
	return invalid;
}

/* --------------------------------
 *		RelationIdRegisterLocalInvalid
 * --------------------------------
 */
static void
RelationIdRegisterLocalInvalid(Oid relationId, Oid objectId)
{
	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "RelationRegisterLocalInvalid(%u, %u)", relationId,
		 objectId);
#endif	 /* defined(INVALIDDEBUG) */

	/* ----------------
	 *	Add message to InvalidForall linked list.
	 * ----------------
	 */
	InvalidForall = RelationIdRegisterSpecifiedLocalInvalid(InvalidForall,
												   relationId, objectId);
	/* ----------------
	 *	Add message to InvalidLocal linked list.
	 * ----------------
	 */
	InvalidLocal = RelationIdRegisterSpecifiedLocalInvalid(InvalidLocal,
												   relationId, objectId);
}

/* --------------------------------
 *		RelationIdRegisterLocalRollback
 * --------------------------------
 */
static void
RelationIdRegisterLocalRollback(Oid relationId, Oid objectId)
{

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "RelationRegisterLocalRollback(%u, %u)", relationId,
		 objectId);
#endif	 /* defined(INVALIDDEBUG) */

	/* ----------------
	 *	Add message to RollbackStack linked list.
	 * ----------------
	 */
	RollbackStack = RelationIdRegisterSpecifiedLocalInvalid(
									RollbackStack, relationId, objectId);
}

/* --------------------------------
 *		CacheIdInvalidate
 *
 *		This routine can invalidate a tuple in a system catalog cache
 *		or a cached relation descriptor.  You pay your money and you
 *		take your chances...
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define CacheIdInvalidate_DEBUG1 \
elog(DEBUG, "CacheIdInvalidate(%d, %d, 0x%x[%d])", cacheId, hashIndex,\
	 pointer, ItemPointerIsValid(pointer))
#else
#define CacheIdInvalidate_DEBUG1
#endif	 /* defined(INVALIDDEBUG) */

static void
CacheIdInvalidate(Index cacheId,
				  Index hashIndex,
				  ItemPointer pointer)
{
	/* ----------------
	 *	assume that if the item pointer is valid, then we are
	 *	invalidating an item in the specified system catalog cache.
	 * ----------------
	 */
	if (ItemPointerIsValid(pointer))
	{
		CatalogCacheIdInvalidate(cacheId, hashIndex, pointer);
		return;
	}

	CacheIdInvalidate_DEBUG1;

	/* ----------------
	 *	if the cacheId is the oid of any of the following system relations,
	 *	then assume we are invalidating a relation descriptor
	 * ----------------
	 */
	if (cacheId == RelOid_pg_class)
	{
		RelationIdInvalidateRelationCacheByRelationId(hashIndex);
		return;
	}

	if (cacheId == RelOid_pg_attribute)
	{
		RelationIdInvalidateRelationCacheByRelationId(hashIndex);
		return;
	}

	/* ----------------
	 *	Yow! the caller asked us to invalidate something else.
	 * ----------------
	 */
	elog(FATAL, "CacheIdInvalidate: cacheId=%d relation id?", cacheId);
}

/* --------------------------------
 *		ResetSystemCaches
 *
 *		This blows away all tuples in the system catalog caches and
 *		all the cached relation descriptors (and closes their files too).
 *		Relation descriptors that have positive refcounts are then rebuilt.
 * --------------------------------
 */
static void
ResetSystemCaches(void)
{
	ResetSystemCache();
	RelationCacheInvalidate();
}

/* --------------------------------
 *		InvalidationMessageRegisterSharedInvalid
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define InvalidationMessageRegisterSharedInvalid_DEBUG1 \
elog(DEBUG,\
	 "InvalidationMessageRegisterSharedInvalid(c, %d, %d, [%d, %d])",\
	 message->any.catalog.cacheId,\
	 message->any.catalog.hashIndex,\
	 ItemPointerGetBlockNumber(&message->any.catalog.pointerData),\
	 ItemPointerGetOffsetNumber(&message->any.catalog.pointerData))
#define InvalidationMessageRegisterSharedInvalid_DEBUG2 \
	 elog(DEBUG, \
		  "InvalidationMessageRegisterSharedInvalid(r, %u, %u)", \
		  message->any.relation.relationId, \
		  message->any.relation.objectId)
#else
#define InvalidationMessageRegisterSharedInvalid_DEBUG1
#define InvalidationMessageRegisterSharedInvalid_DEBUG2
#endif	 /* INVALIDDEBUG */

static void
InvalidationMessageRegisterSharedInvalid(InvalidationMessage message)
{
	Assert(PointerIsValid(message));

	switch (message->kind)
	{
		case 'c':				/* cached system catalog tuple */
			InvalidationMessageRegisterSharedInvalid_DEBUG1;

			RegisterSharedInvalid(message->any.catalog.cacheId,
								  message->any.catalog.hashIndex,
								  &message->any.catalog.pointerData);
			break;

		case 'r':				/* cached relation descriptor */
			InvalidationMessageRegisterSharedInvalid_DEBUG2;

			RegisterSharedInvalid(message->any.relation.relationId,
								  message->any.relation.objectId,
								  (ItemPointer) NULL);
			break;

		default:
			elog(FATAL,
				 "InvalidationMessageRegisterSharedInvalid: `%c' kind",
				 message->kind);
	}
}

/* --------------------------------
 *		InvalidationMessageCacheInvalidate
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define InvalidationMessageCacheInvalidate_DEBUG1 \
elog(DEBUG, "InvalidationMessageCacheInvalidate(c, %d, %d, [%d, %d])",\
	 message->any.catalog.cacheId,\
	 message->any.catalog.hashIndex,\
	 ItemPointerGetBlockNumber(&message->any.catalog.pointerData),\
	 ItemPointerGetOffsetNumber(&message->any.catalog.pointerData))
#define InvalidationMessageCacheInvalidate_DEBUG2 \
	 elog(DEBUG, "InvalidationMessageCacheInvalidate(r, %u, %u)", \
		  message->any.relation.relationId, \
		  message->any.relation.objectId)
#else
#define InvalidationMessageCacheInvalidate_DEBUG1
#define InvalidationMessageCacheInvalidate_DEBUG2
#endif	 /* defined(INVALIDDEBUG) */

static void
InvalidationMessageCacheInvalidate(InvalidationMessage message)
{
	Assert(PointerIsValid(message));

	switch (message->kind)
	{
		case 'c':				/* cached system catalog tuple */
			InvalidationMessageCacheInvalidate_DEBUG1;

			CacheIdInvalidate(message->any.catalog.cacheId,
							  message->any.catalog.hashIndex,
							  &message->any.catalog.pointerData);
			break;

		case 'r':				/* cached relation descriptor */
			InvalidationMessageCacheInvalidate_DEBUG2;

			CacheIdInvalidate(message->any.relation.relationId,
							  message->any.relation.objectId,
							  (ItemPointer) NULL);
			break;

		default:
			elog(FATAL, "InvalidationMessageCacheInvalidate: `%c' kind",
				 message->kind);
	}
}

/* --------------------------------
 *		PrepareToInvalidateRelationCache
 * --------------------------------
 */
static void
PrepareToInvalidateRelationCache(Relation relation,
								 HeapTuple tuple,
								 void (*function) (Oid, Oid))
{
	Oid			relationId;
	Oid			objectId;

	/* ----------------
	 *	get the relation object id
	 * ----------------
	 */
	relationId = RelationGetRelid(relation);

	/* ----------------
	 *	is it one of the ones we need to send an SI message for?
	 * ----------------
	 */
	if (relationId == RelOid_pg_class)
		objectId = tuple->t_data->t_oid;
	else if (relationId == RelOid_pg_attribute)
		objectId = ((Form_pg_attribute) GETSTRUCT(tuple))->attrelid;
	else
		return;

	/* ----------------
	 *	register the relcache-invalidation action in the appropriate list
	 * ----------------
	 */
	Assert(PointerIsValid(function));

	(*function) (relationId, objectId);
}


/*
 * DiscardInvalid
 *		Causes the invalidated cache state to be discarded.
 *
 * Note:
 *		This should be called as the first step in processing a transaction.
 */
void
DiscardInvalid(void)
{
	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "DiscardInvalid called");
#endif	 /* defined(INVALIDDEBUG) */

	InvalidateSharedInvalid(CacheIdInvalidate, ResetSystemCaches);
}

/*
 * RegisterInvalid
 *		Causes registration of invalidated state with other backends iff true.
 *
 * Note:
 *		This should be called as the last step in processing a transaction.
 */
void
RegisterInvalid(bool send)
{
	LocalInvalid invalid;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "RegisterInvalid(%d) called", send);
#endif	 /* defined(INVALIDDEBUG) */

	/* ----------------
	 *	Process and free the current list of inval messages.
	 * ----------------
	 */

	DiscardInvalidStack(&InvalidLocal);
	if (send)
	{
		DiscardInvalidStack(&RollbackStack);
		invalid = InvalidForall;
		InvalidForall = EmptyLocalInvalid;		/* clear InvalidForall */
		LocalInvalidInvalidate(invalid, InvalidationMessageRegisterSharedInvalid, true);
	}
	else
	{
		DiscardInvalidStack(&InvalidForall);
		invalid = RollbackStack;
		RollbackStack = EmptyLocalInvalid;		/* clear RollbackStack */
		LocalInvalidInvalidate(invalid, InvalidationMessageCacheInvalidate, true);
	}

}

/*
 * ImmediateLocalInvalidation
 *		Causes invalidation immediately for the next command of the transaction.
 *
 * Note:
 *		This should be called during CommandCounterIncrement(),
 *		after we have advanced the command ID.
 */
void
ImmediateLocalInvalidation(bool send)
{
	LocalInvalid invalid;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "ImmediateLocalInvalidation(%d) called", send);
#endif	 /* defined(INVALIDDEBUG) */

	/* ----------------
	 *	Process and free the local list of inval messages.
	 * ----------------
	 */

	if (send)
	{
		invalid = InvalidLocal;
		InvalidLocal = EmptyLocalInvalid;		/* clear InvalidLocal */
		LocalInvalidInvalidate(invalid, InvalidationMessageCacheInvalidate, true);
	}
	else
	{

		/*
		 * This may be used for rollback to a savepoint. Don't clear
		 * InvalidForall and RollbackStack here.
		 */
		DiscardInvalidStack(&InvalidLocal);
		invalid = RollbackStack;
		LocalInvalidInvalidate(invalid, InvalidationMessageCacheInvalidate, false);
	}

}

/*
 * PrepareForTupleInvalidation
 *		Invoke functions for the tuple which register invalidation
 *		of catalog/relation cache.
 *	Note:
 *		Assumes object id is valid.
 *		Assumes tuple is valid.
 */
#ifdef	INVALIDDEBUG
#define PrepareForTupleInvalidation_DEBUG1 \
elog(DEBUG, "%s(%s, [%d,%d])", \
	 funcname,\
	 RelationGetPhysicalRelationName(relation), \
	 ItemPointerGetBlockNumber(&tuple->t_self), \
	 ItemPointerGetOffsetNumber(&tuple->t_self))
#else
#define PrepareForTupleInvalidation_DEBUG1
#endif	 /* defined(INVALIDDEBUG) */

static void
PrepareForTupleInvalidation(Relation relation, HeapTuple tuple,
							void (*CacheIdRegisterFunc) (int, Index,
														 ItemPointer),
							void (*RelationIdRegisterFunc) (Oid, Oid),
							const char *funcname)
{
	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(relation));
	Assert(HeapTupleIsValid(tuple));

	if (IsBootstrapProcessingMode())
		return;

	/* ----------------
	 *	We only need to worry about invalidation for tuples that are in
	 *	system relations; user-relation tuples are never in catcaches
	 *	and can't affect the relcache either.
	 * ----------------
	 */
	if (!IsSystemRelationName(NameStr(RelationGetForm(relation)->relname)))
		return;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	PrepareForTupleInvalidation_DEBUG1;

	PrepareToInvalidateCacheTuple(relation, tuple,
								  CacheIdRegisterFunc);

	PrepareToInvalidateRelationCache(relation, tuple,
									 RelationIdRegisterFunc);
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
								CacheIdRegisterLocalInvalid,
								RelationIdRegisterLocalInvalid,
								"RelationInvalidateHeapTuple");
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
								CacheIdRegisterLocalRollback,
								RelationIdRegisterLocalRollback,
								"RelationMark4RollbackHeapTuple");
}
