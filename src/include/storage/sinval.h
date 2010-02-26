/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/sinval.h,v 1.59 2010/02/26 02:01:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include "storage/itemptr.h"
#include "storage/relfilenode.h"


/*
 * We support several types of shared-invalidation messages:
 *	* invalidate a specific tuple in a specific catcache
 *	* invalidate all catcache entries from a given system catalog
 *	* invalidate a relcache entry for a specific logical relation
 *	* invalidate an smgr cache entry for a specific physical relation
 *	* invalidate the mapped-relation mapping for a given database
 * More types could be added if needed.  The message type is identified by
 * the first "int16" field of the message struct.  Zero or positive means a
 * specific-catcache inval message (and also serves as the catcache ID field).
 * Negative values identify the other message types, as per codes below.
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
 *
 * Catcache and relcache invalidations are transactional, and so are sent
 * to other backends upon commit.  Internally to the generating backend,
 * they are also processed at CommandCounterIncrement so that later commands
 * in the same transaction see the new state.  The generating backend also
 * has to process them at abort, to flush out any cache state it's loaded
 * from no-longer-valid entries.
 *
 * smgr and relation mapping invalidations are non-transactional: they are
 * sent immediately when the underlying file change is made.
 */

typedef struct
{
	/* note: field layout chosen with an eye to alignment concerns */
	int16		id;				/* cache ID --- must be first */
	ItemPointerData tuplePtr;	/* tuple identifier in cached relation */
	Oid			dbId;			/* database ID, or 0 if a shared relation */
	uint32		hashValue;		/* hash value of key for this catcache */
} SharedInvalCatcacheMsg;

#define SHAREDINVALCATALOG_ID	(-1)

typedef struct
{
	int16		id;				/* type field --- must be first */
	Oid			dbId;			/* database ID, or 0 if a shared catalog */
	Oid			catId;			/* ID of catalog whose contents are invalid */
} SharedInvalCatalogMsg;

#define SHAREDINVALRELCACHE_ID	(-2)

typedef struct
{
	int16		id;				/* type field --- must be first */
	Oid			dbId;			/* database ID, or 0 if a shared relation */
	Oid			relId;			/* relation ID */
} SharedInvalRelcacheMsg;

#define SHAREDINVALSMGR_ID		(-3)

typedef struct
{
	int16		id;				/* type field --- must be first */
	RelFileNode rnode;			/* physical file ID */
} SharedInvalSmgrMsg;

#define SHAREDINVALRELMAP_ID	(-4)

typedef struct
{
	int16		id;				/* type field --- must be first */
	Oid			dbId;			/* database ID, or 0 for shared catalogs */
} SharedInvalRelmapMsg;

typedef union
{
	int16		id;				/* type field --- must be first */
	SharedInvalCatcacheMsg cc;
	SharedInvalCatalogMsg cat;
	SharedInvalRelcacheMsg rc;
	SharedInvalSmgrMsg sm;
	SharedInvalRelmapMsg rm;
} SharedInvalidationMessage;


extern void SendSharedInvalidMessages(const SharedInvalidationMessage *msgs,
						  int n);
extern void ReceiveSharedInvalidMessages(
					  void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void));

/* signal handler for catchup events (PROCSIG_CATCHUP_INTERRUPT) */
extern void HandleCatchupInterrupt(void);

/*
 * enable/disable processing of catchup events directly from signal handler.
 * The enable routine first performs processing of any catchup events that
 * have occurred since the last disable.
 */
extern void EnableCatchupInterrupt(void);
extern bool DisableCatchupInterrupt(void);

extern int xactGetCommittedInvalidationMessages(SharedInvalidationMessage **msgs,
									 bool *RelcacheInitFileInval);
extern void ProcessCommittedInvalidationMessages(SharedInvalidationMessage *msgs,
									 int nmsgs, bool RelcacheInitFileInval,
									 Oid dbid, Oid tsid);

#endif   /* SINVAL_H */
