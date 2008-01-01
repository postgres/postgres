/*-------------------------------------------------------------------------
 *
 * resowner.h
 *	  POSTGRES resource owner definitions.
 *
 * Query-lifespan resources are tracked by associating them with
 * ResourceOwner objects.  This provides a simple mechanism for ensuring
 * that such resources are freed at the right time.
 * See utils/resowner/README for more info.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/resowner.h,v 1.15 2008/01/01 19:45:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESOWNER_H
#define RESOWNER_H

#include "storage/buf.h"
#include "utils/catcache.h"
#include "utils/plancache.h"


/*
 * ResourceOwner objects are an opaque data structure known only within
 * resowner.c.
 */
typedef struct ResourceOwnerData *ResourceOwner;


/*
 * Globally known ResourceOwners
 */
extern PGDLLIMPORT ResourceOwner CurrentResourceOwner;
extern PGDLLIMPORT ResourceOwner CurTransactionResourceOwner;
extern PGDLLIMPORT ResourceOwner TopTransactionResourceOwner;

/*
 * Resource releasing is done in three phases: pre-locks, locks, and
 * post-locks.	The pre-lock phase must release any resources that are
 * visible to other backends (such as pinned buffers); this ensures that
 * when we release a lock that another backend may be waiting on, it will
 * see us as being fully out of our transaction.  The post-lock phase
 * should be used for backend-internal cleanup.
 */
typedef enum
{
	RESOURCE_RELEASE_BEFORE_LOCKS,
	RESOURCE_RELEASE_LOCKS,
	RESOURCE_RELEASE_AFTER_LOCKS
} ResourceReleasePhase;

/*
 *	Dynamically loaded modules can get control during ResourceOwnerRelease
 *	by providing a callback of this form.
 */
typedef void (*ResourceReleaseCallback) (ResourceReleasePhase phase,
													 bool isCommit,
													 bool isTopLevel,
													 void *arg);


/*
 * Functions in resowner.c
 */

/* generic routines */
extern ResourceOwner ResourceOwnerCreate(ResourceOwner parent,
					const char *name);
extern void ResourceOwnerRelease(ResourceOwner owner,
					 ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel);
extern void ResourceOwnerDelete(ResourceOwner owner);
extern ResourceOwner ResourceOwnerGetParent(ResourceOwner owner);
extern void ResourceOwnerNewParent(ResourceOwner owner,
					   ResourceOwner newparent);
extern void RegisterResourceReleaseCallback(ResourceReleaseCallback callback,
								void *arg);
extern void UnregisterResourceReleaseCallback(ResourceReleaseCallback callback,
								  void *arg);

/* support for buffer refcount management */
extern void ResourceOwnerEnlargeBuffers(ResourceOwner owner);
extern void ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer);
extern void ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer);

/* support for catcache refcount management */
extern void ResourceOwnerEnlargeCatCacheRefs(ResourceOwner owner);
extern void ResourceOwnerRememberCatCacheRef(ResourceOwner owner,
								 HeapTuple tuple);
extern void ResourceOwnerForgetCatCacheRef(ResourceOwner owner,
							   HeapTuple tuple);
extern void ResourceOwnerEnlargeCatCacheListRefs(ResourceOwner owner);
extern void ResourceOwnerRememberCatCacheListRef(ResourceOwner owner,
									 CatCList *list);
extern void ResourceOwnerForgetCatCacheListRef(ResourceOwner owner,
								   CatCList *list);

/* support for relcache refcount management */
extern void ResourceOwnerEnlargeRelationRefs(ResourceOwner owner);
extern void ResourceOwnerRememberRelationRef(ResourceOwner owner,
								 Relation rel);
extern void ResourceOwnerForgetRelationRef(ResourceOwner owner,
							   Relation rel);

/* support for plancache refcount management */
extern void ResourceOwnerEnlargePlanCacheRefs(ResourceOwner owner);
extern void ResourceOwnerRememberPlanCacheRef(ResourceOwner owner,
								  CachedPlan *plan);
extern void ResourceOwnerForgetPlanCacheRef(ResourceOwner owner,
								CachedPlan *plan);

/* support for tupledesc refcount management */
extern void ResourceOwnerEnlargeTupleDescs(ResourceOwner owner);
extern void ResourceOwnerRememberTupleDesc(ResourceOwner owner,
							   TupleDesc tupdesc);
extern void ResourceOwnerForgetTupleDesc(ResourceOwner owner,
							 TupleDesc tupdesc);

#endif   /* RESOWNER_H */
