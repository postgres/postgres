/*-------------------------------------------------------------------------
 *
 * postinit.c--
 *	  postgres initialization utilities
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/postinit.c,v 1.32 1998/07/26 04:31:01 scrappy Exp $
 *
 * NOTES
 *		InitPostgres() is the function called from PostgresMain
 *		which does all non-trival initialization, mainly by calling
 *		all the other initialization functions.  InitPostgres()
 *		is only used within the "postgres" backend and so that routine
 *		is in tcop/postgres.c  InitPostgres() is needed in cinterface.a
 *		because things like the bootstrap backend program need it. Hence
 *		you find that in this file...
 *
 *		If you feel the need to add more initialization code, it should be
 *		done in InitPostgres() or someplace lower.	Do not start
 *		putting stuff in PostgresMain - if you do then someone
 *		will have to clean it up later, and it's not going to be me!
 *		-cim 10/3/90
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <math.h>
#include <unistd.h>

#include "postgres.h"
#include "version.h"

#include <storage/ipc.h>
#include <storage/backendid.h>
#include <storage/buf_internals.h>
#include <storage/smgr.h>
#include <storage/proc.h>
#include <utils/relcache.h>

#include "access/heapam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "access/transam.h"		/* XXX dependency problem */
#include "utils/syscache.h"
#include "storage/bufpage.h"	/* for page layout, for
								 * InitMyDatabaseInfo() */
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "storage/lmgr.h"

#include "miscadmin.h"			/* for global decls */
#include "utils/portal.h"		/* for EnablePortalManager, etc. */

#include "utils/exc.h"			/* for EnableExceptionHandling, etc. */
#include "fmgr.h"				/* for EnableDynamicFunctionManager, etc. */
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/mcxt.h"			/* for EnableMemoryContext, etc. */
#include "utils/inval.h"

#include "catalog/catname.h"
#ifdef MULTIBYTE
#include "catalog/pg_database_mb.h"
#include "mb/pg_wchar.h"
#else
#include "catalog/pg_database.h"
#endif

#include "libpq/libpq.h"

static void VerifySystemDatabase(void);
static void VerifyMyDatabase(void);
static void InitCommunication(void);
static void InitMyDatabaseInfo(char *name);
static void InitStdio(void);
static void InitUserid(void);

extern char *ExpandDatabasePath(char *name);
#ifdef MULTIBYTE
extern void GetRawDatabaseInfo(char *name, Oid *owner, Oid *db_id, char *path, int *encoding);
#else
extern void GetRawDatabaseInfo(char *name, Oid *owner, Oid *db_id, char *path);
#endif

static IPCKey PostgresIpcKey;

/* ----------------------------------------------------------------
 *						InitPostgres support
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *	InitMyDatabaseInfo() -- Find and record the OID of the database we are
 *						  to open.
 *
 *		The database's oid forms half of the unique key for the system
 *		caches and lock tables.  We therefore want it initialized before
 *		we open any relations, since opening relations puts things in the
 *		cache.	To get around this problem, this code opens and scans the
 *		pg_database relation by hand.
 *
 *		This algorithm relies on the fact that first attribute in the
 *		pg_database relation schema is the database name.  It also knows
 *		about the internal format of tuples on disk and the length of
 *		the datname attribute.	It knows the location of the pg_database
 *		file.
 *		Actually, the code looks as though it is using the pg_database
 *		tuple definition to locate the database name, so the above statement
 *		seems to be no longer correct. - thomas 1997-11-01
 *
 *		This code is called from InitPostgres(), before we chdir() to the
 *		local database directory and before we open any relations.
 *		Used to be called after the chdir(), but we now want to confirm
 *		the location of the target database using pg_database info.
 *		- thomas 1997-11-01
 * --------------------------------
 */
static void
InitMyDatabaseInfo(char *name)
{
	Oid			owner;
	char	   *path,
				myPath[MAXPGPATH + 1];
#ifdef MULTIBYTE
	int encoding;
#endif

	SetDatabaseName(name);
#ifdef MULTIBYTE
	GetRawDatabaseInfo(name, &owner, &MyDatabaseId, myPath, &encoding);
#else
	GetRawDatabaseInfo(name, &owner, &MyDatabaseId, myPath);
#endif

	if (!OidIsValid(MyDatabaseId))
		elog(FATAL,
			 "Database %s does not exist in %s",
			 DatabaseName,
			 DatabaseRelationName);

	path = ExpandDatabasePath(myPath);
	SetDatabasePath(path);
#ifdef MULTIBYTE
	SetDatabaseEncoding(encoding);
#endif

	return;
}	/* InitMyDatabaseInfo() */


/*
 * DoChdirAndInitDatabaseNameAndPath --
 *		Set current directory to the database directory for the database
 *		named <name>.
 *		Also set global variables DatabasePath and DatabaseName to those
 *		values.  Also check for proper version of database system and
 *		database.  Exit program via elog() if anything doesn't check out.
 *
 * Arguments:
 *		Path and name are invalid if it invalid as a string.
 *		Path is "badly formatted" if it is not a string containing a path
 *		to a writable directory.
 *		Name is "badly formatted" if it contains more than 16 characters or if
 *		it is a bad file name (e.g., it contains a '/' or an 8-bit character).
 *
 * Exceptions:
 *		BadState if called more than once.
 *		BadArg if both path and name are "badly formatted" or invalid.
 *		BadArg if path and name are both "inconsistent" and valid.
 *
 *		This routine is inappropriate in bootstrap mode, since the directories
 *		and version files need not exist yet if we're in bootstrap mode.
 */
static void
VerifySystemDatabase()
{
	char	   *reason;

	/* Failure reason returned by some function.  NULL if no failure */
	int			fd;
	char		errormsg[1000];

	errormsg[0] = '\0';

	if ((fd = open(DataDir, O_RDONLY, 0)) == -1)
		sprintf(errormsg, "Database system does not exist.  "
				"PGDATA directory '%s' not found.\n\tNormally, you "
				"create a database system by running initdb.",
				DataDir);
	else
	{
		close(fd);
		ValidatePgVersion(DataDir, &reason);
		if (reason != NULL)
			sprintf(errormsg,
					"InitPostgres could not validate that the database"
					" system version is compatible with this level of"
					" Postgres.\n\tYou may need to run initdb to create"
					" a new database system.\n\t%s", reason);
	}
	if (errormsg[0] != '\0')
		elog(FATAL, errormsg);
	/* Above does not return */
}	/* VerifySystemDatabase() */


static void
VerifyMyDatabase()
{
	const char	   *name;
	const char	   *myPath;

	/* Failure reason returned by some function.  NULL if no failure */
	char	   *reason;
	int			fd;
	char		errormsg[1000];

	name = DatabaseName;
	myPath = DatabasePath;

	if ((fd = open(myPath, O_RDONLY, 0)) == -1)
		sprintf(errormsg,
				"Database '%s' does not exist."
			"\n\tWe know this because the directory '%s' does not exist."
				"\n\tYou can create a database with the SQL command"
				" CREATE DATABASE.\n\tTo see what databases exist,"
				" look at the subdirectories of '%s/base/'.",
				name, myPath, DataDir);
	else
	{
		close(fd);
		ValidatePgVersion(myPath, &reason);
		if (reason != NULL)
			sprintf(errormsg,
					"InitPostgres could not validate that the database"
					" version is compatible with this level of Postgres"
					"\n\teven though the database system as a whole"
					" appears to be at a compatible level."
					"\n\tYou may need to recreate the database with SQL"
					" commands DROP DATABASE and CREATE DATABASE."
					"\n\t%s", reason);
		else
		{

			/*
			 * The directories and PG_VERSION files are in order.
			 */
			int			rc;		/* return code from some function we call */

#ifdef FILEDEBUG
			printf("Try changing directory for database %s to %s\n", name, myPath);
#endif

			rc = chdir(myPath);
			if (rc < 0)
				sprintf(errormsg,
						"InitPostgres unable to change "
						"current directory to '%s', errno = %s (%d).",
						myPath, strerror(errno), errno);
			else
				errormsg[0] = '\0';
		}
	}

	if (errormsg[0] != '\0')
		elog(FATAL, errormsg);
	/* Above does not return */
}	/* VerifyMyDatabase() */


/* --------------------------------
 *		InitUserid
 *
 *		initializes crap associated with the user id.
 * --------------------------------
 */
static void
InitUserid()
{
	setuid(geteuid());
	SetUserId();
}

/* --------------------------------
 *		InitCommunication
 *
 *		This routine initializes stuff needed for ipc, locking, etc.
 *		it should be called something more informative.
 *
 * Note:
 *		This does not set MyBackendId.	MyBackendTag is set, however.
 * --------------------------------
 */
static void
InitCommunication()
{
	char	   *postid;			/* value of environment variable */
	char	   *postport;		/* value of environment variable */
	char	   *ipc_key;		/* value of environemnt variable */
	IPCKey		key = 0;

	/* ----------------
	 *	try and get the backend tag from POSTID
	 * ----------------
	 */
	MyBackendId = -1;

	postid = getenv("POSTID");
	if (!PointerIsValid(postid))
		MyBackendTag = -1;
	else
	{
		MyBackendTag = atoi(postid);
		Assert(MyBackendTag >= 0);
	}


	ipc_key = getenv("IPC_KEY");
	if (!PointerIsValid(ipc_key))
		key = -1;
	else
	{
		key = atoi(ipc_key);
		Assert(MyBackendTag >= 0);
	}

	postport = getenv("POSTPORT");

	if (PointerIsValid(postport))
	{
		if (MyBackendTag == -1)
			elog(FATAL, "InitCommunication: missing POSTID");

		/*
		 * Enable this if you are trying to force the backend to run as if
		 * it is running under the postmaster.
		 *
		 * This goto forces Postgres to attach to shared memory instead of
		 * using malloc'ed memory (which is the normal behavior if run
		 * directly).
		 *
		 * To enable emulation, run the following shell commands (in addition
		 * to enabling this goto)
		 *
		 * % setenv POSTID 1 % setenv POSTPORT 4321 % setenv IPC_KEY 4321000
		 * % postmaster & % kill -9 %1
		 *
		 * Upon doing this, Postmaster will have allocated the shared memory
		 * resources that Postgres will attach to if you enable
		 * EMULATE_UNDER_POSTMASTER.
		 *
		 * This comment may well age with time - it is current as of 8
		 * January 1990
		 *
		 * Greg
		 */

#ifdef EMULATE_UNDER_POSTMASTER

		goto forcesharedmemory;

#endif

	}
	else if (IsUnderPostmaster)
	{
		elog(FATAL,
			 "InitCommunication: under postmaster and POSTPORT not set");
	}
	else
	{
		/* ----------------
		 *	assume we're running a postgres backend by itself with
		 *	no front end or postmaster.
		 * ----------------
		 */
		if (MyBackendTag == -1)
			MyBackendTag = 1;

		key = PrivateIPCKey;
	}

	/* ----------------
	 *	initialize shared memory and semaphores appropriately.
	 * ----------------
	 */
#ifdef EMULATE_UNDER_POSTMASTER

forcesharedmemory:

#endif

    if (!IsUnderPostmaster) /* postmaster already did this */
	{
		PostgresIpcKey = key;
		AttachSharedMemoryAndSemaphores(key);
	}
}


/* --------------------------------
 *		InitStdio
 *
 *		this routine consists of a bunch of code fragments
 *		that used to be randomly scattered through cinit().
 *		they all seem to do stuff associated with io.
 * --------------------------------
 */
static void
InitStdio()
{
	DebugFileOpen();
}

/* --------------------------------
 * InitPostgres --
 *		Initialize POSTGRES.
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
bool		PostgresIsInitialized = false;
extern int	NBuffers;

/*
 *	this global is used by wei for testing his code, but must be declared
 *	here rather than in postgres.c so that it's defined for cinterface.a
 *	applications.
 */

/*int	testFlag = 0;*/
int			lockingOff = 0;

/*
 */
void
InitPostgres(char *name)		/* database name */
{
	bool		bootstrap;		/* true if BootstrapProcessing */

	/* ----------------
	 *	see if we're running in BootstrapProcessing mode
	 * ----------------
	 */
	bootstrap = IsBootstrapProcessingMode();

	/* ----------------
	 *	turn on the exception handler.	Note: we cannot use elog, Assert,
	 *	AssertState, etc. until after exception handling is on.
	 * ----------------
	 */
	EnableExceptionHandling(true);

	/* ----------------
	 *	A stupid check to make sure we don't call this more than once.
	 *	But things like ReinitPostgres() get around this by just diddling
	 *	the PostgresIsInitialized flag.
	 * ----------------
	 */
	AssertState(!PostgresIsInitialized);

	/* ----------------
	 *	Memory system initialization.
	 *	(we may call palloc after EnableMemoryContext())
	 *
	 *	Note EnableMemoryContext() must happen before EnablePortalManager().
	 * ----------------
	 */
	EnableMemoryContext(true);	/* initializes the "top context" */
	EnablePortalManager(true);	/* memory for portal/transaction stuff */

	/* ----------------
	 *	initialize the backend local portal stack used by
	 *	internal PQ function calls.  see src/lib/libpq/be-dumpdata.c
	 *	This is different from the "portal manager" so this goes here.
	 *	-cim 2/12/91
	 * ----------------
	 */
	be_portalinit();

	/* ----------------
	 *	 attach to shared memory and semaphores, and initialize our
	 *	 input/output/debugging file descriptors.
	 * ----------------
	 */
	InitCommunication();
	InitStdio();

	/*
	 * initialize the local buffer manager
	 */
	InitLocalBuffer();

	if (!TransactionFlushEnabled())
		on_shmem_exit(FlushBufferPool, (caddr_t) NULL);

	/* ----------------
	 *	initialize the database id used for system caches and lock tables
	 * ----------------
	 */
	if (bootstrap)
	{
		SetDatabasePath(ExpandDatabasePath(name));
		SetDatabaseName(name);
		LockDisable(true);
	}
	else
	{
		VerifySystemDatabase();
		InitMyDatabaseInfo(name);
		VerifyMyDatabase();
	}

	/*
	 * ********************************
	 *
	 * code after this point assumes we are in the proper directory!
	 *
	 * So, how do we implement alternate locations for databases? There are
	 * two possible locations for tables and we need to look in
	 * DataDir/pg_database to find the true location of an individual
	 * database. We can brute-force it as done in InitMyDatabaseInfo(), or
	 * we can be patient and wait until we open pg_database gracefully.
	 * Will try that, but may not work... - thomas 1997-11-01 ********************************
	 *
	 */

	/* Does not touch files (?) - thomas 1997-11-01 */
	smgrinit();

	/* ----------------
	 *	initialize the transaction system and the relation descriptor cache.
	 *	Note we have to make certain the lock manager is off while we do this.
	 * ----------------
	 */
	AmiTransactionOverride(IsBootstrapProcessingMode());
	LockDisable(true);

	/*
	 * Part of the initialization processing done here sets a read lock on
	 * pg_log.	Since locking is disabled the set doesn't have intended
	 * effect of locking out writers, but this is ok, since we only lock
	 * it to examine AMI transaction status, and this is never written
	 * after initdb is done. -mer 15 June 1992
	 */
	RelationInitialize();		/* pre-allocated reldescs created here */
	InitializeTransactionSystem();		/* pg_log,etc init/crash recovery
										 * here */

	LockDisable(false);

	/* ----------------
	 *	anyone knows what this does?  something having to do with
	 *	system catalog cache invalidation in the case of multiple
	 *	backends, I think -cim 10/3/90
	 *	Sets up MyBackendId a unique backend identifier.
	 * ----------------
	 */
	InitSharedInvalidationState();

	/* ----------------
	 * Set up a per backend process in shared memory.  Must be done after
	 * InitSharedInvalidationState() as it relies on MyBackendId being
	 * initialized already.  XXX -mer 11 Aug 1991
	 * ----------------
	 */
	InitProcess(PostgresIpcKey);

	if (MyBackendId > MaxBackendId || MyBackendId <= 0)
	{
		elog(FATAL, "cinit2: bad backend id %d (%d)",
			 MyBackendTag,
			 MyBackendId);
	}

	/* ----------------
	 *	initialize the access methods.
	 *	Does not touch files (?) - thomas 1997-11-01
	 * ----------------
	 */
	initam();

	/* ----------------
	 *	initialize all the system catalog caches.
	 * ----------------
	 */
	zerocaches();

	/*
	 * Does not touch files since all routines are builtins (?) - thomas
	 * 1997-11-01
	 */
	InitCatalogCache();

	/* ----------------
	 *	 set ourselves to the proper user id and figure out our postgres
	 *	 user id.  If we ever add security so that we check for valid
	 *	 postgres users, we might do it here.
	 * ----------------
	 */
	InitUserid();

	/* ----------------
	 *	 initialize local data in cache invalidation stuff
	 * ----------------
	 */
	if (!bootstrap)
		InitLocalInvalidateData();

	/* ----------------
	 *	ok, all done, now let's make sure we don't do it again.
	 * ----------------
	 */
	PostgresIsInitialized = true;
/*	  on_shmem_exit(DestroyLocalRelList, (caddr_t) NULL); */

	/* ----------------
	 *	Done with "InitPostgres", now change to NormalProcessing unless
	 *	we're in BootstrapProcessing mode.
	 * ----------------
	 */
	if (!bootstrap)
		SetProcessingMode(NormalProcessing);
/*	  if (testFlag || lockingOff) */
	if (lockingOff)
		LockDisable(true);
}
