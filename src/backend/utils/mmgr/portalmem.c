/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory context management stuff
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/portalmem.c,v 1.55 2003/04/29 03:21:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * NOTES
 *		A "Portal" is a structure used to keep track of cursor queries.
 *
 *		When the backend sees a "declare cursor" query, it allocates a
 *		"PortalData" structure, plans the query and then stores the query
 *		in the portal without executing it.  Later, when the backend
 *		sees a
 *				fetch 1 from foo
 *		the system looks up the portal named "foo" in the portal table,
 *		gets the planned query and then calls the executor with a count
 *		of 1.  The executor then runs the query and returns a single
 *		tuple.	The problem is that we have to hold onto the state of the
 *		portal query until we see a "close".  This means we have to be
 *		careful about memory management.
 *
 *		I hope this makes things clearer to whoever reads this -cim 2/22/91
 */

#include "postgres.h"

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
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", NAME); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_FIND, NULL); \
	if (hentry) \
		PORTAL = hentry->portal; \
	else \
		PORTAL = NULL; \
} while(0)

#define PortalHashTableInsert(PORTAL) \
do { \
	PortalHashEnt *hentry; bool found; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", PORTAL->name); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_ENTER, &found); \
	if (hentry == NULL) \
		elog(ERROR, "out of memory in PortalHashTable"); \
	if (found) \
		elog(WARNING, "trying to insert a portal name that exists."); \
	hentry->portal = PORTAL; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
do { \
	PortalHashEnt *hentry; char key[MAX_PORTALNAME_LEN]; \
	\
	MemSet(key, 0, MAX_PORTALNAME_LEN); \
	snprintf(key, MAX_PORTALNAME_LEN - 1, "%s", PORTAL->name); \
	hentry = (PortalHashEnt*)hash_search(PortalHashTable, \
										 key, HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "trying to delete portal name that does not exist."); \
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
 * PortalSetQuery
 *		Attaches a QueryDesc to the specified portal.  This should be
 *		called only after successfully doing ExecutorStart for the query.
 *
 * Note that in the case of DECLARE CURSOR, some Portal options have
 * already been set in portalcmds.c's PreparePortal().  This is grotty.
 */
void
PortalSetQuery(Portal portal, QueryDesc *queryDesc)
{
	AssertArg(PortalIsValid(portal));

	/*
	 * If the user didn't specify a SCROLL type, allow or disallow
	 * scrolling based on whether it would require any additional
	 * runtime overhead to do so.
	 */
	if (portal->scrollType == DEFAULT_SCROLL)
	{
		if (ExecSupportsBackwardScan(queryDesc->plantree))
			portal->scrollType = ENABLE_SCROLL;
		else
			portal->scrollType = DISABLE_SCROLL;
	}

	portal->queryDesc = queryDesc;
	portal->executorRunning = true;	/* now need to shut down executor */

	portal->atStart = true;
	portal->atEnd = false;		/* allow fetches */
	portal->portalPos = 0;
	portal->posOverflow = false;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 *		An elog(WARNING) is emitted if portal name is in use (existing
 *		portal is returned!)
 */
Portal
CreatePortal(const char *name)
{
	Portal		portal;

	AssertArg(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		elog(WARNING, "CreatePortal: portal \"%s\" already exists", name);
		return portal;
	}

	/* make new portal structure */
	portal = (Portal) MemoryContextAlloc(PortalMemory, sizeof *portal);

	/* initialize portal name */
	portal->name = MemoryContextStrdup(PortalMemory, name);

	/* initialize portal heap context */
	portal->heap = AllocSetContextCreate(PortalMemory,
										 "PortalHeapMemory",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	/* initialize portal query */
	portal->queryDesc = NULL;
	portal->cleanup = PortalCleanup;
	portal->scrollType = DEFAULT_SCROLL;
	portal->executorRunning = false;
	portal->holdOpen = false;
	portal->createXact = GetCurrentTransactionId();
	portal->holdStore = NULL;
	portal->holdContext = NULL;
	portal->atStart = true;
	portal->atEnd = true;		/* disallow fetches until query is set */
	portal->portalPos = 0;
	portal->posOverflow = false;

	/* put portal in table */
	PortalHashTableInsert(portal);

	return portal;
}

/*
 * PortalDrop
 *		Destroy the portal.
 *
 *		isError: if true, we are destroying portals at the end of a failed
 *		transaction.  (This causes PortalCleanup to skip unneeded steps.)
 */
void
PortalDrop(Portal portal, bool isError)
{
	AssertArg(PortalIsValid(portal));

	/*
	 * Remove portal from hash table.  Because we do this first, we will
	 * not come back to try to remove the portal again if there's any error
	 * in the subsequent steps.  Better to leak a little memory than to get
	 * into an infinite error-recovery loop.
	 */
	PortalHashTableDelete(portal);

	/* let portalcmds.c clean up the state it knows about */
	if (PointerIsValid(portal->cleanup))
		(*portal->cleanup) (portal, isError);

	/* delete tuplestore storage, if any */
	if (portal->holdContext)
		MemoryContextDelete(portal->holdContext);

	/* release subsidiary storage */
	if (PortalGetHeapMemory(portal))
		MemoryContextDelete(PortalGetHeapMemory(portal));

	/* release name and portal data (both are in PortalMemory) */
	pfree(portal->name);
	pfree(portal);
}

/*
 * Cleanup the portals created in the current transaction. If the
 * transaction was aborted, all the portals created in this transaction
 * should be removed. If the transaction was successfully committed, any
 * holdable cursors created in this transaction need to be kept
 * open. In any case, portals remaining from prior transactions should
 * be left untouched.
 *
 * XXX This assumes that portals can be deleted in a random order, ie,
 * no portal has a reference to any other (at least not one that will be
 * exercised during deletion).	I think this is okay at the moment, but
 * we've had bugs of that ilk in the past.  Keep a close eye on cursor
 * references...
 */
void
AtEOXact_portals(bool isCommit)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	TransactionId xact = GetCurrentTransactionId();

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal portal = hentry->portal;

		if (portal->createXact != xact)
			continue;

		if (portal->holdOpen && isCommit)
		{
			/*
			 * We are exiting the transaction that created a holdable
			 * cursor.  Instead of dropping the portal, prepare it for
			 * access by later transactions.
			 */

			/*
			 * Create the memory context that is used for storage of
			 * the held cursor's tuple set.
			 */
			portal->holdContext =
				AllocSetContextCreate(PortalMemory,
									  "PortalHeapMemory",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

			/*
			 * Transfer data into the held tuplestore.
			 *
			 * Note that PersistHoldablePortal() must release all
			 * resources used by the portal that are local to the creating
			 * transaction.
			 */
			PersistHoldablePortal(portal);
		}
		else
		{
			PortalDrop(portal, !isCommit);
		}
	}
}
