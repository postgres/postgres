/*-------------------------------------------------------------------------
 *
 * inval.c
 *	  POSTGRES cache invalidation dispatcher code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/inval.c,v 1.30 1999/11/21 01:58:22 tgl Exp $
 *
 * Note - this code is real crufty...
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/pg_class.h"
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
static LocalInvalid Invalid = EmptyLocalInvalid;	/* head of linked list */


static InvalidationEntry InvalidationEntryAllocate(uint16 size);
static void LocalInvalidInvalidate(LocalInvalid invalid, void (*function) ());
static LocalInvalid LocalInvalidRegister(LocalInvalid invalid,
										 InvalidationEntry entry);


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
 *				invalidation list.
 * --------------------------------
 */
static void
LocalInvalidInvalidate(LocalInvalid invalid, void (*function) ())
{
	InvalidationEntryData *entryDataP;

	while (PointerIsValid(invalid))
	{
		entryDataP = (InvalidationEntryData *)
			&((InvalidationUserData *) invalid)->dataP[-1];

		if (PointerIsValid(function))
			(*function) ((Pointer) &entryDataP->userData);

		invalid = (Pointer) entryDataP->nextP;

		/* help catch errors */
		entryDataP->nextP = (InvalidationUserData *) NULL;

		free((Pointer) entryDataP);
	}
}

/* ----------------------------------------------------------------
 *					  private support functions
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		CacheIdRegisterLocalInvalid
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define CacheIdRegisterLocalInvalid_DEBUG1 \
elog(DEBUG, "CacheIdRegisterLocalInvalid(%d, %d, [%d, %d])", \
	 cacheId, hashIndex, ItemPointerGetBlockNumber(pointer), \
	 ItemPointerGetOffsetNumber(pointer))
#else
#define CacheIdRegisterLocalInvalid_DEBUG1
#endif	 /* INVALIDDEBUG */

static void
CacheIdRegisterLocalInvalid(Index cacheId,
							Index hashIndex,
							ItemPointer pointer)
{
	InvalidationMessage message;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	CacheIdRegisterLocalInvalid_DEBUG1;

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
	Invalid = LocalInvalidRegister(Invalid, (InvalidationEntry) message);
}

/* --------------------------------
 *		RelationIdRegisterLocalInvalid
 * --------------------------------
 */
static void
RelationIdRegisterLocalInvalid(Oid relationId, Oid objectId)
{
	InvalidationMessage message;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
#ifdef	INVALIDDEBUG
	elog(DEBUG, "RelationRegisterLocalInvalid(%u, %u)", relationId,
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
	Invalid = LocalInvalidRegister(Invalid, (InvalidationEntry) message);
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
 *		this blows away all tuples in the system catalog caches and
 *		all the cached relation descriptors (and closes the files too).
 * --------------------------------
 */
static void
ResetSystemCaches()
{
	ResetSystemCache();
	RelationCacheInvalidate(false);
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

			CatalogCacheIdInvalidate(message->any.catalog.cacheId,
									 message->any.catalog.hashIndex,
									 &message->any.catalog.pointerData);
			break;

		case 'r':				/* cached relation descriptor */
			InvalidationMessageCacheInvalidate_DEBUG2;

			/* XXX ignore this--is this correct ??? */
			break;

		default:
			elog(FATAL, "InvalidationMessageCacheInvalidate: `%c' kind",
				 message->kind);
	}
}

/* --------------------------------
 *		RelationInvalidateRelationCache
 * --------------------------------
 */
static void
RelationInvalidateRelationCache(Relation relation,
								HeapTuple tuple,
								void (*function) ())
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
	 *	  can't handle immediate relation descriptor invalidation
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
 *		This should be called while waiting for a query from the front end
 *		when other backends are active.
 */
void
DiscardInvalid()
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
	invalid = Invalid;
	Invalid = EmptyLocalInvalid; /* anything added now is part of a new list */

	if (send)
		LocalInvalidInvalidate(invalid,
							   InvalidationMessageRegisterSharedInvalid);
	else
		LocalInvalidInvalidate(invalid,
							   InvalidationMessageCacheInvalidate);

}

/*
 * RelationIdInvalidateHeapTuple
 *		Causes the given tuple in a relation to be invalidated.
 *
 * Note:
 *		Assumes object id is valid.
 *		Assumes tuple is valid.
 */
#ifdef	INVALIDDEBUG
#define RelationInvalidateHeapTuple_DEBUG1 \
elog(DEBUG, "RelationInvalidateHeapTuple(%s, [%d,%d])", \
	 RelationGetPhysicalRelationName(relation), \
	 ItemPointerGetBlockNumber(&tuple->t_ctid), \
	 ItemPointerGetOffsetNumber(&tuple->t_ctid))
#else
#define RelationInvalidateHeapTuple_DEBUG1
#endif	 /* defined(INVALIDDEBUG) */

void
RelationInvalidateHeapTuple(Relation relation, HeapTuple tuple)
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
	 *	this only works for system relations now
	 * ----------------
	 */
	if (!IsSystemRelationName(NameStr(RelationGetForm(relation)->relname)))
		return;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	RelationInvalidateHeapTuple_DEBUG1;

	RelationInvalidateCatalogCacheTuple(relation,
										tuple,
										CacheIdRegisterLocalInvalid);

	RelationInvalidateRelationCache(relation,
									tuple,
									RelationIdRegisterLocalInvalid);
}
