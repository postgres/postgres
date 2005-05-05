/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/postinit.c,v 1.127.2.1 2005/05/05 19:53:49 tgl Exp $
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/file.h>
#include <math.h>
#include <unistd.h>

#include "catalog/catalog.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "commands/trigger.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/portal.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


static void ReverifyMyDatabase(const char *name);
static void InitCommunication(void);
static void ShutdownPostgres(void);
static bool ThereIsAtLeastOneUser(void);


/*** InitPostgres support ***/


/* --------------------------------
 *		ReverifyMyDatabase
 *
 * Since we are forced to fetch the database OID out of pg_database without
 * benefit of locking or transaction ID checking (see utils/misc/database.c),
 * we might have gotten a wrong answer.  Or, we might have attached to a
 * database that's in process of being destroyed by destroydb().  This
 * routine is called after we have all the locking and other infrastructure
 * running --- now we can check that we are really attached to a valid
 * database.
 *
 * In reality, if destroydb() is running in parallel with our startup,
 * it's pretty likely that we will have failed before now, due to being
 * unable to read some of the system tables within the doomed database.
 * This routine just exists to make *sure* we have not started up in an
 * invalid database.  If we quit now, we should have managed to avoid
 * creating any serious problems.
 *
 * This is also a handy place to fetch the database encoding info out
 * of pg_database.
 *
 * To avoid having to read pg_database more times than necessary
 * during session startup, this place is also fitting to set up any
 * database-specific configuration variables.
 * --------------------------------
 */
static void
ReverifyMyDatabase(const char *name)
{
	Relation	pgdbrel;
	HeapScanDesc pgdbscan;
	ScanKeyData key;
	HeapTuple	tup;
	Form_pg_database dbform;

	/*
	 * Because we grab AccessShareLock here, we can be sure that destroydb
	 * is not running in parallel with us (any more).
	 */
	pgdbrel = heap_openr(DatabaseRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&key, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(name));

	pgdbscan = heap_beginscan(pgdbrel, SnapshotNow, 1, &key);

	tup = heap_getnext(pgdbscan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup) ||
		HeapTupleGetOid(tup) != MyDatabaseId)
	{
		/* OOPS */
		heap_close(pgdbrel, AccessShareLock);

		/*
		 * The only real problem I could have created is to load dirty
		 * buffers for the dead database into shared buffer cache; if I
		 * did, some other backend will eventually try to write them and
		 * die in mdblindwrt.  Flush any such pages to forestall trouble.
		 */
		DropBuffers(MyDatabaseId);
		/* Now I can commit hara-kiri with a clear conscience... */
		ereport(FATAL,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\", OID %u, has disappeared from pg_database",
						name, MyDatabaseId)));
	}

	/*
	 * Also check that the database is currently allowing connections.
	 * (We do not enforce this in standalone mode, however, so that there is
	 * a way to recover from "UPDATE pg_database SET datallowconn = false;")
	 */
	dbform = (Form_pg_database) GETSTRUCT(tup);
	if (IsUnderPostmaster && !dbform->datallowconn)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		 errmsg("database \"%s\" is not currently accepting connections",
				name)));

	/*
	 * OK, we're golden.  Only other to-do item is to save the encoding
	 * info out of the pg_database tuple.
	 */
	SetDatabaseEncoding(dbform->encoding);
	/* Record it as a GUC internal option, too */
	SetConfigOption("server_encoding", GetDatabaseEncodingName(),
					PGC_INTERNAL, PGC_S_OVERRIDE);
	/* If we have no other source of client_encoding, use server encoding */
	SetConfigOption("client_encoding", GetDatabaseEncodingName(),
					PGC_BACKEND, PGC_S_DEFAULT);

	/*
	 * Set up database-specific configuration variables.
	 */
	if (IsUnderPostmaster)
	{
		Datum		datum;
		bool		isnull;

		datum = heap_getattr(tup, Anum_pg_database_datconfig,
							 RelationGetDescr(pgdbrel), &isnull);
		if (!isnull)
		{
			ArrayType  *a = DatumGetArrayTypeP(datum);

			ProcessGUCArray(a, PGC_S_DATABASE);
		}
	}

	heap_endscan(pgdbscan);
	heap_close(pgdbrel, AccessShareLock);
}



/* --------------------------------
 *		InitCommunication
 *
 *		This routine initializes stuff needed for ipc, locking, etc.
 *		it should be called something more informative.
 * --------------------------------
 */
static void
InitCommunication(void)
{
	/*
	 * initialize shared memory and semaphores appropriately.
	 */
	if (!IsUnderPostmaster)		/* postmaster already did this */
	{
		/*
		 * We're running a postgres bootstrap process or a standalone
		 * backend. Create private "shmem" and semaphores.
		 */
		CreateSharedMemoryAndSemaphores(true, MaxBackends, 0);
	}
}


/*
 * Early initialization of a backend (either standalone or under postmaster).
 * This happens even before InitPostgres.
 *
 * If you're wondering why this is separate from InitPostgres at all:
 * the critical distinction is that this stuff has to happen before we can
 * run XLOG-related initialization, which is done before InitPostgres --- in
 * fact, for cases such as checkpoint creation processes, InitPostgres may
 * never be done at all.
 */
void
BaseInit(void)
{
	/*
	 * Attach to shared memory and semaphores, and initialize our
	 * input/output/debugging file descriptors.
	 */
	InitCommunication();
	DebugFileOpen();

	/* Do local initialization of storage and buffer managers */
	smgrinit();
	InitBufferPoolAccess();
	InitLocalBuffer();
}


/* --------------------------------
 * InitPostgres
 *		Initialize POSTGRES.
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
void
InitPostgres(const char *dbname, const char *username)
{
	bool		bootstrap = IsBootstrapProcessingMode();

	/*
	 * Set up the global variables holding database id and path.
	 *
	 * We take a shortcut in the bootstrap case, otherwise we have to look up
	 * the db name in pg_database.
	 */
	if (bootstrap)
	{
		MyDatabaseId = TemplateDbOid;
		SetDatabasePath(GetDatabasePath(MyDatabaseId));
	}
	else
	{
		char	   *fullpath,
					datpath[MAXPGPATH];

		/*
		 * Formerly we validated DataDir here, but now that's done
		 * earlier.
		 */

		/*
		 * Find oid and path of the database we're about to open. Since
		 * we're not yet up and running we have to use the hackish
		 * GetRawDatabaseInfo.
		 */
		GetRawDatabaseInfo(dbname, &MyDatabaseId, datpath);

		if (!OidIsValid(MyDatabaseId))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist",
							dbname)));

		fullpath = GetDatabasePath(MyDatabaseId);

		/* Verify the database path */

		if (access(fullpath, F_OK) == -1)
		{
			if (errno == ENOENT)
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database \"%s\" does not exist",
								dbname),
				errdetail("The database subdirectory \"%s\" is missing.",
						  fullpath)));
			else
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not access directory \"%s\": %m",
								fullpath)));
		}

		ValidatePgVersion(fullpath);

		if (chdir(fullpath) == -1)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not change directory to \"%s\": %m",
							fullpath)));

		SetDatabasePath(fullpath);
	}

	/*
	 * Code after this point assumes we are in the proper directory!
	 */

	/*
	 * Set up my per-backend PGPROC struct in shared memory.	(We need
	 * to know MyDatabaseId before we can do this, since it's entered into
	 * the PGPROC struct.)
	 */
	InitProcess();

	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.  (Formerly this came before InitProcess, but now
	 * it must happen after, because it uses MyProc.)  Once I have done
	 * this, I am visible to other backends!
	 *
	 * Sets up MyBackendId, a unique backend identifier.
	 */
	MyBackendId = InvalidBackendId;

	InitBackendSharedInvalidationState();

	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad backend id: %d", MyBackendId);

	/*
	 * Initialize the transaction system override state.
	 */
	AmiTransactionOverride(bootstrap);

	/*
	 * Initialize the relation descriptor cache.  This must create at
	 * least the minimum set of "nailed-in" cache entries.	No catalog
	 * access happens here.
	 */
	RelationCacheInitialize();

	/*
	 * Initialize all the system catalog caches.  Note that no catalog
	 * access happens here; we only set up the cache structure.
	 */
	InitCatalogCache();

	/* Initialize portal manager */
	EnablePortalManager();

	/*
	 * Initialize the deferred trigger manager --- must happen before
	 * first transaction start.
	 */
	DeferredTriggerInit();

	/* start a new transaction here before access to db */
	if (!bootstrap)
		StartTransactionCommand();

	/*
	 * It's now possible to do real access to the system catalogs.
	 *
	 * Replace faked-up relcache entries with correct info.
	 */
	RelationCacheInitializePhase2();

	/*
	 * Figure out our postgres user id.  In standalone mode we use a fixed
	 * id, otherwise we figure it out from the authenticated user name.
	 */
	if (bootstrap)
		InitializeSessionUserIdStandalone();
	else if (!IsUnderPostmaster)
	{
		InitializeSessionUserIdStandalone();
		if (!ThereIsAtLeastOneUser())
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
				  errmsg("no users are defined in this database system"),
					 errhint("You should immediately run CREATE USER \"%s\" WITH SYSID %d CREATEUSER;.",
							 username, BOOTSTRAP_USESYSID)));
	}
	else
	{
		/* normal multiuser case */
		InitializeSessionUserId(username);
	}

	/*
	 * Unless we are bootstrapping, double-check that InitMyDatabaseInfo()
	 * got a correct result.  We can't do this until all the
	 * database-access infrastructure is up.
	 */
	if (!bootstrap)
		ReverifyMyDatabase(dbname);

	/*
	 * Final phase of relation cache startup: write a new cache file if
	 * necessary.  This is done after ReverifyMyDatabase to avoid writing
	 * a cache file into a dead database.
	 */
	RelationCacheInitializePhase3();

	/*
	 * Check a normal user hasn't connected to a superuser reserved slot.
	 * We can't do this till after we've read the user information, and we
	 * must do it inside a transaction since checking superuserness may
	 * require database access.  The superuser check is probably the most
	 * expensive part; don't do it until necessary.
	 */
	if (ReservedBackends > 0 &&
		CountEmptyBackendSlots() < ReservedBackends &&
		!superuser())
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("connection limit exceeded for non-superusers")));

	/*
	 * Initialize various default states that can't be set up until we've
	 * selected the active user and done ReverifyMyDatabase.
	 */

	/* set default namespace search path */
	InitializeSearchPath();

	/* initialize client encoding */
	InitializeClientEncoding();

	/*
	 * Now all default states are fully set up.  Report them to client if
	 * appropriate.
	 */
	BeginReportingGUCOptions();

	/*
	 * Set up process-exit callback to do pre-shutdown cleanup.  This
	 * should be last because we want shmem_exit to call this routine
	 * before the exit callbacks that are registered by buffer manager,
	 * lock manager, etc. We need to run this code before we close down
	 * database access!
	 */
	on_shmem_exit(ShutdownPostgres, 0);

	/* close the transaction we started above */
	if (!bootstrap)
		CommitTransactionCommand();
}

/*
 * Backend-shutdown callback.  Do cleanup that we want to be sure happens
 * before all the supporting modules begin to nail their doors shut via
 * their own callbacks.  Note that because this has to be registered very
 * late in startup, it will not get called if we suffer a failure *during*
 * startup.
 *
 * User-level cleanup, such as temp-relation removal and UNLISTEN, happens
 * via separate callbacks that execute before this one.  We don't combine the
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
static void
ShutdownPostgres(void)
{
	/*
	 * These operations are really just a minimal subset of
	 * AbortTransaction(). We don't want to do any inessential cleanup,
	 * since that just raises the odds of failure --- but there's some
	 * stuff we need to do.
	 *
	 * Release any LW locks and buffer context locks we might be holding.
	 * This is a kluge to improve the odds that we won't get into a
	 * self-made stuck-lock scenario while trying to shut down.
	 */
	LWLockReleaseAll();
	AbortBufferIO();
	UnlockBuffers();

	/*
	 * In case a transaction is open, delete any files it created.	This
	 * has to happen before bufmgr shutdown, so having smgr register a
	 * callback for it wouldn't work.
	 */
	smgrDoPendingDeletes(false);	/* delete as though aborting xact */
}



/*
 * Returns true if at least one user is defined in this database cluster.
 */
static bool
ThereIsAtLeastOneUser(void)
{
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapScanDesc scan;
	bool		result;

	pg_shadow_rel = heap_openr(ShadowRelationName, AccessExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, SnapshotNow, 0, NULL);
	result = (heap_getnext(scan, ForwardScanDirection) != NULL);

	heap_endscan(scan);
	heap_close(pg_shadow_rel, AccessExclusiveLock);

	return result;
}
