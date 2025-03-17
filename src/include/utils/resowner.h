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
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/resowner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESOWNER_H
#define RESOWNER_H


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
extern PGDLLIMPORT ResourceOwner AuxProcessResourceOwner;

/*
 * Resource releasing is done in three phases: pre-locks, locks, and
 * post-locks.  The pre-lock phase must release any resources that are visible
 * to other backends (such as pinned buffers); this ensures that when we
 * release a lock that another backend may be waiting on, it will see us as
 * being fully out of our transaction.  The post-lock phase should be used for
 * backend-internal cleanup.
 *
 * Within each phase, resources are released in priority order.  Priority is
 * just an integer specified in ResourceOwnerDesc.  The priorities of built-in
 * resource types are given below, extensions may use any priority relative to
 * those or RELEASE_PRIO_FIRST/LAST.  RELEASE_PRIO_FIRST is a fine choice if
 * your resource doesn't depend on any other resources.
 */
typedef enum
{
	RESOURCE_RELEASE_BEFORE_LOCKS = 1,
	RESOURCE_RELEASE_LOCKS,
	RESOURCE_RELEASE_AFTER_LOCKS,
} ResourceReleasePhase;

typedef uint32 ResourceReleasePriority;

/* priorities of built-in BEFORE_LOCKS resources */
#define RELEASE_PRIO_BUFFER_IOS			    100
#define RELEASE_PRIO_BUFFER_PINS		    200
#define RELEASE_PRIO_RELCACHE_REFS			300
#define RELEASE_PRIO_DSMS					400
#define RELEASE_PRIO_JIT_CONTEXTS			500
#define RELEASE_PRIO_CRYPTOHASH_CONTEXTS	600
#define RELEASE_PRIO_HMAC_CONTEXTS			700

/* priorities of built-in AFTER_LOCKS resources */
#define RELEASE_PRIO_CATCACHE_REFS			100
#define RELEASE_PRIO_CATCACHE_LIST_REFS		200
#define RELEASE_PRIO_PLANCACHE_REFS			300
#define RELEASE_PRIO_TUPDESC_REFS			400
#define RELEASE_PRIO_SNAPSHOT_REFS			500
#define RELEASE_PRIO_FILES					600
#define RELEASE_PRIO_WAITEVENTSETS			700

/* 0 is considered invalid */
#define RELEASE_PRIO_FIRST					1
#define RELEASE_PRIO_LAST					UINT32_MAX

/*
 * In order to track an object, resowner.c needs a few callbacks for it.
 * The callbacks for resources of a specific kind are encapsulated in
 * ResourceOwnerDesc.
 *
 * Note that the callbacks occur post-commit or post-abort, so the callback
 * functions can only do noncritical cleanup and must not fail.
 */
typedef struct ResourceOwnerDesc
{
	const char *name;			/* name for the object kind, for debugging */

	/* when are these objects released? */
	ResourceReleasePhase release_phase;
	ResourceReleasePriority release_priority;

	/*
	 * Release resource.
	 *
	 * This is called for each resource in the resource owner, in the order
	 * specified by 'release_phase' and 'release_priority' when the whole
	 * resource owner is been released or when ResourceOwnerReleaseAllOfKind()
	 * is called.  The resource is implicitly removed from the owner, the
	 * callback function doesn't need to call ResourceOwnerForget.
	 */
	void		(*ReleaseResource) (Datum res);

	/*
	 * Format a string describing the resource, for debugging purposes.  If a
	 * resource has not been properly released before commit, this is used to
	 * print a WARNING.
	 *
	 * This can be left to NULL, in which case a generic "[resource name]: %p"
	 * format is used.
	 */
	char	   *(*DebugPrint) (Datum res);

} ResourceOwnerDesc;

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

extern void ResourceOwnerEnlarge(ResourceOwner owner);
extern void ResourceOwnerRemember(ResourceOwner owner, Datum value, const ResourceOwnerDesc *kind);
extern void ResourceOwnerForget(ResourceOwner owner, Datum value, const ResourceOwnerDesc *kind);

extern void ResourceOwnerReleaseAllOfKind(ResourceOwner owner, const ResourceOwnerDesc *kind);

extern void RegisterResourceReleaseCallback(ResourceReleaseCallback callback,
											void *arg);
extern void UnregisterResourceReleaseCallback(ResourceReleaseCallback callback,
											  void *arg);

extern void CreateAuxProcessResourceOwner(void);
extern void ReleaseAuxProcessResources(bool isCommit);

/* special support for local lock management */
struct LOCALLOCK;
extern void ResourceOwnerRememberLock(ResourceOwner owner, struct LOCALLOCK *locallock);
extern void ResourceOwnerForgetLock(ResourceOwner owner, struct LOCALLOCK *locallock);

/* special support for AIO */
struct dlist_node;
extern void ResourceOwnerRememberAioHandle(ResourceOwner owner, struct dlist_node *ioh_node);
extern void ResourceOwnerForgetAioHandle(ResourceOwner owner, struct dlist_node *ioh_node);

#endif							/* RESOWNER_H */
