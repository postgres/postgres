/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory management
 *
 * Portals are objects representing the execution state of a query.
 * This module provides memory management services for portals, but it
 * doesn't actually run the executor for them.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mmgr/portalmem.c,v 1.70 2004/08/29 04:13:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "commands/portalcmds.h"
#include "executor/executor.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/portal.h"

/*
 * estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in EnablePortalManager()
 */
#define PORTALS_PER_USER	   64


/* ----------------
 *		Global state
 * ----------------
 */

#define MAX_PORTALNAME_LEN		NAMEDATALEN

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

static HTAB *PortalHashTable = NULL;

#define PortalHashTableLookup(NAME, PORTAL) \
do { \
	PortalHashEnt *hentry; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	StrNCpy(key, NAME, MAX_PORTALNAME_LEN); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_FIND, NULL); \
	if (hentry) \
		PORTAL = hentry->portal; \
	else \
		PORTAL = NULL; \
} while(0)

#define PortalHashTableInsert(PORTAL, NAME) \
do { \
	PortalHashEnt *hentry; bool found; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	StrNCpy(key, NAME, MAX_PORTALNAME_LEN); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_ENTER, &found); \
	if (hentry == NULL) \
		ereport(ERROR, \
				(errcode(ERRCODE_OUT_OF_MEMORY), \
				 errmsg("out of memory"))); \
	if (found) \
		elog(ERROR, "duplicate portal name"); \
	hentry->portal = PORTAL; \
	/* To avoid duplicate storage, make PORTAL->name point to htab entry */ \
	PORTAL->name = hentry->portalname; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
do { \
	PortalHashEnt *hentry; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	StrNCpy(key, PORTAL->name, MAX_PORTALNAME_LEN); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "trying to delete portal name that does not exist"); \
} while(0)

static MemoryContext PortalMemory = NULL;


/* ----------------------------------------------------------------
 *				   public portal interface functions
 * ----------------------------------------------------------------
 */

/*
 * EnablePortalManager
 *		Enables the portal management module at backend startup.
 */
void
EnablePortalManager(void)
{
	HASHCTL		ctl;

	Assert(PortalMemory == NULL);

	PortalMemory = AllocSetContextCreate(TopMemoryContext,
										 "PortalMemory",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	ctl.keysize = MAX_PORTALNAME_LEN;
	ctl.entrysize = sizeof(PortalHashEnt);

	/*
	 * use PORTALS_PER_USER as a guess of how many hash table entries to
	 * create, initially
	 */
	PortalHashTable = hash_create("Portal hash", PORTALS_PER_USER,
								  &ctl, HASH_ELEM);
}

/*
 * GetPortalByName
 *		Returns a portal given a portal name, or NULL if name not found.
 */
Portal
GetPortalByName(const char *name)
{
	Portal		portal;

	if (PointerIsValid(name))
		PortalHashTableLookup(name, portal);
	else
		portal = NULL;

	return portal;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 * allowDup: if true, automatically drop any pre-existing portal of the
 * same name (if false, an error is raised).
 *
 * dupSilent: if true, don't even emit a WARNING.
 */
Portal
CreatePortal(const char *name, bool allowDup, bool dupSilent)
{
	Portal		portal;

	AssertArg(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		if (!allowDup)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already exists", name)));
		if (!dupSilent)
			ereport(WARNING,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("closing existing cursor \"%s\"",
							name)));
		PortalDrop(portal, false);
	}

	/* make new portal structure */
	portal = (Portal) MemoryContextAllocZero(PortalMemory, sizeof *portal);

	/* initialize portal heap context; typically it won't store much */
	portal->heap = AllocSetContextCreate(PortalMemory,
										 "PortalHeapMemory",
										 ALLOCSET_SMALL_MINSIZE,
										 ALLOCSET_SMALL_INITSIZE,
										 ALLOCSET_SMALL_MAXSIZE);

	/* create a resource owner for the portal */
	portal->resowner = ResourceOwnerCreate(CurTransactionResourceOwner,
										   "Portal");

	/* initialize portal fields that don't start off zero */
	portal->cleanup = PortalCleanup;
	portal->createXact = GetCurrentTransactionId();
	portal->strategy = PORTAL_MULTI_QUERY;
	portal->cursorOptions = CURSOR_OPT_NO_SCROLL;
	portal->atStart = true;
	portal->atEnd = true;		/* disallow fetches until query is set */

	/* put portal in table (sets portal->name) */
	PortalHashTableInsert(portal, name);

	return portal;
}

/*
 * CreateNewPortal
 *		Create a new portal, assigning it a random nonconflicting name.
 */
Portal
CreateNewPortal(void)
{
	static unsigned int unnamed_portal_count = 0;

	char		portalname[MAX_PORTALNAME_LEN];

	/* Select a nonconflicting name */
	for (;;)
	{
		unnamed_portal_count++;
		sprintf(portalname, "<unnamed portal %u>", unnamed_portal_count);
		if (GetPortalByName(portalname) == NULL)
			break;
	}

	return CreatePortal(portalname, false, false);
}

/*
 * PortalDefineQuery
 *		A simple subroutine to establish a portal's query.
 *
 * Notes: commandTag shall be NULL if and only if the original query string
 * (before rewriting) was an empty string.	Also, the passed commandTag must
 * be a pointer to a constant string, since it is not copied.  The caller is
 * responsible for ensuring that the passed sourceText (if any), parse and
 * plan trees have adequate lifetime.  Also, queryContext must accurately
 * describe the location of the parse and plan trees.
 */
void
PortalDefineQuery(Portal portal,
				  const char *sourceText,
				  const char *commandTag,
				  List *parseTrees,
				  List *planTrees,
				  MemoryContext queryContext)
{
	AssertArg(PortalIsValid(portal));
	AssertState(portal->queryContext == NULL);	/* else defined already */

	Assert(list_length(parseTrees) == list_length(planTrees));

	Assert(commandTag != NULL || parseTrees == NIL);

	portal->sourceText = sourceText;
	portal->commandTag = commandTag;
	portal->parseTrees = parseTrees;
	portal->planTrees = planTrees;
	portal->queryContext = queryContext;
}

/*
 * PortalCreateHoldStore
 *		Create the tuplestore for a portal.
 */
void
PortalCreateHoldStore(Portal portal)
{
	MemoryContext oldcxt;

	Assert(portal->holdContext == NULL);
	Assert(portal->holdStore == NULL);

	/*
	 * Create the memory context that is used for storage of the tuple
	 * set. Note this is NOT a child of the portal's heap memory.
	 */
	portal->holdContext =
		AllocSetContextCreate(PortalMemory,
							  "PortalHeapMemory",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/* Create the tuple store, selecting cross-transaction temp files. */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	/* XXX: Should maintenance_work_mem be used for the portal size? */
	portal->holdStore = tuplestore_begin_heap(true, true, work_mem);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * PortalDrop
 *		Destroy the portal.
 */
void
PortalDrop(Portal portal, bool isTopCommit)
{
	AssertArg(PortalIsValid(portal));

	/* Not sure if this case can validly happen or not... */
	if (portal->status == PORTAL_ACTIVE)
		elog(ERROR, "cannot drop active portal");

	/*
	 * Remove portal from hash table.  Because we do this first, we will
	 * not come back to try to remove the portal again if there's any
	 * error in the subsequent steps.  Better to leak a little memory than
	 * to get into an infinite error-recovery loop.
	 */
	PortalHashTableDelete(portal);

	/* let portalcmds.c clean up the state it knows about */
	if (PointerIsValid(portal->cleanup))
		(*portal->cleanup) (portal);

	/*
	 * Release any resources still attached to the portal.  There are
	 * several cases being covered here:
	 *
	 * Top transaction commit (indicated by isTopCommit): normally we should
	 * do nothing here and let the regular end-of-transaction resource
	 * releasing mechanism handle these resources too.  However, if we have
	 * a FAILED portal (eg, a cursor that got an error), we'd better clean
	 * up its resources to avoid resource-leakage warning messages.
	 *
	 * Sub transaction commit: never comes here at all, since we don't
	 * kill any portals in AtSubCommit_Portals().
	 *
	 * Main or sub transaction abort: we will do nothing here because
	 * portal->resowner was already set NULL; the resources were already
	 * cleaned up in transaction abort.
	 *
	 * Ordinary portal drop: must release resources.  However, if the portal
	 * is not FAILED then we do not release its locks.  The locks become
	 * the responsibility of the transaction's ResourceOwner (since it is
	 * the parent of the portal's owner) and will be released when the
	 * transaction eventually ends.
	 */
	if (portal->resowner &&
		(!isTopCommit || portal->status == PORTAL_FAILED))
	{
		bool	isCommit = (portal->status != PORTAL_FAILED);

		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_AFTER_LOCKS,
							 isCommit, false);
		ResourceOwnerDelete(portal->resowner);
	}
	portal->resowner = NULL;

	/*
	 * Delete tuplestore if present.  We should do this even under error
	 * conditions; since the tuplestore would have been using cross-
	 * transaction storage, its temp files need to be explicitly deleted.
	 */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_end(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
		portal->holdStore = NULL;
	}

	/* delete tuplestore storage, if any */
	if (portal->holdContext)
		MemoryContextDelete(portal->holdContext);

	/* release subsidiary storage */
	MemoryContextDelete(PortalGetHeapMemory(portal));

	/* release portal struct (it's in PortalMemory) */
	pfree(portal);
}

/*
 * DropDependentPortals
 *		Drop any portals using the specified context as queryContext.
 *
 * This is normally used to make sure we can safely drop a prepared statement.
 */
void
DropDependentPortals(MemoryContext queryContext)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->queryContext == queryContext)
			PortalDrop(portal, false);
	}
}


/*
 * Pre-commit processing for portals.
 *
 * Any holdable cursors created in this transaction need to be converted to
 * materialized form, since we are going to close down the executor and
 * release locks.  Remove all other portals created in this transaction.
 * Portals remaining from prior transactions should be left untouched.
 *
 * XXX This assumes that portals can be deleted in a random order, ie,
 * no portal has a reference to any other (at least not one that will be
 * exercised during deletion).	I think this is okay at the moment, but
 * we've had bugs of that ilk in the past.  Keep a close eye on cursor
 * references...
 */
void
AtCommit_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId xact = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * Do not touch active portals --- this can only happen in the
		 * case of a multi-transaction utility command, such as VACUUM.
		 *
		 * Note however that any resource owner attached to such a portal
		 * is still going to go away, so don't leave a dangling pointer.
		 */
		if (portal->status == PORTAL_ACTIVE)
		{
			portal->resowner = NULL;
			continue;
		}

		/*
		 * Do nothing else to cursors held over from a previous
		 * transaction. (This test must include checking CURSOR_OPT_HOLD,
		 * else we will fail to clean up a VACUUM portal if it fails after
		 * its first sub-transaction.)
		 */
		if (portal->createXact != xact &&
			(portal->cursorOptions & CURSOR_OPT_HOLD))
			continue;

		if ((portal->cursorOptions & CURSOR_OPT_HOLD) &&
			portal->status == PORTAL_READY)
		{
			/*
			 * We are exiting the transaction that created a holdable
			 * cursor.	Instead of dropping the portal, prepare it for
			 * access by later transactions.
			 *
			 * Note that PersistHoldablePortal() must release all resources
			 * used by the portal that are local to the creating
			 * transaction.
			 */
			PortalCreateHoldStore(portal);
			PersistHoldablePortal(portal);

			/*
			 * Any resources belonging to the portal will be released in the
			 * upcoming transaction-wide cleanup; the portal will no
			 * longer have its own resources.
			 */
			portal->resowner = NULL;
		}
		else
		{
			/* Zap all non-holdable portals */
			PortalDrop(portal, true);
		}
	}
}

/*
 * Abort processing for portals.
 *
 * At this point we reset "active" status and run the cleanup hook if
 * present, but we can't release memory until the cleanup call.
 *
 * The reason we need to reset active is so that we can replace the unnamed
 * portal, else we'll fail to execute ROLLBACK when it arrives.
 */
void
AtAbort_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId xact = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->status == PORTAL_ACTIVE)
			portal->status = PORTAL_FAILED;

		/*
		 * Do nothing else to cursors held over from a previous
		 * transaction. (This test must include checking CURSOR_OPT_HOLD,
		 * else we will fail to clean up a VACUUM portal if it fails after
		 * its first sub-transaction.)
		 */
		if (portal->createXact != xact &&
			(portal->cursorOptions & CURSOR_OPT_HOLD))
			continue;

		/* let portalcmds.c clean up the state it knows about */
		if (PointerIsValid(portal->cleanup))
		{
			(*portal->cleanup) (portal);
			portal->cleanup = NULL;
		}
		/*
		 * Any resources belonging to the portal will be released in the
		 * upcoming transaction-wide cleanup; they will be gone before
		 * we run PortalDrop.
		 */
		portal->resowner = NULL;
	}
}

/*
 * Post-abort cleanup for portals.
 *
 * Delete all portals not held over from prior transactions.  */
void
AtCleanup_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId xact = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * Do nothing else to cursors held over from a previous
		 * transaction. (This test must include checking CURSOR_OPT_HOLD,
		 * else we will fail to clean up a VACUUM portal if it fails after
		 * its first sub-transaction.)
		 */
		if (portal->createXact != xact &&
			(portal->cursorOptions & CURSOR_OPT_HOLD))
		{
			Assert(portal->status != PORTAL_ACTIVE);
			Assert(portal->resowner == NULL);
			continue;
		}

		/* Else zap it. */
		PortalDrop(portal, false);
	}
}

/*
 * Pre-subcommit processing for portals.
 *
 * Reassign the portals created in the current subtransaction to the parent
 * transaction.
 */
void
AtSubCommit_Portals(TransactionId parentXid,
					ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId curXid = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal	portal = hentry->portal;

		if (portal->createXact == curXid)
		{
			portal->createXact = parentXid;
			if (portal->resowner)
				ResourceOwnerNewParent(portal->resowner, parentXactOwner);
		}
	}
}

/*
 * Subtransaction abort handling for portals.
 *
 * Deactivate failed portals created during the failed subtransaction.
 * Note that per AtSubCommit_Portals, this will catch portals created
 * in descendants of the subtransaction too.
 */
void
AtSubAbort_Portals(TransactionId parentXid,
				   ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId curXid = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal	portal = hentry->portal;

		if (portal->createXact != curXid)
			continue;

		/*
		 * Force any active portals of my own transaction into FAILED state.
		 * This is mostly to ensure that a portal running a FETCH will go
		 * FAILED if the underlying cursor fails.  (Note we do NOT want to
		 * do this to upper-level portals, since they may be able to continue.)
		 */
 		if (portal->status == PORTAL_ACTIVE)
			portal->status = PORTAL_FAILED;

		/*
		 * If the portal is READY then allow it to survive into the
		 * parent transaction; otherwise shut it down.
		 */
		if (portal->status == PORTAL_READY)
		{
			portal->createXact = parentXid;
			if (portal->resowner)
				ResourceOwnerNewParent(portal->resowner, parentXactOwner);
		}
		else
		{
			/* let portalcmds.c clean up the state it knows about */
			if (PointerIsValid(portal->cleanup))
			{
				(*portal->cleanup) (portal);
				portal->cleanup = NULL;
			}
			/*
			 * Any resources belonging to the portal will be released in the
			 * upcoming transaction-wide cleanup; they will be gone before
			 * we run PortalDrop.
			 */
			portal->resowner = NULL;
		}
	}
}

/*
 * Post-subabort cleanup for portals.
 *
 * Drop all portals created in the failed subtransaction (but note that
 * we will not drop any that were reassigned to the parent above).
 */
void
AtSubCleanup_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId curXid = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createXact != curXid)
			continue;

		/* AtSubAbort_Portals should have fixed these: */
		Assert(portal->status != PORTAL_ACTIVE);
		Assert(portal->resowner == NULL);

		/* Zap it. */
		PortalDrop(portal, false);
	}
}
