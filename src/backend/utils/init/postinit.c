/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/init/postinit.c,v 1.172.2.1 2008/09/11 14:01:16 alvherre Exp $
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
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
#include "utils/guc.h"
#include "utils/portal.h"
#include "utils/relcache.h"
#include "utils/tqual.h"
#include "utils/syscache.h"
#include "pgstat.h"


static bool FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace);
static void CheckMyDatabase(const char *name, bool am_superuser);
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
 * see InitPostgres.
 */
static bool
FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace)
{
	bool		result = false;
	char	   *filename;
	FILE	   *db_file;
	char		thisname[NAMEDATALEN];
	TransactionId db_frozenxid;

	filename = database_getflatfilename();
	db_file = AllocateFile(filename, "r");
	if (db_file == NULL)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));

	while (read_pg_database_line(db_file, thisname, db_id,
								 db_tablespace, &db_frozenxid))
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
 * CheckMyDatabase -- fetch information from the pg_database entry for our DB
 */
static void
CheckMyDatabase(const char *name, bool am_superuser)
{
	HeapTuple	tup;
	Form_pg_database dbform;

	/* Fetch our real pg_database row */
	tup = SearchSysCache(DATABASEOID,
						 ObjectIdGetDatum(MyDatabaseId),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbform = (Form_pg_database) GETSTRUCT(tup);

	/* This recheck is strictly paranoia */
	if (strcmp(name, NameStr(dbform->datname)) != 0)
		ereport(FATAL,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" has disappeared from pg_database",
						name),
				 errdetail("Database OID %u now seems to belong to \"%s\".",
						   MyDatabaseId, NameStr(dbform->datname))));

	/*
	 * Check permissions to connect to the database.
	 *
	 * These checks are not enforced when in standalone mode, so that there is
	 * a way to recover from disabling all access to all databases, for
	 * example "UPDATE pg_database SET datallowconn = false;".
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
		 * Check privilege to connect to the database.	(The am_superuser test
		 * is redundant, but since we have the flag, might as well check it
		 * and save a few cycles.)
		 */
		if (!am_superuser &&
			pg_database_aclcheck(MyDatabaseId, GetUserId(),
								 ACL_CONNECT) != ACLCHECK_OK)
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for database \"%s\"", name),
					 errdetail("User does not have CONNECT privilege.")));

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
			!am_superuser &&
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

		datum = SysCacheGetAttr(DATABASEOID, tup, Anum_pg_database_datconfig,
								&isnull);
		if (!isnull)
		{
			ArrayType  *a = DatumGetArrayTypeP(datum);

			ProcessGUCArray(a, PGC_S_DATABASE);
		}
	}

	ReleaseSysCache(tup);
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
 * As of PostgreSQL 8.2, we expect InitProcess() was already called, so we
 * already have a PGPROC struct ... but it's not filled in yet.
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
	char	   *fullpath;

	/*
	 * Set up the global variables holding database id and path.  But note we
	 * won't actually try to touch the database just yet.
	 *
	 * We take a shortcut in the bootstrap case, otherwise we have to look up
	 * the db name in pg_database.
	 */
	if (bootstrap)
	{
		MyDatabaseId = TemplateDbOid;
		MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
	}
	else
	{
		/*
		 * Find oid and tablespace of the database we're about to open. Since
		 * we're not yet up and running we have to use the hackish
		 * FindMyDatabase, which looks in the flat-file copy of pg_database.
		 */
		if (!FindMyDatabase(dbname, &MyDatabaseId, &MyDatabaseTableSpace))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist",
							dbname)));
	}

	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

	SetDatabasePath(fullpath);

	/*
	 * Finish filling in the PGPROC struct, and add it to the ProcArray. (We
	 * need to know MyDatabaseId before we can do this, since it's entered
	 * into the PGPROC struct.)
	 *
	 * Once I have done this, I am visible to other backends!
	 */
	InitProcessPhase2();

	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.
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
	 * Initialize the relation cache and the system catalog caches.  Note that
	 * no catalog access happens here; we only set up the hashtable structure.
	 * We must do this before starting a transaction because transaction abort
	 * would try to touch these hashtables.
	 */
	RelationCacheInitialize();
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

	/*
	 * Start a new transaction here before first access to db, and get a
	 * snapshot.  We don't have a use for the snapshot itself, but we're
	 * interested in the secondary effect that it sets RecentGlobalXmin.
	 */
	if (!bootstrap)
	{
		StartTransactionCommand();
		(void) GetTransactionSnapshot();
	}

	/*
	 * Now that we have a transaction, we can take locks.  Take a writer's
	 * lock on the database we are trying to connect to.  If there is a
	 * concurrently running DROP DATABASE on that database, this will block us
	 * until it finishes (and has updated the flat file copy of pg_database).
	 *
	 * Note that the lock is not held long, only until the end of this startup
	 * transaction.  This is OK since we are already advertising our use of
	 * the database in the PGPROC array; anyone trying a DROP DATABASE after
	 * this point will see us there.
	 *
	 * Note: use of RowExclusiveLock here is reasonable because we envision
	 * our session as being a concurrent writer of the database.  If we had a
	 * way of declaring a session as being guaranteed-read-only, we could use
	 * AccessShareLock for such sessions and thereby not conflict against
	 * CREATE DATABASE.
	 */
	if (!bootstrap)
		LockSharedObject(DatabaseRelationId, MyDatabaseId, 0,
						 RowExclusiveLock);

	/*
	 * Recheck the flat file copy of pg_database to make sure the target
	 * database hasn't gone away.  If there was a concurrent DROP DATABASE,
	 * this ensures we will die cleanly without creating a mess.
	 */
	if (!bootstrap)
	{
		Oid			dbid2;
		Oid			tsid2;

		if (!FindMyDatabase(dbname, &dbid2, &tsid2) ||
			dbid2 != MyDatabaseId || tsid2 != MyDatabaseTableSpace)
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist",
							dbname),
			   errdetail("It seems to have just been dropped or renamed.")));
	}

	/*
	 * Now we should be able to access the database directory safely. Verify
	 * it's there and looks reasonable.
	 */
	if (!bootstrap)
	{
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
	}

	/*
	 * It's now possible to do real access to the system catalogs.
	 *
	 * Load relcache entries for the system catalogs.  This must create at
	 * least the minimum set of "nailed-in" cache entries.
	 */
	RelationCacheInitializePhase2();

	/*
	 * Figure out our postgres user id, and see if we are a superuser.
	 *
	 * In standalone mode and in the autovacuum process, we use a fixed id,
	 * otherwise we figure it out from the authenticated user name.
	 */
	if (bootstrap || autovacuum)
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
	}
	else if (!IsUnderPostmaster)
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
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
		am_superuser = superuser();
	}

	/* set up ACL framework (so CheckMyDatabase can check permissions) */
	initialize_acl();

	/*
	 * Read the real pg_database row for our database, check permissions and
	 * set up database-specific GUC settings.  We can't do this until all the
	 * database-access infrastructure is up.  (Also, it wants to know if the
	 * user is a superuser, so the above stuff has to happen first.)
	 */
	if (!bootstrap)
		CheckMyDatabase(dbname, am_superuser);

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
	 * selected the active user and gotten the right GUC settings.
	 */

	/* set default namespace search path */
	InitializeSearchPath();

	/* initialize client encoding */
	InitializeClientEncoding();

	/* initialize statistics collection for this backend */
	if (!bootstrap)
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
	LockReleaseAll(USER_LOCKMETHOD, true);
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

	pg_authid_rel = heap_open(AuthIdRelationId, AccessShareLock);

	scan = heap_beginscan(pg_authid_rel, SnapshotNow, 0, NULL);
	result = (heap_getnext(scan, ForwardScanDirection) != NULL);

	heap_endscan(scan);
	heap_close(pg_authid_rel, AccessShareLock);

	return result;
}
