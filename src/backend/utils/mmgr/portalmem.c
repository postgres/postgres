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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/portalmem.c,v 1.52 2003/03/10 03:53:51 tgl Exp $
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
 *				fetch 1 from FOO
 *		the system looks up the portal named "FOO" in the portal table,
 *		gets the planned query and then calls the executor with a count
 *		of 1.  The executor then runs the query and returns a single
 *		tuple.	The problem is that we have to hold onto the state of the
 *		portal query until we see a "close".  This means we have to be
 *		careful about memory management.
 *
 *		I hope this makes things clearer to whoever reads this -cim 2/22/91
 */

#include "postgres.h"

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
	 * use PORTALS_PER_USER, defined in utils/portal.h as a guess of how
	 * many hash table entries to create, initially
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
 *		Attaches a "query" to portal.
 */
void
PortalSetQuery(Portal portal,
			   QueryDesc *queryDesc,
			   void (*cleanup) (Portal portal))
{
	AssertArg(PortalIsValid(portal));

	portal->queryDesc = queryDesc;
	portal->backwardOK = ExecSupportsBackwardScan(queryDesc->plantree);
	portal->atStart = true;		/* Allow fetch forward only, to start */
	portal->atEnd = false;
	portal->cleanup = cleanup;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal name is invalid.
 *		"WARNING" if portal name is in use (existing portal is returned!)
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
	portal->backwardOK = false;
	portal->atStart = true;		/* disallow fetches until query is set */
	portal->atEnd = true;
	portal->cleanup = NULL;

	/* put portal in table */
	PortalHashTableInsert(portal);

	return portal;
}

/*
 * PortalDrop
 *		Destroys portal.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if portal is invalid.
 */
void
PortalDrop(Portal portal)
{
	AssertArg(PortalIsValid(portal));

	/* remove portal from hash table */
	PortalHashTableDelete(portal);

	/* reset portal */
	if (PointerIsValid(portal->cleanup))
		(*portal->cleanup) (portal);

	/* release subsidiary storage */
	MemoryContextDelete(PortalGetHeapMemory(portal));

	/* release name and portal data (both are in PortalMemory) */
	pfree(portal->name);
	pfree(portal);
}

/*
 * Destroy all portals created in the current transaction (ie, all of them).
 *
 * XXX This assumes that portals can be deleted in a random order, ie,
 * no portal has a reference to any other (at least not one that will be
 * exercised during deletion).	I think this is okay at the moment, but
 * we've had bugs of that ilk in the past.  Keep a close eye on cursor
 * references...
 */
void
AtEOXact_portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
		PortalDrop(hentry->portal);
}
