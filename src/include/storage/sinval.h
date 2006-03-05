/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/sinval.h,v 1.44 2006/03/05 15:59:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include "storage/itemptr.h"
#include "storage/relfilenode.h"


/*
 * We currently support three types of shared-invalidation messages: one that
 * invalidates an entry in a catcache, one that invalidates a relcache entry,
 * and one that invalidates an smgr cache entry.  More types could be added
 * if needed.  The message type is identified by the first "int16" field of
 * the message struct.	Zero or positive means a catcache inval message (and
 * also serves as the catcache ID field).  -1 means a relcache inval message.
 * -2 means an smgr inval message.	Other negative values are available to
 * identify other inval message types.
 *
 * Catcache inval events are initially driven by detecting tuple inserts,
 * updates and deletions in system catalogs (see CacheInvalidateHeapTuple).
 * An update generates two inval events, one for the old tuple and one for
 * the new --- this is needed to get rid of both positive entries for the
 * old tuple, and negative cache entries associated with the new tuple's
 * cache key.  (This could perhaps be optimized down to one event when the
 * cache key is not changing, but for now we don't bother to try.)  Note that
 * the inval events themselves don't actually say whether the tuple is being
 * inserted or deleted.
 *
 * Note that some system catalogs have multiple caches on them (with different
 * indexes).  On detecting a tuple invalidation in such a catalog, separate
 * catcache inval messages must be generated for each of its caches.  The
 * catcache inval messages carry the hash value for the target tuple, so
 * that the catcache only needs to search one hash chain not all its chains,
 * and so that negative cache entries can be recognized with good accuracy.
 * (Of course this assumes that all the backends are using identical hashing
 * code, but that should be OK.)
 */

typedef struct
{
	/* note: field layout chosen with an eye to alignment concerns */
	int16		id;				/* cache ID --- must be first */
	ItemPointerData tuplePtr;	/* tuple identifier in cached relation */
	Oid			dbId;			/* database ID, or 0 if a shared relation */
	uint32		hashValue;		/* hash value of key for this catcache */
} SharedInvalCatcacheMsg;

#define SHAREDINVALRELCACHE_ID	(-1)

typedef struct
{
	int16		id;				/* type field --- must be first */
	Oid			dbId;			/* database ID, or 0 if a shared relation */
	Oid			relId;			/* relation ID */
} SharedInvalRelcacheMsg;

#define SHAREDINVALSMGR_ID		(-2)

typedef struct
{
	int16		id;				/* type field --- must be first */
	RelFileNode rnode;			/* physical file ID */
} SharedInvalSmgrMsg;

typedef union
{
	int16		id;				/* type field --- must be first */
	SharedInvalCatcacheMsg cc;
	SharedInvalRelcacheMsg rc;
	SharedInvalSmgrMsg sm;
} SharedInvalidationMessage;


extern Size SInvalShmemSize(void);
extern void CreateSharedInvalidationState(void);
extern void InitBackendSharedInvalidationState(void);

extern void SendSharedInvalidMessage(SharedInvalidationMessage *msg);
extern void ReceiveSharedInvalidMessages(
					  void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void));

/* signal handler for catchup events (SIGUSR1) */
extern void CatchupInterruptHandler(SIGNAL_ARGS);

/*
 * enable/disable processing of catchup events directly from signal handler.
 * The enable routine first performs processing of any catchup events that
 * have occurred since the last disable.
 */
extern void EnableCatchupInterrupt(void);
extern bool DisableCatchupInterrupt(void);

#endif   /* SINVAL_H */
