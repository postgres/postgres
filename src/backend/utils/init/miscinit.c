/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/miscinit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/param.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <utime.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/file_perm.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/slotsync.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


#define DIRECTORY_LOCK_FILE		"postmaster.pid"

ProcessingMode Mode = InitProcessing;

BackendType MyBackendType;

/* List of lock files to be removed at proc exit */
static List *lock_files = NIL;

static Latch LocalLatchData;

/* ----------------------------------------------------------------
 *		ignoring system indexes support stuff
 *
 * NOTE: "ignoring system indexes" means we do not use the system indexes
 * for lookups (either in hardwired catalog accesses or in planner-generated
 * plans).  We do, however, still update the indexes when a catalog
 * modification is made.
 * ----------------------------------------------------------------
 */

bool		IgnoreSystemIndexes = false;


/* ----------------------------------------------------------------
 *	common process startup code
 * ----------------------------------------------------------------
 */

/*
 * Initialize the basic environment for a postmaster child
 *
 * Should be called as early as possible after the child's startup. However,
 * on EXEC_BACKEND builds it does need to be after read_backend_variables().
 */
void
InitPostmasterChild(void)
{
	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	/*
	 * Start our win32 signal implementation. This has to be done after we
	 * read the backend variables, because we need to pick up the signal pipe
	 * from the parent process.
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	InitProcessGlobals();

	/*
	 * make sure stderr is in binary mode before anything can possibly be
	 * written to it, in case it's actually the syslogger pipe, so the pipe
	 * chunking protocol isn't disturbed. Non-logpipe data gets translated on
	 * redirection (e.g. via pg_ctl -l) anyway.
	 */
#ifdef WIN32
	_setmode(fileno(stderr), _O_BINARY);
#endif

	/* We don't want the postmaster's proc_exit() handlers */
	on_exit_reset();

	/* In EXEC_BACKEND case we will not have inherited BlockSig etc values */
#ifdef EXEC_BACKEND
	pqinitmask();
#endif

	/* Initialize process-local latch support */
	InitializeWaitEventSupport();
	InitProcessLocalLatch();
	InitializeLatchWaitSet();

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too. Not all processes will have
	 * children, but for consistency we make all postmaster child processes do
	 * this.
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Every postmaster child process is expected to respond promptly to
	 * SIGQUIT at all times.  Therefore we centrally remove SIGQUIT from
	 * BlockSig and install a suitable signal handler.  (Client-facing
	 * processes may choose to replace this default choice of handler with
	 * quickdie().)  All other blockable signals remain blocked for now.
	 */
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);

	sigdelset(&BlockSig, SIGQUIT);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/* Request a signal if the postmaster dies, if possible. */
	PostmasterDeathSignalInit();

	/* Don't give the pipe to subprograms that we execute. */
#ifndef WIN32
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFD, FD_CLOEXEC) < 0)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to FD_CLOEXEC mode: %m")));
#endif
}

/*
 * Initialize the basic environment for a standalone process.
 *
 * argv0 has to be suitable to find the program's executable.
 */
void
InitStandaloneProcess(const char *argv0)
{
	Assert(!IsPostmasterEnvironment);

	MyBackendType = B_STANDALONE_BACKEND;

	/*
	 * Start our win32 signal implementation
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	InitProcessGlobals();

	/* Initialize process-local latch support */
	InitializeWaitEventSupport();
	InitProcessLocalLatch();
	InitializeLatchWaitSet();

	/*
	 * For consistency with InitPostmasterChild, initialize signal mask here.
	 * But we don't unblock SIGQUIT or provide a default handler for it.
	 */
	pqinitmask();
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/* Compute paths, no postmaster to inherit from */
	if (my_exec_path[0] == '\0')
	{
		if (find_my_exec(argv0, my_exec_path) < 0)
			elog(FATAL, "%s: could not locate my own executable path",
				 argv0);
	}

	if (pkglib_path[0] == '\0')
		get_pkglib_path(my_exec_path, pkglib_path);
}

void
SwitchToSharedLatch(void)
{
	Assert(MyLatch == &LocalLatchData);
	Assert(MyProc != NULL);

	MyLatch = &MyProc->procLatch;

	if (FeBeWaitSet)
		ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetLatchPos, WL_LATCH_SET,
						MyLatch);

	/*
	 * Set the shared latch as the local one might have been set. This
	 * shouldn't normally be necessary as code is supposed to check the
	 * condition before waiting for the latch, but a bit care can't hurt.
	 */
	SetLatch(MyLatch);
}

void
InitProcessLocalLatch(void)
{
	MyLatch = &LocalLatchData;
	InitLatch(MyLatch);
}

void
SwitchBackToLocalLatch(void)
{
	Assert(MyLatch != &LocalLatchData);
	Assert(MyProc != NULL && MyLatch == &MyProc->procLatch);

	MyLatch = &LocalLatchData;

	if (FeBeWaitSet)
		ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetLatchPos, WL_LATCH_SET,
						MyLatch);

	SetLatch(MyLatch);
}

/*
 * Return a human-readable string representation of a BackendType.
 *
 * The string is not localized here, but we mark the strings for translation
 * so that callers can invoke _() on the result.
 */
const char *
GetBackendTypeDesc(BackendType backendType)
{
	const char *backendDesc = gettext_noop("unknown process type");

	switch (backendType)
	{
		case B_INVALID:
			backendDesc = gettext_noop("not initialized");
			break;
		case B_ARCHIVER:
			backendDesc = gettext_noop("archiver");
			break;
		case B_AUTOVAC_LAUNCHER:
			backendDesc = gettext_noop("autovacuum launcher");
			break;
		case B_AUTOVAC_WORKER:
			backendDesc = gettext_noop("autovacuum worker");
			break;
		case B_BACKEND:
			backendDesc = gettext_noop("client backend");
			break;
		case B_DEAD_END_BACKEND:
			backendDesc = gettext_noop("dead-end client backend");
			break;
		case B_BG_WORKER:
			backendDesc = gettext_noop("background worker");
			break;
		case B_BG_WRITER:
			backendDesc = gettext_noop("background writer");
			break;
		case B_CHECKPOINTER:
			backendDesc = gettext_noop("checkpointer");
			break;
		case B_LOGGER:
			backendDesc = gettext_noop("logger");
			break;
		case B_SLOTSYNC_WORKER:
			backendDesc = gettext_noop("slotsync worker");
			break;
		case B_STANDALONE_BACKEND:
			backendDesc = gettext_noop("standalone backend");
			break;
		case B_STARTUP:
			backendDesc = gettext_noop("startup");
			break;
		case B_WAL_RECEIVER:
			backendDesc = gettext_noop("walreceiver");
			break;
		case B_WAL_SENDER:
			backendDesc = gettext_noop("walsender");
			break;
		case B_WAL_SUMMARIZER:
			backendDesc = gettext_noop("walsummarizer");
			break;
		case B_WAL_WRITER:
			backendDesc = gettext_noop("walwriter");
			break;
	}

	return backendDesc;
}

/* ----------------------------------------------------------------
 *				database path / name support stuff
 * ----------------------------------------------------------------
 */

void
SetDatabasePath(const char *path)
{
	/* This should happen only once per process */
	Assert(!DatabasePath);
	DatabasePath = MemoryContextStrdup(TopMemoryContext, path);
}

/*
 * Validate the proposed data directory.
 *
 * Also initialize file and directory create modes and mode mask.
 */
void
checkDataDir(void)
{
	struct stat stat_buf;

	Assert(DataDir);

	if (stat(DataDir, &stat_buf) != 0)
	{
		if (errno == ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("data directory \"%s\" does not exist",
							DataDir)));
		else
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read permissions of directory \"%s\": %m",
							DataDir)));
	}

	/* eventual chdir would fail anyway, but let's test ... */
	if (!S_ISDIR(stat_buf.st_mode))
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("specified data directory \"%s\" is not a directory",
						DataDir)));

	/*
	 * Check that the directory belongs to my userid; if not, reject.
	 *
	 * This check is an essential part of the interlock that prevents two
	 * postmasters from starting in the same directory (see CreateLockFile()).
	 * Do not remove or weaken it.
	 *
	 * XXX can we safely enable this check on Windows?
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	if (stat_buf.st_uid != geteuid())
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data directory \"%s\" has wrong ownership",
						DataDir),
				 errhint("The server must be started by the user that owns the data directory.")));
#endif

	/*
	 * Check if the directory has correct permissions.  If not, reject.
	 *
	 * Only two possible modes are allowed, 0700 and 0750.  The latter mode
	 * indicates that group read/execute should be allowed on all newly
	 * created files and directories.
	 *
	 * XXX temporarily suppress check when on Windows, because there may not
	 * be proper support for Unix-y file permissions.  Need to think of a
	 * reasonable check to apply on Windows.
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	if (stat_buf.st_mode & PG_MODE_MASK_GROUP)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data directory \"%s\" has invalid permissions",
						DataDir),
				 errdetail("Permissions should be u=rwx (0700) or u=rwx,g=rx (0750).")));
#endif

	/*
	 * Reset creation modes and mask based on the mode of the data directory.
	 *
	 * The mask was set earlier in startup to disallow group permissions on
	 * newly created files and directories.  However, if group read/execute
	 * are present on the data directory then modify the create modes and mask
	 * to allow group read/execute on newly created files and directories and
	 * set the data_directory_mode GUC.
	 *
	 * Suppress when on Windows, because there may not be proper support for
	 * Unix-y file permissions.
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	SetDataDirectoryCreatePerm(stat_buf.st_mode);

	umask(pg_mode_mask);
	data_directory_mode = pg_dir_create_mode;
#endif

	/* Check for PG_VERSION */
	ValidatePgVersion(DataDir);
}

/*
 * Set data directory, but make sure it's an absolute path.  Use this,
 * never set DataDir directly.
 */
void
SetDataDir(const char *dir)
{
	char	   *new;

	Assert(dir);

	/* If presented path is relative, convert to absolute */
	new = make_absolute_path(dir);

	free(DataDir);
	DataDir = new;
}

/*
 * Change working directory to DataDir.  Most of the postmaster and backend
 * code assumes that we are in DataDir so it can use relative paths to access
 * stuff in and under the data directory.  For convenience during path
 * setup, however, we don't force the chdir to occur during SetDataDir.
 */
void
ChangeToDataDir(void)
{
	Assert(DataDir);

	if (chdir(DataDir) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not change directory to \"%s\": %m",
						DataDir)));
}


/* ----------------------------------------------------------------
 *	User ID state
 *
 * We have to track several different values associated with the concept
 * of "user ID".
 *
 * AuthenticatedUserId is determined at connection start and never changes.
 *
 * SessionUserId is initially the same as AuthenticatedUserId, but can be
 * changed by SET SESSION AUTHORIZATION (if AuthenticatedUserId is a
 * superuser).  This is the ID reported by the SESSION_USER SQL function.
 *
 * OuterUserId is the current user ID in effect at the "outer level" (outside
 * any transaction or function).  This is initially the same as SessionUserId,
 * but can be changed by SET ROLE to any role that SessionUserId is a
 * member of.  (XXX rename to something like CurrentRoleId?)
 *
 * CurrentUserId is the current effective user ID; this is the one to use
 * for all normal permissions-checking purposes.  At outer level this will
 * be the same as OuterUserId, but it changes during calls to SECURITY
 * DEFINER functions, as well as locally in some specialized commands.
 *
 * SecurityRestrictionContext holds flags indicating reason(s) for changing
 * CurrentUserId.  In some cases we need to lock down operations that are
 * not directly controlled by privilege settings, and this provides a
 * convenient way to do it.
 * ----------------------------------------------------------------
 */
static Oid	AuthenticatedUserId = InvalidOid;
static Oid	SessionUserId = InvalidOid;
static Oid	OuterUserId = InvalidOid;
static Oid	CurrentUserId = InvalidOid;
static const char *SystemUser = NULL;

/* We also have to remember the superuser state of the session user */
static bool SessionUserIsSuperuser = false;

static int	SecurityRestrictionContext = 0;

/* We also remember if a SET ROLE is currently active */
static bool SetRoleIsActive = false;

/*
 * GetUserId - get the current effective user ID.
 *
 * Note: there's no SetUserId() anymore; use SetUserIdAndSecContext().
 */
Oid
GetUserId(void)
{
	Assert(OidIsValid(CurrentUserId));
	return CurrentUserId;
}


/*
 * GetOuterUserId/SetOuterUserId - get/set the outer-level user ID.
 */
Oid
GetOuterUserId(void)
{
	Assert(OidIsValid(OuterUserId));
	return OuterUserId;
}


static void
SetOuterUserId(Oid userid, bool is_superuser)
{
	Assert(SecurityRestrictionContext == 0);
	Assert(OidIsValid(userid));
	OuterUserId = userid;

	/* We force the effective user ID to match, too */
	CurrentUserId = userid;

	/* Also update the is_superuser GUC to match OuterUserId's property */
	SetConfigOption("is_superuser",
					is_superuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
}


/*
 * GetSessionUserId/SetSessionUserId - get/set the session user ID.
 */
Oid
GetSessionUserId(void)
{
	Assert(OidIsValid(SessionUserId));
	return SessionUserId;
}

bool
GetSessionUserIsSuperuser(void)
{
	Assert(OidIsValid(SessionUserId));
	return SessionUserIsSuperuser;
}

static void
SetSessionUserId(Oid userid, bool is_superuser)
{
	Assert(SecurityRestrictionContext == 0);
	Assert(OidIsValid(userid));
	SessionUserId = userid;
	SessionUserIsSuperuser = is_superuser;
}

/*
 * Return the system user representing the authenticated identity.
 * It is defined in InitializeSystemUser() as auth_method:authn_id.
 */
const char *
GetSystemUser(void)
{
	return SystemUser;
}

/*
 * GetAuthenticatedUserId/SetAuthenticatedUserId - get/set the authenticated
 * user ID
 */
Oid
GetAuthenticatedUserId(void)
{
	Assert(OidIsValid(AuthenticatedUserId));
	return AuthenticatedUserId;
}

void
SetAuthenticatedUserId(Oid userid)
{
	Assert(OidIsValid(userid));

	/* call only once */
	Assert(!OidIsValid(AuthenticatedUserId));

	AuthenticatedUserId = userid;

	/* Also mark our PGPROC entry with the authenticated user id */
	/* (We assume this is an atomic store so no lock is needed) */
	MyProc->roleId = userid;
}


/*
 * GetUserIdAndSecContext/SetUserIdAndSecContext - get/set the current user ID
 * and the SecurityRestrictionContext flags.
 *
 * Currently there are three valid bits in SecurityRestrictionContext:
 *
 * SECURITY_LOCAL_USERID_CHANGE indicates that we are inside an operation
 * that is temporarily changing CurrentUserId via these functions.  This is
 * needed to indicate that the actual value of CurrentUserId is not in sync
 * with guc.c's internal state, so SET ROLE has to be disallowed.
 *
 * SECURITY_RESTRICTED_OPERATION indicates that we are inside an operation
 * that does not wish to trust called user-defined functions at all.  The
 * policy is to use this before operations, e.g. autovacuum and REINDEX, that
 * enumerate relations of a database or schema and run functions associated
 * with each found relation.  The relation owner is the new user ID.  Set this
 * as soon as possible after locking the relation.  Restore the old user ID as
 * late as possible before closing the relation; restoring it shortly after
 * close is also tolerable.  If a command has both relation-enumerating and
 * non-enumerating modes, e.g. ANALYZE, both modes set this bit.  This bit
 * prevents not only SET ROLE, but various other changes of session state that
 * normally is unprotected but might possibly be used to subvert the calling
 * session later.  An example is replacing an existing prepared statement with
 * new code, which will then be executed with the outer session's permissions
 * when the prepared statement is next used.  These restrictions are fairly
 * draconian, but the functions called in relation-enumerating operations are
 * really supposed to be side-effect-free anyway.
 *
 * SECURITY_NOFORCE_RLS indicates that we are inside an operation which should
 * ignore the FORCE ROW LEVEL SECURITY per-table indication.  This is used to
 * ensure that FORCE RLS does not mistakenly break referential integrity
 * checks.  Note that this is intentionally only checked when running as the
 * owner of the table (which should always be the case for referential
 * integrity checks).
 *
 * Unlike GetUserId, GetUserIdAndSecContext does *not* Assert that the current
 * value of CurrentUserId is valid; nor does SetUserIdAndSecContext require
 * the new value to be valid.  In fact, these routines had better not
 * ever throw any kind of error.  This is because they are used by
 * StartTransaction and AbortTransaction to save/restore the settings,
 * and during the first transaction within a backend, the value to be saved
 * and perhaps restored is indeed invalid.  We have to be able to get
 * through AbortTransaction without asserting in case InitPostgres fails.
 */
void
GetUserIdAndSecContext(Oid *userid, int *sec_context)
{
	*userid = CurrentUserId;
	*sec_context = SecurityRestrictionContext;
}

void
SetUserIdAndSecContext(Oid userid, int sec_context)
{
	CurrentUserId = userid;
	SecurityRestrictionContext = sec_context;
}


/*
 * InLocalUserIdChange - are we inside a local change of CurrentUserId?
 */
bool
InLocalUserIdChange(void)
{
	return (SecurityRestrictionContext & SECURITY_LOCAL_USERID_CHANGE) != 0;
}

/*
 * InSecurityRestrictedOperation - are we inside a security-restricted command?
 */
bool
InSecurityRestrictedOperation(void)
{
	return (SecurityRestrictionContext & SECURITY_RESTRICTED_OPERATION) != 0;
}

/*
 * InNoForceRLSOperation - are we ignoring FORCE ROW LEVEL SECURITY ?
 */
bool
InNoForceRLSOperation(void)
{
	return (SecurityRestrictionContext & SECURITY_NOFORCE_RLS) != 0;
}


/*
 * These are obsolete versions of Get/SetUserIdAndSecContext that are
 * only provided for bug-compatibility with some rather dubious code in
 * pljava.  We allow the userid to be set, but only when not inside a
 * security restriction context.
 */
void
GetUserIdAndContext(Oid *userid, bool *sec_def_context)
{
	*userid = CurrentUserId;
	*sec_def_context = InLocalUserIdChange();
}

void
SetUserIdAndContext(Oid userid, bool sec_def_context)
{
	/* We throw the same error SET ROLE would. */
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot set parameter \"%s\" within security-restricted operation",
						"role")));
	CurrentUserId = userid;
	if (sec_def_context)
		SecurityRestrictionContext |= SECURITY_LOCAL_USERID_CHANGE;
	else
		SecurityRestrictionContext &= ~SECURITY_LOCAL_USERID_CHANGE;
}


/*
 * Check whether specified role has explicit REPLICATION privilege
 */
bool
has_rolreplication(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolreplication;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * Initialize user identity during normal backend startup
 */
void
InitializeSessionUserId(const char *rolename, Oid roleid,
						bool bypass_login_check)
{
	HeapTuple	roleTup;
	Form_pg_authid rform;
	char	   *rname;
	bool		is_superuser;

	/*
	 * In a parallel worker, we don't have to do anything here.
	 * ParallelWorkerMain already set our output variables, and we aren't
	 * going to enforce either rolcanlogin or rolconnlimit.  Furthermore, we
	 * don't really want to perform a catalog lookup for the role: we don't
	 * want to fail if it's been dropped.
	 */
	if (InitializingParallelWorker)
	{
		Assert(bypass_login_check);
		return;
	}

	/*
	 * Don't do scans if we're bootstrapping, none of the system catalogs
	 * exist yet, and they should be owned by postgres anyway.
	 */
	Assert(!IsBootstrapProcessingMode());

	/*
	 * Make sure syscache entries are flushed for recent catalog changes. This
	 * allows us to find roles that were created on-the-fly during
	 * authentication.
	 */
	AcceptInvalidationMessages();

	/*
	 * Look up the role, either by name if that's given or by OID if not.
	 */
	if (rolename != NULL)
	{
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(rolename));
		if (!HeapTupleIsValid(roleTup))
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role \"%s\" does not exist", rolename)));
	}
	else
	{
		roleTup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
		if (!HeapTupleIsValid(roleTup))
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role with OID %u does not exist", roleid)));
	}

	rform = (Form_pg_authid) GETSTRUCT(roleTup);
	roleid = rform->oid;
	rname = NameStr(rform->rolname);
	is_superuser = rform->rolsuper;

	SetAuthenticatedUserId(roleid);

	/*
	 * Set SessionUserId and related variables, including "role", via the GUC
	 * mechanisms.
	 *
	 * Note: ideally we would use PGC_S_DYNAMIC_DEFAULT here, so that
	 * session_authorization could subsequently be changed from
	 * pg_db_role_setting entries.  Instead, session_authorization in
	 * pg_db_role_setting has no effect.  Changing that would require solving
	 * two problems:
	 *
	 * 1. If pg_db_role_setting has values for both session_authorization and
	 * role, we could not be sure which order those would be applied in, and
	 * it would matter.
	 *
	 * 2. Sites may have years-old session_authorization entries.  There's not
	 * been any particular reason to remove them.  Ending the dormancy of
	 * those entries could seriously change application behavior, so only a
	 * major release should do that.
	 */
	SetConfigOption("session_authorization", rname,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/*
	 * These next checks are not enforced when in standalone mode, so that
	 * there is a way to recover from sillinesses like "UPDATE pg_authid SET
	 * rolcanlogin = false;".
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Is role allowed to login at all?  (But background workers can
		 * override this by setting bypass_login_check.)
		 */
		if (!bypass_login_check && !rform->rolcanlogin)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role \"%s\" is not permitted to log in",
							rname)));

		/*
		 * Check connection limit for this role.  We enforce the limit only
		 * for regular backends, since other process types have their own
		 * PGPROC pools.
		 *
		 * There is a race condition here --- we create our PGPROC before
		 * checking for other PGPROCs.  If two backends did this at about the
		 * same time, they might both think they were over the limit, while
		 * ideally one should succeed and one fail.  Getting that to work
		 * exactly seems more trouble than it is worth, however; instead we
		 * just document that the connection limit is approximate.
		 */
		if (rform->rolconnlimit >= 0 &&
			AmRegularBackendProcess() &&
			!is_superuser &&
			CountUserBackends(roleid) > rform->rolconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for role \"%s\"",
							rname)));
	}

	ReleaseSysCache(roleTup);
}


/*
 * Initialize user identity during special backend startup
 */
void
InitializeSessionUserIdStandalone(void)
{
	/*
	 * This function should only be called in single-user mode, in autovacuum
	 * workers, in slot sync worker and in background workers.
	 */
	Assert(!IsUnderPostmaster || AmAutoVacuumWorkerProcess() ||
		   AmLogicalSlotSyncWorkerProcess() || AmBackgroundWorkerProcess());

	/* call only once */
	Assert(!OidIsValid(AuthenticatedUserId));

	AuthenticatedUserId = BOOTSTRAP_SUPERUSERID;

	/*
	 * XXX Ideally we'd do this via SetConfigOption("session_authorization"),
	 * but we lack the role name needed to do that, and we can't fetch it
	 * because one reason for this special case is to be able to start up even
	 * if something's happened to the BOOTSTRAP_SUPERUSERID's pg_authid row.
	 * Since we don't set the GUC itself, C code will see the value as NULL,
	 * and current_setting() will report an empty string within this session.
	 */
	SetSessionAuthorization(BOOTSTRAP_SUPERUSERID, true);

	/* We could do SetConfigOption("role"), but let's be consistent */
	SetCurrentRoleId(InvalidOid, false);
}

/*
 * Initialize the system user.
 *
 * This is built as auth_method:authn_id.
 */
void
InitializeSystemUser(const char *authn_id, const char *auth_method)
{
	char	   *system_user;

	/* call only once */
	Assert(SystemUser == NULL);

	/*
	 * InitializeSystemUser should be called only when authn_id is not NULL,
	 * meaning that auth_method is valid.
	 */
	Assert(authn_id != NULL);

	system_user = psprintf("%s:%s", auth_method, authn_id);

	/* Store SystemUser in long-lived storage */
	SystemUser = MemoryContextStrdup(TopMemoryContext, system_user);
	pfree(system_user);
}

/*
 * SQL-function SYSTEM_USER
 */
Datum
system_user(PG_FUNCTION_ARGS)
{
	const char *sysuser = GetSystemUser();

	if (sysuser)
		PG_RETURN_DATUM(CStringGetTextDatum(sysuser));
	else
		PG_RETURN_NULL();
}

/*
 * Change session auth ID while running
 *
 * The SQL standard says that SET SESSION AUTHORIZATION implies SET ROLE NONE.
 * We mechanize that at higher levels not here, because this is the GUC
 * assign hook for "session_authorization", and it must be commutative with
 * SetCurrentRoleId (the hook for "role") because guc.c provides no guarantees
 * which will run first during cases such as transaction rollback.  Therefore,
 * we update derived state (OuterUserId/CurrentUserId/is_superuser) only if
 * !SetRoleIsActive.
 */
void
SetSessionAuthorization(Oid userid, bool is_superuser)
{
	SetSessionUserId(userid, is_superuser);

	if (!SetRoleIsActive)
		SetOuterUserId(userid, is_superuser);
}

/*
 * Report current role id
 *		This follows the semantics of SET ROLE, ie return the outer-level ID
 *		not the current effective ID, and return InvalidOid when the setting
 *		is logically SET ROLE NONE.
 */
Oid
GetCurrentRoleId(void)
{
	if (SetRoleIsActive)
		return OuterUserId;
	else
		return InvalidOid;
}

/*
 * Change Role ID while running (SET ROLE)
 *
 * If roleid is InvalidOid, we are doing SET ROLE NONE: revert to the
 * session user authorization.  In this case the is_superuser argument
 * is ignored.
 *
 * When roleid is not InvalidOid, the caller must have checked whether
 * the session user has permission to become that role.  (We cannot check
 * here because this routine must be able to execute in a failed transaction
 * to restore a prior value of the ROLE GUC variable.)
 */
void
SetCurrentRoleId(Oid roleid, bool is_superuser)
{
	/*
	 * Get correct info if it's SET ROLE NONE
	 *
	 * If SessionUserId hasn't been set yet, do nothing beyond updating
	 * SetRoleIsActive --- the eventual SetSessionAuthorization call will
	 * update the derived state.  This is needed since we will get called
	 * during GUC initialization.
	 */
	if (!OidIsValid(roleid))
	{
		SetRoleIsActive = false;

		if (!OidIsValid(SessionUserId))
			return;

		roleid = SessionUserId;
		is_superuser = SessionUserIsSuperuser;
	}
	else
		SetRoleIsActive = true;

	SetOuterUserId(roleid, is_superuser);
}


/*
 * Get user name from user oid, returns NULL for nonexistent roleid if noerr
 * is true.
 */
char *
GetUserNameFromId(Oid roleid, bool noerr)
{
	HeapTuple	tuple;
	char	   *result;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (!HeapTupleIsValid(tuple))
	{
		if (!noerr)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("invalid role OID: %u", roleid)));
		result = NULL;
	}
	else
	{
		result = pstrdup(NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname));
		ReleaseSysCache(tuple);
	}
	return result;
}

/* ------------------------------------------------------------------------
 *				Client connection state shared with parallel workers
 *
 * ClientConnectionInfo contains pieces of information about the client that
 * need to be synced to parallel workers when they initialize.
 *-------------------------------------------------------------------------
 */

ClientConnectionInfo MyClientConnectionInfo;

/*
 * Intermediate representation of ClientConnectionInfo for easier
 * serialization.  Variable-length fields are allocated right after this
 * header.
 */
typedef struct SerializedClientConnectionInfo
{
	int32		authn_id_len;	/* strlen(authn_id), or -1 if NULL */
	UserAuth	auth_method;
} SerializedClientConnectionInfo;

/*
 * Calculate the space needed to serialize MyClientConnectionInfo.
 */
Size
EstimateClientConnectionInfoSpace(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(SerializedClientConnectionInfo));

	if (MyClientConnectionInfo.authn_id)
		size = add_size(size, strlen(MyClientConnectionInfo.authn_id) + 1);

	return size;
}

/*
 * Serialize MyClientConnectionInfo for use by parallel workers.
 */
void
SerializeClientConnectionInfo(Size maxsize, char *start_address)
{
	SerializedClientConnectionInfo serialized = {0};

	serialized.authn_id_len = -1;
	serialized.auth_method = MyClientConnectionInfo.auth_method;

	if (MyClientConnectionInfo.authn_id)
		serialized.authn_id_len = strlen(MyClientConnectionInfo.authn_id);

	/* Copy serialized representation to buffer */
	Assert(maxsize >= sizeof(serialized));
	memcpy(start_address, &serialized, sizeof(serialized));

	maxsize -= sizeof(serialized);
	start_address += sizeof(serialized);

	/* Copy authn_id into the space after the struct */
	if (serialized.authn_id_len >= 0)
	{
		Assert(maxsize >= (serialized.authn_id_len + 1));
		memcpy(start_address,
			   MyClientConnectionInfo.authn_id,
		/* include the NULL terminator to ease deserialization */
			   serialized.authn_id_len + 1);
	}
}

/*
 * Restore MyClientConnectionInfo from its serialized representation.
 */
void
RestoreClientConnectionInfo(char *conninfo)
{
	SerializedClientConnectionInfo serialized;

	memcpy(&serialized, conninfo, sizeof(serialized));

	/* Copy the fields back into place */
	MyClientConnectionInfo.authn_id = NULL;
	MyClientConnectionInfo.auth_method = serialized.auth_method;

	if (serialized.authn_id_len >= 0)
	{
		char	   *authn_id;

		authn_id = conninfo + sizeof(serialized);
		MyClientConnectionInfo.authn_id = MemoryContextStrdup(TopMemoryContext,
															  authn_id);
	}
}


/*-------------------------------------------------------------------------
 *				Interlock-file support
 *
 * These routines are used to create both a data-directory lockfile
 * ($DATADIR/postmaster.pid) and Unix-socket-file lockfiles ($SOCKFILE.lock).
 * Both kinds of files contain the same info initially, although we can add
 * more information to a data-directory lockfile after it's created, using
 * AddToDataDirLockFile().  See pidfile.h for documentation of the contents
 * of these lockfiles.
 *
 * On successful lockfile creation, a proc_exit callback to remove the
 * lockfile is automatically created.
 *-------------------------------------------------------------------------
 */

/*
 * proc_exit callback to remove lockfiles.
 */
static void
UnlinkLockFiles(int status, Datum arg)
{
	ListCell   *l;

	foreach(l, lock_files)
	{
		char	   *curfile = (char *) lfirst(l);

		unlink(curfile);
		/* Should we complain if the unlink fails? */
	}
	/* Since we're about to exit, no need to reclaim storage */
	lock_files = NIL;

	/*
	 * Lock file removal should always be the last externally visible action
	 * of a postmaster or standalone backend, while we won't come here at all
	 * when exiting postmaster child processes.  Therefore, this is a good
	 * place to log completion of shutdown.  We could alternatively teach
	 * proc_exit() to do it, but that seems uglier.  In a standalone backend,
	 * use NOTICE elevel to be less chatty.
	 */
	ereport(IsPostmasterEnvironment ? LOG : NOTICE,
			(errmsg("database system is shut down")));
}

/*
 * Create a lockfile.
 *
 * filename is the path name of the lockfile to create.
 * amPostmaster is used to determine how to encode the output PID.
 * socketDir is the Unix socket directory path to include (possibly empty).
 * isDDLock and refName are used to determine what error message to produce.
 */
static void
CreateLockFile(const char *filename, bool amPostmaster,
			   const char *socketDir,
			   bool isDDLock, const char *refName)
{
	int			fd;
	char		buffer[MAXPGPATH * 2 + 256];
	int			ntries;
	int			len;
	int			encoded_pid;
	pid_t		other_pid;
	pid_t		my_pid,
				my_p_pid,
				my_gp_pid;
	const char *envvar;

	/*
	 * If the PID in the lockfile is our own PID or our parent's or
	 * grandparent's PID, then the file must be stale (probably left over from
	 * a previous system boot cycle).  We need to check this because of the
	 * likelihood that a reboot will assign exactly the same PID as we had in
	 * the previous reboot, or one that's only one or two counts larger and
	 * hence the lockfile's PID now refers to an ancestor shell process.  We
	 * allow pg_ctl to pass down its parent shell PID (our grandparent PID)
	 * via the environment variable PG_GRANDPARENT_PID; this is so that
	 * launching the postmaster via pg_ctl can be just as reliable as
	 * launching it directly.  There is no provision for detecting
	 * further-removed ancestor processes, but if the init script is written
	 * carefully then all but the immediate parent shell will be root-owned
	 * processes and so the kill test will fail with EPERM.  Note that we
	 * cannot get a false negative this way, because an existing postmaster
	 * would surely never launch a competing postmaster or pg_ctl process
	 * directly.
	 */
	my_pid = getpid();

#ifndef WIN32
	my_p_pid = getppid();
#else

	/*
	 * Windows hasn't got getppid(), but doesn't need it since it's not using
	 * real kill() either...
	 */
	my_p_pid = 0;
#endif

	envvar = getenv("PG_GRANDPARENT_PID");
	if (envvar)
		my_gp_pid = atoi(envvar);
	else
		my_gp_pid = 0;

	/*
	 * We need a loop here because of race conditions.  But don't loop forever
	 * (for example, a non-writable $PGDATA directory might cause a failure
	 * that won't go away).  100 tries seems like plenty.
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 *
		 * Think not to make the file protection weaker than 0600/0640.  See
		 * comments below.
		 */
		fd = open(filename, O_RDWR | O_CREAT | O_EXCL, pg_file_create_mode);
		if (fd >= 0)
			break;				/* Success; exit the retry loop */

		/*
		 * Couldn't create the pid file. Probably it already exists.
		 */
		if ((errno != EEXIST && errno != EACCES) || ntries > 100)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create lock file \"%s\": %m",
							filename)));

		/*
		 * Read the file to get the old owner's PID.  Note race condition
		 * here: file might have been deleted since we tried to create it.
		 */
		fd = open(filename, O_RDONLY, pg_file_create_mode);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* race condition; try again */
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open lock file \"%s\": %m",
							filename)));
		}
		pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_CREATE_READ);
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read lock file \"%s\": %m",
							filename)));
		pgstat_report_wait_end();
		close(fd);

		if (len == 0)
		{
			ereport(FATAL,
					(errcode(ERRCODE_LOCK_FILE_EXISTS),
					 errmsg("lock file \"%s\" is empty", filename),
					 errhint("Either another server is starting, or the lock file is the remnant of a previous server startup crash.")));
		}

		buffer[len] = '\0';
		encoded_pid = atoi(buffer);

		/* if pid < 0, the pid is for postgres, not postmaster */
		other_pid = (pid_t) (encoded_pid < 0 ? -encoded_pid : encoded_pid);

		if (other_pid <= 0)
			elog(FATAL, "bogus data in lock file \"%s\": \"%s\"",
				 filename, buffer);

		/*
		 * Check to see if the other process still exists
		 *
		 * Per discussion above, my_pid, my_p_pid, and my_gp_pid can be
		 * ignored as false matches.
		 *
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist.
		 *
		 * We can treat the EPERM-error case as okay because that error
		 * implies that the existing process has a different userid than we
		 * do, which means it cannot be a competing postmaster.  A postmaster
		 * cannot successfully attach to a data directory owned by a userid
		 * other than its own, as enforced in checkDataDir(). Also, since we
		 * create the lockfiles mode 0600/0640, we'd have failed above if the
		 * lockfile belonged to another userid --- which means that whatever
		 * process kill() is reporting about isn't the one that made the
		 * lockfile.  (NOTE: this last consideration is the only one that
		 * keeps us from blowing away a Unix socket file belonging to an
		 * instance of Postgres being run by someone else, at least on
		 * machines where /tmp hasn't got a stickybit.)
		 */
		if (other_pid != my_pid && other_pid != my_p_pid &&
			other_pid != my_gp_pid)
		{
			if (kill(other_pid, 0) == 0 ||
				(errno != ESRCH && errno != EPERM))
			{
				/* lockfile belongs to a live process */
				ereport(FATAL,
						(errcode(ERRCODE_LOCK_FILE_EXISTS),
						 errmsg("lock file \"%s\" already exists",
								filename),
						 isDDLock ?
						 (encoded_pid < 0 ?
						  errhint("Is another postgres (PID %d) running in data directory \"%s\"?",
								  (int) other_pid, refName) :
						  errhint("Is another postmaster (PID %d) running in data directory \"%s\"?",
								  (int) other_pid, refName)) :
						 (encoded_pid < 0 ?
						  errhint("Is another postgres (PID %d) using socket file \"%s\"?",
								  (int) other_pid, refName) :
						  errhint("Is another postmaster (PID %d) using socket file \"%s\"?",
								  (int) other_pid, refName))));
			}
		}

		/*
		 * No, the creating process did not exist.  However, it could be that
		 * the postmaster crashed (or more likely was kill -9'd by a clueless
		 * admin) but has left orphan backends behind.  Check for this by
		 * looking to see if there is an associated shmem segment that is
		 * still in use.
		 *
		 * Note: because postmaster.pid is written in multiple steps, we might
		 * not find the shmem ID values in it; we can't treat that as an
		 * error.
		 */
		if (isDDLock)
		{
			char	   *ptr = buffer;
			unsigned long id1,
						id2;
			int			lineno;

			for (lineno = 1; lineno < LOCK_FILE_LINE_SHMEM_KEY; lineno++)
			{
				if ((ptr = strchr(ptr, '\n')) == NULL)
					break;
				ptr++;
			}

			if (ptr != NULL &&
				sscanf(ptr, "%lu %lu", &id1, &id2) == 2)
			{
				if (PGSharedMemoryIsInUse(id1, id2))
					ereport(FATAL,
							(errcode(ERRCODE_LOCK_FILE_EXISTS),
							 errmsg("pre-existing shared memory block (key %lu, ID %lu) is still in use",
									id1, id2),
							 errhint("Terminate any old server processes associated with data directory \"%s\".",
									 refName)));
			}
		}

		/*
		 * Looks like nobody's home.  Unlink the file and try again to create
		 * it.  Need a loop because of possible race condition against other
		 * would-be creators.
		 */
		if (unlink(filename) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not remove old lock file \"%s\": %m",
							filename),
					 errhint("The file seems accidentally left over, but "
							 "it could not be removed. Please remove the file "
							 "by hand and try again.")));
	}

	/*
	 * Successfully created the file, now fill it.  See comment in pidfile.h
	 * about the contents.  Note that we write the same first five lines into
	 * both datadir and socket lockfiles; although more stuff may get added to
	 * the datadir lockfile later.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n%s\n" INT64_FORMAT "\n%d\n%s\n",
			 amPostmaster ? (int) my_pid : -((int) my_pid),
			 DataDir,
			 MyStartTime,
			 PostPortNumber,
			 socketDir);

	/*
	 * In a standalone backend, the next line (LOCK_FILE_LINE_LISTEN_ADDR)
	 * will never receive data, so fill it in as empty now.
	 */
	if (isDDLock && !amPostmaster)
		strlcat(buffer, "\n", sizeof(buffer));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_CREATE_WRITE);
	if (write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int			save_errno = errno;

		close(fd);
		unlink(filename);
		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_CREATE_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		close(fd);
		unlink(filename);
		errno = save_errno;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}
	pgstat_report_wait_end();
	if (close(fd) != 0)
	{
		int			save_errno = errno;

		unlink(filename);
		errno = save_errno;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}

	/*
	 * Arrange to unlink the lock file(s) at proc_exit.  If this is the first
	 * one, set up the on_proc_exit function to do it; then add this lock file
	 * to the list of files to unlink.
	 */
	if (lock_files == NIL)
		on_proc_exit(UnlinkLockFiles, 0);

	/*
	 * Use lcons so that the lock files are unlinked in reverse order of
	 * creation; this is critical!
	 */
	lock_files = lcons(pstrdup(filename), lock_files);
}

/*
 * Create the data directory lockfile.
 *
 * When this is called, we must have already switched the working
 * directory to DataDir, so we can just use a relative path.  This
 * helps ensure that we are locking the directory we should be.
 *
 * Note that the socket directory path line is initially written as empty.
 * postmaster.c will rewrite it upon creating the first Unix socket.
 */
void
CreateDataDirLockFile(bool amPostmaster)
{
	CreateLockFile(DIRECTORY_LOCK_FILE, amPostmaster, "", true, DataDir);
}

/*
 * Create a lockfile for the specified Unix socket file.
 */
void
CreateSocketLockFile(const char *socketfile, bool amPostmaster,
					 const char *socketDir)
{
	char		lockfile[MAXPGPATH];

	snprintf(lockfile, sizeof(lockfile), "%s.lock", socketfile);
	CreateLockFile(lockfile, amPostmaster, socketDir, false, socketfile);
}

/*
 * TouchSocketLockFiles -- mark socket lock files as recently accessed
 *
 * This routine should be called every so often to ensure that the socket
 * lock files have a recent mod or access date.  That saves them
 * from being removed by overenthusiastic /tmp-directory-cleaner daemons.
 * (Another reason we should never have put the socket file in /tmp...)
 */
void
TouchSocketLockFiles(void)
{
	ListCell   *l;

	foreach(l, lock_files)
	{
		char	   *socketLockFile = (char *) lfirst(l);

		/* No need to touch the data directory lock file, we trust */
		if (strcmp(socketLockFile, DIRECTORY_LOCK_FILE) == 0)
			continue;

		/* we just ignore any error here */
		(void) utime(socketLockFile, NULL);
	}
}


/*
 * Add (or replace) a line in the data directory lock file.
 * The given string should not include a trailing newline.
 *
 * Note: because we don't truncate the file, if we were to rewrite a line
 * with less data than it had before, there would be garbage after the last
 * line.  While we could fix that by adding a truncate call, that would make
 * the file update non-atomic, which we'd rather avoid.  Therefore, callers
 * should endeavor never to shorten a line once it's been written.
 */
void
AddToDataDirLockFile(int target_line, const char *str)
{
	int			fd;
	int			len;
	int			lineno;
	char	   *srcptr;
	char	   *destptr;
	char		srcbuffer[BLCKSZ];
	char		destbuffer[BLCKSZ];

	fd = open(DIRECTORY_LOCK_FILE, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		return;
	}
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_ADDTODATADIR_READ);
	len = read(fd, srcbuffer, sizeof(srcbuffer) - 1);
	pgstat_report_wait_end();
	if (len < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return;
	}
	srcbuffer[len] = '\0';

	/*
	 * Advance over lines we are not supposed to rewrite, then copy them to
	 * destbuffer.
	 */
	srcptr = srcbuffer;
	for (lineno = 1; lineno < target_line; lineno++)
	{
		char	   *eol = strchr(srcptr, '\n');

		if (eol == NULL)
			break;				/* not enough lines in file yet */
		srcptr = eol + 1;
	}
	memcpy(destbuffer, srcbuffer, srcptr - srcbuffer);
	destptr = destbuffer + (srcptr - srcbuffer);

	/*
	 * Fill in any missing lines before the target line, in case lines are
	 * added to the file out of order.
	 */
	for (; lineno < target_line; lineno++)
	{
		if (destptr < destbuffer + sizeof(destbuffer))
			*destptr++ = '\n';
	}

	/*
	 * Write or rewrite the target line.
	 */
	snprintf(destptr, destbuffer + sizeof(destbuffer) - destptr, "%s\n", str);
	destptr += strlen(destptr);

	/*
	 * If there are more lines in the old file, append them to destbuffer.
	 */
	if ((srcptr = strchr(srcptr, '\n')) != NULL)
	{
		srcptr++;
		snprintf(destptr, destbuffer + sizeof(destbuffer) - destptr, "%s",
				 srcptr);
	}

	/*
	 * And rewrite the data.  Since we write in a single kernel call, this
	 * update should appear atomic to onlookers.
	 */
	len = strlen(destbuffer);
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_ADDTODATADIR_WRITE);
	if (pg_pwrite(fd, destbuffer, len, 0) != len)
	{
		pgstat_report_wait_end();
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return;
	}
	pgstat_report_wait_end();
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_ADDTODATADIR_SYNC);
	if (pg_fsync(fd) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
	}
	pgstat_report_wait_end();
	if (close(fd) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
	}
}


/*
 * Recheck that the data directory lock file still exists with expected
 * content.  Return true if the lock file appears OK, false if it isn't.
 *
 * We call this periodically in the postmaster.  The idea is that if the
 * lock file has been removed or replaced by another postmaster, we should
 * do a panic database shutdown.  Therefore, we should return true if there
 * is any doubt: we do not want to cause a panic shutdown unnecessarily.
 * Transient failures like EINTR or ENFILE should not cause us to fail.
 * (If there really is something wrong, we'll detect it on a future recheck.)
 */
bool
RecheckDataDirLockFile(void)
{
	int			fd;
	int			len;
	long		file_pid;
	char		buffer[BLCKSZ];

	fd = open(DIRECTORY_LOCK_FILE, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
	{
		/*
		 * There are many foreseeable false-positive error conditions.  For
		 * safety, fail only on enumerated clearly-something-is-wrong
		 * conditions.
		 */
		switch (errno)
		{
			case ENOENT:
			case ENOTDIR:
				/* disaster */
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								DIRECTORY_LOCK_FILE)));
				return false;
			default:
				/* non-fatal, at least for now */
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m; continuing anyway",
								DIRECTORY_LOCK_FILE)));
				return true;
		}
	}
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_RECHECKDATADIR_READ);
	len = read(fd, buffer, sizeof(buffer) - 1);
	pgstat_report_wait_end();
	if (len < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return true;			/* treat read failure as nonfatal */
	}
	buffer[len] = '\0';
	close(fd);
	file_pid = atol(buffer);
	if (file_pid == getpid())
		return true;			/* all is well */

	/* Trouble: someone's overwritten the lock file */
	ereport(LOG,
			(errmsg("lock file \"%s\" contains wrong PID: %ld instead of %ld",
					DIRECTORY_LOCK_FILE, file_pid, (long) getpid())));
	return false;
}


/*-------------------------------------------------------------------------
 *				Version checking support
 *-------------------------------------------------------------------------
 */

/*
 * Determine whether the PG_VERSION file in directory `path' indicates
 * a data version compatible with the version of this program.
 *
 * If compatible, return. Otherwise, ereport(FATAL).
 */
void
ValidatePgVersion(const char *path)
{
	char		full_path[MAXPGPATH];
	FILE	   *file;
	int			ret;
	long		file_major;
	long		my_major;
	char	   *endptr;
	char		file_version_string[64];
	const char *my_version_string = PG_VERSION;

	my_major = strtol(my_version_string, &endptr, 10);

	snprintf(full_path, sizeof(full_path), "%s/PG_VERSION", path);

	file = AllocateFile(full_path, "r");
	if (!file)
	{
		if (errno == ENOENT)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" is not a valid data directory",
							path),
					 errdetail("File \"%s\" is missing.", full_path)));
		else
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", full_path)));
	}

	file_version_string[0] = '\0';
	ret = fscanf(file, "%63s", file_version_string);
	file_major = strtol(file_version_string, &endptr, 10);

	if (ret != 1 || endptr == file_version_string)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a valid data directory",
						path),
				 errdetail("File \"%s\" does not contain valid data.",
						   full_path),
				 errhint("You might need to initdb.")));

	FreeFile(file);

	if (my_major != file_major)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The data directory was initialized by PostgreSQL version %s, "
						   "which is not compatible with this version %s.",
						   file_version_string, my_version_string)));
}

/*-------------------------------------------------------------------------
 *				Library preload support
 *-------------------------------------------------------------------------
 */

/*
 * GUC variables: lists of library names to be preloaded at postmaster
 * start and at backend start
 */
char	   *session_preload_libraries_string = NULL;
char	   *shared_preload_libraries_string = NULL;
char	   *local_preload_libraries_string = NULL;

/* Flag telling that we are loading shared_preload_libraries */
bool		process_shared_preload_libraries_in_progress = false;
bool		process_shared_preload_libraries_done = false;

shmem_request_hook_type shmem_request_hook = NULL;
bool		process_shmem_requests_in_progress = false;

/*
 * load the shared libraries listed in 'libraries'
 *
 * 'gucname': name of GUC variable, for error reports
 * 'restricted': if true, force libraries to be in $libdir/plugins/
 */
static void
load_libraries(const char *libraries, const char *gucname, bool restricted)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;

	if (libraries == NULL || libraries[0] == '\0')
		return;					/* nothing to do */

	/* Need a modifiable copy of string */
	rawstring = pstrdup(libraries);

	/* Parse string into list of filename paths */
	if (!SplitDirectoriesString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		list_free_deep(elemlist);
		pfree(rawstring);
		ereport(LOG,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid list syntax in parameter \"%s\"",
						gucname)));
		return;
	}

	foreach(l, elemlist)
	{
		/* Note that filename was already canonicalized */
		char	   *filename = (char *) lfirst(l);
		char	   *expanded = NULL;

		/* If restricting, insert $libdir/plugins if not mentioned already */
		if (restricted && first_dir_separator(filename) == NULL)
		{
			expanded = psprintf("$libdir/plugins/%s", filename);
			filename = expanded;
		}
		load_file(filename, restricted);
		ereport(DEBUG1,
				(errmsg_internal("loaded library \"%s\"", filename)));
		if (expanded)
			pfree(expanded);
	}

	list_free_deep(elemlist);
	pfree(rawstring);
}

/*
 * process any libraries that should be preloaded at postmaster start
 */
void
process_shared_preload_libraries(void)
{
	process_shared_preload_libraries_in_progress = true;
	load_libraries(shared_preload_libraries_string,
				   "shared_preload_libraries",
				   false);
	process_shared_preload_libraries_in_progress = false;
	process_shared_preload_libraries_done = true;
}

/*
 * process any libraries that should be preloaded at backend start
 */
void
process_session_preload_libraries(void)
{
	load_libraries(session_preload_libraries_string,
				   "session_preload_libraries",
				   false);
	load_libraries(local_preload_libraries_string,
				   "local_preload_libraries",
				   true);
}

/*
 * process any shared memory requests from preloaded libraries
 */
void
process_shmem_requests(void)
{
	process_shmem_requests_in_progress = true;
	if (shmem_request_hook)
		shmem_request_hook();
	process_shmem_requests_in_progress = false;
}

void
pg_bindtextdomain(const char *domain)
{
#ifdef ENABLE_NLS
	if (my_exec_path[0] != '\0')
	{
		char		locale_path[MAXPGPATH];

		get_locale_path(my_exec_path, locale_path);
		bindtextdomain(domain, locale_path);
		pg_bind_textdomain_codeset(domain);
	}
#endif
}
