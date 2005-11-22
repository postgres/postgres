/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/init/postinit.c,v 1.158.2.1 2005/11/22 18:23:24 momjian Exp $
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/file.h>
#include <math.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "libpq/hba.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "storage/backendid.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/portal.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "pgstat.h"


static bool FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace);
static void ReverifyMyDatabase(const char *name);
static void InitCommunication(void);
static void ShutdownPostgres(int code, Datum arg);
static bool ThereIsAtLeastOneRole(void);


/*** InitPostgres support ***/


/*
 * FindMyDatabase -- get the critical info needed to locate my database
 *
 * Find the named database in pg_database, return its database OID and the
 * OID of its default tablespace.  Return TRUE if found, FALSE if not.
 *
 * Since we are not yet up and running as a backend, we cannot look directly
 * at pg_database (we can't obtain locks nor participate in transactions).
 * So to get the info we need before starting up, we must look at the "flat
 * file" copy of pg_database that is helpfully maintained by flatfiles.c.
 * This is subject to various race conditions, so after we have the
 * transaction infrastructure started, we have to recheck the information;
 * see ReverifyMyDatabase.
 */
static bool
FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace)
{
	bool		result = false;
	char	   *filename;
	FILE	   *db_file;
	char		thisname[NAMEDATALEN];
	TransactionId dummyxid;

	filename = database_getflatfilename();
	db_file = AllocateFile(filename, "r");
	if (db_file == NULL)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));

	while (read_pg_database_line(db_file, thisname, db_id,
								 db_tablespace, &dummyxid,
								 &dummyxid))
	{
		if (strcmp(thisname, name) == 0)
		{
			result = true;
			break;
		}
	}

	FreeFile(db_file);
	pfree(filename);

	return result;
}

/*
 * ReverifyMyDatabase -- recheck info obtained by FindMyDatabase
 *
 * Since FindMyDatabase cannot lock pg_database, the information it read
 * could be stale; for example we might have attached to a database that's in
 * process of being destroyed by dropdb().	This routine is called after
 * we have all the locking and other infrastructure running --- now we can
 * check that we are really attached to a valid database.
 *
 * In reality, if dropdb() is running in parallel with our startup,
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
 */
static void
ReverifyMyDatabase(const char *name)
{
	Relation	pgdbrel;
	SysScanDesc pgdbscan;
	ScanKeyData key;
	HeapTuple	tup;
	Form_pg_database dbform;

	/*
	 * Because we grab RowShareLock here, we can be sure that dropdb() is not
	 * running in parallel with us (any more).
	 */
	pgdbrel = heap_open(DatabaseRelationId, RowShareLock);

	ScanKeyInit(&key,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(name));

	pgdbscan = systable_beginscan(pgdbrel, DatabaseNameIndexId, true,
								  SnapshotNow, 1, &key);

	tup = systable_getnext(pgdbscan);
	if (!HeapTupleIsValid(tup) ||
		HeapTupleGetOid(tup) != MyDatabaseId)
	{
		/* OOPS */
		heap_close(pgdbrel, RowShareLock);

		/*
		 * The only real problem I could have created is to load dirty buffers
		 * for the dead database into shared buffer cache; if I did, some
		 * other backend will eventually try to write them and die in
		 * mdblindwrt.	Flush any such pages to forestall trouble.
		 */
		DropBuffers(MyDatabaseId);
		/* Now I can commit hara-kiri with a clear conscience... */
		ereport(FATAL,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
		  errmsg("database \"%s\", OID %u, has disappeared from pg_database",
				 name, MyDatabaseId)));
	}

	dbform = (Form_pg_database) GETSTRUCT(tup);

	/*
	 * These next checks are not enforced when in standalone mode, so that
	 * there is a way to recover from disabling all access to all databases,
	 * for example "UPDATE pg_database SET datallowconn = false;".
	 *
	 * We do not enforce them for the autovacuum process either.
	 */
	if (IsUnderPostmaster && !IsAutoVacuumProcess())
	{
		/*
		 * Check that the database is currently allowing connections.
		 */
		if (!dbform->datallowconn)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("database \"%s\" is not currently accepting connections",
					name)));

		/*
		 * Check connection limit for this database.
		 *
		 * There is a race condition here --- we create our PGPROC before
		 * checking for other PGPROCs.	If two backends did this at about the
		 * same time, they might both think they were over the limit, while
		 * ideally one should succeed and one fail.  Getting that to work
		 * exactly seems more trouble than it is worth, however; instead we
		 * just document that the connection limit is approximate.
		 */
		if (dbform->datconnlimit >= 0 &&
			!superuser() &&
			CountDBBackends(MyDatabaseId) > dbform->datconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for database \"%s\"",
							name)));
	}

	/*
	 * OK, we're golden.  Next to-do item is to save the encoding info out of
	 * the pg_database tuple.
	 */
	SetDatabaseEncoding(dbform->encoding);
	/* Record it as a GUC internal option, too */
	SetConfigOption("server_encoding", GetDatabaseEncodingName(),
					PGC_INTERNAL, PGC_S_OVERRIDE);
	/* If we have no other source of client_encoding, use server encoding */
	SetConfigOption("client_encoding", GetDatabaseEncodingName(),
					PGC_BACKEND, PGC_S_DEFAULT);

	/*
	 * Lastly, set up any database-specific configuration variables.
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

	systable_endscan(pgdbscan);
	heap_close(pgdbrel, RowShareLock);
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
		 * We're running a postgres bootstrap process or a standalone backend.
		 * Create private "shmem" and semaphores.
		 */
		CreateSharedMemoryAndSemaphores(true, 0);
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

	/* Do local initialization of file, storage and buffer managers */
	InitFileAccess();
	smgrinit();
	InitBufferPoolAccess();
}


/* --------------------------------
 * InitPostgres
 *		Initialize POSTGRES.
 *
 * In bootstrap mode neither of the parameters are used.  In autovacuum
 * mode, the username parameter is not used.
 *
 * The return value indicates whether the userID is a superuser.  (That
 * can only be tested inside a transaction, so we want to do it during
 * the startup transaction rather than doing a separate one in postgres.c.)
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
bool
InitPostgres(const char *dbname, const char *username)
{
	bool		bootstrap = IsBootstrapProcessingMode();
	bool		autovacuum = IsAutoVacuumProcess();
	bool		am_superuser;

	/*
	 * Set up the global variables holding database id and path.
	 *
	 * We take a shortcut in the bootstrap case, otherwise we have to look up
	 * the db name in pg_database.
	 */
	if (bootstrap)
	{
		MyDatabaseId = TemplateDbOid;
		MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
		SetDatabasePath(GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace));
	}
	else
	{
		char	   *fullpath;

		/*
		 * Formerly we validated DataDir here, but now that's done earlier.
		 */

		/*
		 * Find oid and tablespace of the database we're about to open. Since
		 * we're not yet up and running we have to use the hackish
		 * FindMyDatabase.
		 */
		if (!FindMyDatabase(dbname, &MyDatabaseId, &MyDatabaseTableSpace))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist",
							dbname)));

		fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

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

		SetDatabasePath(fullpath);
	}

	/*
	 * Code after this point assumes we are in the proper directory!
	 */

	/*
	 * Set up my per-backend PGPROC struct in shared memory.	(We need to
	 * know MyDatabaseId before we can do this, since it's entered into the
	 * PGPROC struct.)
	 */
	InitProcess();

	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.  (Formerly this came before InitProcess, but now it
	 * must happen after, because it uses MyProc.)	Once I have done this, I
	 * am visible to other backends!
	 *
	 * Sets up MyBackendId, a unique backend identifier.
	 */
	MyBackendId = InvalidBackendId;

	InitBackendSharedInvalidationState();

	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad backend id: %d", MyBackendId);

	/*
	 * bufmgr needs another initialization call too
	 */
	InitBufferPoolBackend();

	/*
	 * Initialize local process's access to XLOG.  In bootstrap case we may
	 * skip this since StartupXLOG() was run instead.
	 */
	if (!bootstrap)
		InitXLOGAccess();

	/*
	 * Initialize the relation descriptor cache.  This must create at least
	 * the minimum set of "nailed-in" cache entries.  No catalog access
	 * happens here.
	 */
	RelationCacheInitialize();

	/*
	 * Initialize all the system catalog caches.  Note that no catalog access
	 * happens here; we only set up the cache structure.
	 */
	InitCatalogCache();

	/* Initialize portal manager */
	EnablePortalManager();

	/*
	 * Set up process-exit callback to do pre-shutdown cleanup.  This has to
	 * be after we've initialized all the low-level modules like the buffer
	 * manager, because during shutdown this has to run before the low-level
	 * modules start to close down.  On the other hand, we want it in place
	 * before we begin our first transaction --- if we fail during the
	 * initialization transaction, as is entirely possible, we need the
	 * AbortTransaction call to clean up.
	 */
	on_shmem_exit(ShutdownPostgres, 0);

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
	 * Figure out our postgres user id.  In standalone mode and in the
	 * autovacuum process, we use a fixed id, otherwise we figure it out from
	 * the authenticated user name.
	 */
	if (bootstrap || autovacuum)
		InitializeSessionUserIdStandalone();
	else if (!IsUnderPostmaster)
	{
		InitializeSessionUserIdStandalone();
		if (!ThereIsAtLeastOneRole())
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no roles are defined in this database system"),
					 errhint("You should immediately run CREATE USER \"%s\" CREATEUSER;.",
							 username)));
	}
	else
	{
		/* normal multiuser case */
		InitializeSessionUserId(username);
	}

	/*
	 * Unless we are bootstrapping, double-check that InitMyDatabaseInfo() got
	 * a correct result.  We can't do this until all the database-access
	 * infrastructure is up.  (Also, it wants to know if the user is a
	 * superuser, so the above stuff has to happen first.)
	 */
	if (!bootstrap)
		ReverifyMyDatabase(dbname);

	/*
	 * Final phase of relation cache startup: write a new cache file if
	 * necessary.  This is done after ReverifyMyDatabase to avoid writing a
	 * cache file into a dead database.
	 */
	RelationCacheInitializePhase3();

	/*
	 * Check if user is a superuser.
	 */
	if (bootstrap || autovacuum)
		am_superuser = true;
	else
		am_superuser = superuser();

	/*
	 * Check a normal user hasn't connected to a superuser reserved slot.
	 */
	if (!am_superuser &&
		ReservedBackends > 0 &&
		!HaveNFreeProcs(ReservedBackends))
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("connection limit exceeded for non-superusers")));

	/*
	 * Initialize various default states that can't be set up until we've
	 * selected the active user and done ReverifyMyDatabase.
	 */

	/* set default namespace search path */
	InitializeSearchPath();

	/* set up ACL framework (currently just sets RolMemCache callback) */
	initialize_acl();

	/* initialize client encoding */
	InitializeClientEncoding();

	/* initialize statistics collection for this backend */
	if (IsUnderPostmaster)
		pgstat_bestart();

	/* close the transaction we started above */
	if (!bootstrap)
		CommitTransactionCommand();

	return am_superuser;
}


/*
 * Backend-shutdown callback.  Do cleanup that we want to be sure happens
 * before all the supporting modules begin to nail their doors shut via
 * their own callbacks.
 *
 * User-level cleanup, such as temp-relation removal and UNLISTEN, happens
 * via separate callbacks that execute before this one.  We don't combine the
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
static void
ShutdownPostgres(int code, Datum arg)
{
	/* Make sure we've killed any active transaction */
	AbortOutOfAnyTransaction();

	/*
	 * User locks are not released by transaction end, so be sure to release
	 * them explicitly.
	 */
#ifdef USER_LOCKS
	LockReleaseAll(USER_LOCKMETHOD, true);
#endif
}


/*
 * Returns true if at least one role is defined in this database cluster.
 */
static bool
ThereIsAtLeastOneRole(void)
{
	Relation	pg_authid_rel;
	HeapScanDesc scan;
	bool		result;

	pg_authid_rel = heap_open(AuthIdRelationId, AccessExclusiveLock);

	scan = heap_beginscan(pg_authid_rel, SnapshotNow, 0, NULL);
	result = (heap_getnext(scan, ForwardScanDirection) != NULL);

	heap_endscan(scan);
	heap_close(pg_authid_rel, AccessExclusiveLock);

	return result;
}
