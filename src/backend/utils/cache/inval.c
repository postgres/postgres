/*-------------------------------------------------------------------------
 *
 * inval.c--
 *    POSTGRES cache invalidation dispatcher code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/cache/inval.c,v 1.4 1997/08/19 21:35:06 momjian Exp $
 *
 * Note - this code is real crufty...
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <miscadmin.h>

#include "access/heapam.h"	/* XXX to support hacks below */
#include "access/htup.h"
#include "catalog/catalog.h"
#include "storage/bufpage.h"
#include "storage/buf.h"	/* XXX for InvalidBuffer */
#include "storage/ipc.h"
#include "storage/sinval.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "catalog/catname.h"	/* XXX to support hacks below */
#include "utils/syscache.h"	/* XXX to support the hacks below */

static InvalidationEntry InvalidationEntryAllocate(uint16 size);
static void LocalInvalidInvalidate(LocalInvalid invalid, void (*function)());
static LocalInvalid LocalInvalidRegister(LocalInvalid invalid,
					 InvalidationEntry entry);
static void getmyrelids(void);


/* ----------------
 *	private invalidation structures
 * ----------------
 */
typedef struct CatalogInvalidationData {
    Index		cacheId;
    Index		hashIndex;
    ItemPointerData	pointerData;
} CatalogInvalidationData;

typedef struct RelationInvalidationData {
    Oid	relationId;
    Oid	objectId;
} RelationInvalidationData;

typedef union AnyInvalidation {
    CatalogInvalidationData	catalog;
    RelationInvalidationData	relation;
} AnyInvalidation;

typedef struct InvalidationMessageData {
    char		kind;
    AnyInvalidation	any;
} InvalidationMessageData;

typedef InvalidationMessageData	*InvalidationMessage;

/* ----------------
 *	variables and macros
 * ----------------
 */
static LocalInvalid Invalid = EmptyLocalInvalid;	/* XXX global */
static bool RefreshWhenInvalidate = false;

Oid MyRelationRelationId = InvalidOid;
Oid MyAttributeRelationId = InvalidOid;
Oid MyAMRelationId = 	InvalidOid;
Oid MyAMOPRelationId = 	InvalidOid;

#define ValidateHacks() \
    if (!OidIsValid(MyRelationRelationId)) getmyrelids()

/* ----------------------------------------------------------------
 *		"local" invalidation support functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *	InvalidationEntryAllocate--
 *		Allocates an invalidation entry.
 * --------------------------------
 */
static InvalidationEntry
InvalidationEntryAllocate(uint16 size)
{
    InvalidationEntryData	*entryDataP;
    entryDataP = (InvalidationEntryData *)
	malloc(sizeof (char *) + size);	/* XXX alignment */
    entryDataP->nextP = NULL;
    return ((Pointer) &entryDataP->userData);
}

/* --------------------------------
 *	LocalInvalidRegister --
 *	   Returns a new local cache invalidation state containing a new entry.
 * --------------------------------
 */
static LocalInvalid
LocalInvalidRegister(LocalInvalid invalid,
		     InvalidationEntry entry)
{
    Assert(PointerIsValid(entry));
    
    ((InvalidationUserData *)entry)->dataP[-1] =
	(InvalidationUserData *)invalid;
    
    return (entry);
}

/* --------------------------------
 *	LocalInvalidInvalidate--
 *		Processes, then frees all entries in a local cache 
 *		invalidation state.
 * --------------------------------
 */
static void
LocalInvalidInvalidate(LocalInvalid invalid, void (*function)())
{
    InvalidationEntryData	*entryDataP;
    
    while (PointerIsValid(invalid)) {
	entryDataP = (InvalidationEntryData *)
	    &((InvalidationUserData *)invalid)->dataP[-1];
	
	if (PointerIsValid(function)) {
	    (*function)((Pointer) &entryDataP->userData);
	}
	
	invalid = (Pointer) entryDataP->nextP;
	
	/* help catch errors */
	entryDataP->nextP = (InvalidationUserData *) NULL;
	
	free((Pointer)entryDataP);
    }
}

/* ----------------------------------------------------------------
 *		      private support functions
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *	CacheIdRegisterLocalInvalid
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define CacheIdRegisterLocalInvalid_DEBUG1 \
elog(DEBUG, "CacheIdRegisterLocalInvalid(%d, %d, [%d, %d])", \
     cacheId, hashIndex, ItemPointerGetBlockNumber(pointer), \
     ItemPointerGetOffsetNumber(pointer))
#else
#define CacheIdRegisterLocalInvalid_DEBUG1
#endif	/* INVALIDDEBUG */
    
static void
CacheIdRegisterLocalInvalid(Index cacheId,
			    Index hashIndex,
			    ItemPointer pointer)
{
    InvalidationMessage	message;
    
    /* ----------------
     *	debugging stuff
     * ----------------
     */
    CacheIdRegisterLocalInvalid_DEBUG1;
    
    /* ----------------
     *	create a message describing the system catalog tuple
     *  we wish to invalidate.
     * ----------------
     */
    message = (InvalidationMessage)
	InvalidationEntryAllocate(sizeof (InvalidationMessageData));
    
    message->kind = 'c';
    message->any.catalog.cacheId = cacheId;
    message->any.catalog.hashIndex = hashIndex;
    
    ItemPointerCopy(pointer, &message->any.catalog.pointerData);
    
    /* ----------------
     *	Note: Invalid is a global variable
     * ----------------
     */
    Invalid = LocalInvalidRegister(Invalid, (InvalidationEntry)message);
}

/* --------------------------------
 *	RelationIdRegisterLocalInvalid
 * --------------------------------
 */
static void
RelationIdRegisterLocalInvalid(Oid relationId, Oid objectId)
{
    InvalidationMessage	message;
    
    /* ----------------
     *	debugging stuff
     * ----------------
     */
#ifdef	INVALIDDEBUG
    elog(DEBUG, "RelationRegisterLocalInvalid(%d, %d)", relationId,
	 objectId);
#endif	/* defined(INVALIDDEBUG) */
    
    /* ----------------
     *	create a message describing the relation descriptor
     *  we wish to invalidate.
     * ----------------
     */
    message = (InvalidationMessage)
	InvalidationEntryAllocate(sizeof (InvalidationMessageData));
    
    message->kind = 'r';
    message->any.relation.relationId = relationId;
    message->any.relation.objectId = objectId;
    
    /* ----------------
     *	Note: Invalid is a global variable
     * ----------------
     */
    Invalid = LocalInvalidRegister(Invalid, (InvalidationEntry)message);
}

/* --------------------------------
 *	getmyrelids
 * --------------------------------
 */
static void
getmyrelids()
{
    HeapTuple	tuple;
    
    tuple = SearchSysCacheTuple(RELNAME,
				PointerGetDatum(RelationRelationName),
				0,0,0);
    Assert(HeapTupleIsValid(tuple));
    MyRelationRelationId = tuple->t_oid;
    
    tuple = SearchSysCacheTuple(RELNAME,
				PointerGetDatum(AttributeRelationName),
				0,0,0);
    Assert(HeapTupleIsValid(tuple));
    MyAttributeRelationId = tuple->t_oid;
    
    tuple = SearchSysCacheTuple(RELNAME,
				PointerGetDatum(AccessMethodRelationName),
				0,0,0);
    Assert(HeapTupleIsValid(tuple));
    MyAMRelationId = tuple->t_oid;
    
    tuple = SearchSysCacheTuple(RELNAME,
				PointerGetDatum(AccessMethodOperatorRelationName),
				0,0,0);
    Assert(HeapTupleIsValid(tuple));
    MyAMOPRelationId = tuple->t_oid;
}

/* --------------------------------
 *	CacheIdInvalidate
 *
 *	This routine can invalidate an tuple in a system catalog cache
 *	or a cached relation descriptor.  You pay your money and you
 *	take your chances...
 * --------------------------------
 */
#ifdef	INVALIDDEBUG
#define CacheIdInvalidate_DEBUG1 \
elog(DEBUG, "CacheIdInvalidate(%d, %d, 0x%x[%d])", cacheId, hashIndex,\
     pointer, ItemPointerIsValid(pointer))
#else
#define CacheIdInvalidate_DEBUG1
#endif	/* defined(INVALIDDEBUG) */
     
static void
CacheIdInvalidate(Index cacheId,
		  Index hashIndex,
		  ItemPointer pointer)
{
    /* ----------------
     *	assume that if the item pointer is valid, then we are
     *  invalidating an item in the specified system catalog cache.
     * ----------------
     */
    if (ItemPointerIsValid(pointer)) {
	CatalogCacheIdInvalidate(cacheId, hashIndex, pointer);
	return;
    }
    
    CacheIdInvalidate_DEBUG1;
    
    ValidateHacks();	/* XXX */
    
    /* ----------------
     *	if the cacheId is the oid of any of the tuples in the
     *  following system relations, then assume we are invalidating
     *  a relation descriptor
     * ----------------
     */
    if (cacheId == MyRelationRelationId) {
	RelationIdInvalidateRelationCacheByRelationId(hashIndex);
	return;
    }
    
    if (cacheId == MyAttributeRelationId) {
	RelationIdInvalidateRelationCacheByRelationId(hashIndex);
	return;
    }
    
    if (cacheId == MyAMRelationId) {
	RelationIdInvalidateRelationCacheByAccessMethodId(hashIndex);
	return;
    }
    
    if (cacheId == MyAMOPRelationId) {
	RelationIdInvalidateRelationCacheByAccessMethodId(InvalidOid);
	return;
    }
    
    /* ----------------
     *	Yow! the caller asked us to invalidate something else.
     * ----------------
     */
    elog(FATAL, "CacheIdInvalidate: cacheId=%d relation id?", cacheId);
}

/* --------------------------------
 *	ResetSystemCaches
 *
 *	this blows away all tuples in the system catalog caches and
 *	all the cached relation descriptors (and closes the files too).
 * --------------------------------
 */
static void
ResetSystemCaches()
{
    ResetSystemCache();
    RelationCacheInvalidate(false); 
}

/* --------------------------------
 *	InvalidationMessageRegisterSharedInvalid
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
	  "InvalidationMessageRegisterSharedInvalid(r, %d, %d)", \
	  message->any.relation.relationId, \
	  message->any.relation.objectId)
#else    
#define InvalidationMessageRegisterSharedInvalid_DEBUG1
#define InvalidationMessageRegisterSharedInvalid_DEBUG2
#endif /* INVALIDDEBUG */
     
static void
InvalidationMessageRegisterSharedInvalid(InvalidationMessage message)
{
    Assert(PointerIsValid(message));
    
    switch (message->kind) {
    case 'c':	/* cached system catalog tuple */
	InvalidationMessageRegisterSharedInvalid_DEBUG1;
	
	RegisterSharedInvalid(message->any.catalog.cacheId,
			      message->any.catalog.hashIndex,
			      &message->any.catalog.pointerData);
	break;
	
    case 'r':   /* cached relation descriptor */
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
 *	InvalidationMessageCacheInvalidate
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
     elog(DEBUG, "InvalidationMessageCacheInvalidate(r, %d, %d)", \
	  message->any.relation.relationId, \
	  message->any.relation.objectId)
#else
#define InvalidationMessageCacheInvalidate_DEBUG1
#define InvalidationMessageCacheInvalidate_DEBUG2
#endif	/* defined(INVALIDDEBUG) */
     
static void
InvalidationMessageCacheInvalidate(InvalidationMessage message)
{
    Assert(PointerIsValid(message));
    
    switch (message->kind) {
    case 'c':  /* cached system catalog tuple */
	InvalidationMessageCacheInvalidate_DEBUG1;
	
	CatalogCacheIdInvalidate(message->any.catalog.cacheId,
				 message->any.catalog.hashIndex,
				 &message->any.catalog.pointerData);
	break;
	
    case 'r':  /* cached relation descriptor */
	InvalidationMessageCacheInvalidate_DEBUG2;
	
	/* XXX ignore this--is this correct ??? */
	break;
	
    default:
	elog(FATAL, "InvalidationMessageCacheInvalidate: `%c' kind",
	     message->kind);
    }
}

/* --------------------------------
 *	RelationInvalidateRelationCache
 * --------------------------------
 */
static void
RelationInvalidateRelationCache(Relation relation,
				HeapTuple tuple,
				void (*function)())
{
    Oid	relationId;
    Oid	objectId = (Oid)0;
    
    /* ----------------
     *	get the relation object id
     * ----------------
     */
    ValidateHacks();	/* XXX */
    relationId = RelationGetRelationId(relation);
    
    /* ----------------
     *	
     * ----------------
     */
    if (relationId == MyRelationRelationId) {
	objectId = tuple->t_oid;
    } else if (relationId == MyAttributeRelationId) {
	objectId = ((AttributeTupleForm)GETSTRUCT(tuple))->attrelid;
    } else if (relationId == MyAMRelationId) {
	objectId = tuple->t_oid;
    } else if (relationId == MyAMOPRelationId) {
	; /* objectId is unused */
    } else 
	return;
    
    /* ----------------
     *	  can't handle immediate relation descriptor invalidation
     * ----------------
     */
    Assert(PointerIsValid(function));
    
    (*function)(relationId, objectId);
}

/*
 * DiscardInvalid --
 *	Causes the invalidated cache state to be discarded.
 *
 * Note:
 *	This should be called as the first step in processing a transaction.
 *	This should be called while waiting for a query from the front end
 *	when other backends are active.
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
#endif	/* defined(INVALIDDEBUG) */
    
    InvalidateSharedInvalid(CacheIdInvalidate, ResetSystemCaches);
}

/*
 * RegisterInvalid --
 *	Causes registration of invalidated state with other backends iff true.
 *
 * Note:
 *	This should be called as the last step in processing a transaction.
 */
void
RegisterInvalid(bool send)
{
    /* ----------------
     *	debugging stuff
     * ----------------
     */
#ifdef	INVALIDDEBUG
    elog(DEBUG, "RegisterInvalid(%d) called", send);
#endif	/* defined(INVALIDDEBUG) */
    
    /* ----------------
     *	Note: Invalid is a global variable
     * ----------------
     */
    if (send)
	LocalInvalidInvalidate(Invalid,
			       InvalidationMessageRegisterSharedInvalid);
    else
	LocalInvalidInvalidate(Invalid,
			       InvalidationMessageCacheInvalidate);
    
    Invalid = EmptyLocalInvalid;
}

/*
 * SetRefreshWhenInvalidate --
 *	Causes the local caches to be immediately refreshed iff true.
 */
void
SetRefreshWhenInvalidate(bool on)
{
#ifdef	INVALIDDEBUG
    elog(DEBUG, "RefreshWhenInvalidate(%d) called", on);
#endif	/* defined(INVALIDDEBUG) */
    
    RefreshWhenInvalidate = on;
}

/*
 * RelationIdInvalidateHeapTuple --
 *	Causes the given tuple in a relation to be invalidated.
 *
 * Note:
 *	Assumes object id is valid.
 *	Assumes tuple is valid.
 */
#ifdef	INVALIDDEBUG
#define RelationInvalidateHeapTuple_DEBUG1 \
elog(DEBUG, "RelationInvalidateHeapTuple(%.16s, [%d,%d])", \
     RelationGetRelationName(relation), \
     ItemPointerGetBlockNumber(&tuple->t_ctid), \
     ItemPointerGetOffsetNumber(&tuple->t_ctid))
#else
#define RelationInvalidateHeapTuple_DEBUG1
#endif	/* defined(INVALIDDEBUG) */
     
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
    if (! IsSystemRelationName(RelationGetRelationTupleForm(relation)->relname.data))
	return;
    
    /* ----------------
     *	debugging stuff
     * ----------------
     */
    RelationInvalidateHeapTuple_DEBUG1;
    
    /* ----------------
     *	
     * ----------------
     */
    RelationInvalidateCatalogCacheTuple(relation,
					tuple,
					CacheIdRegisterLocalInvalid);
    
    RelationInvalidateRelationCache(relation,
				    tuple,
				    RelationIdRegisterLocalInvalid);
    
    if (RefreshWhenInvalidate)
	RelationInvalidateCatalogCacheTuple(relation,
					    tuple,
					    (void (*)()) NULL);
}

