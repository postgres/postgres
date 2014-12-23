/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


#define DIRECTORY_LOCK_FILE		"postmaster.pid"

ProcessingMode Mode = InitProcessing;

/* List of lock files to be removed at proc exit */
static List *lock_files = NIL;


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
 * Set data directory, but make sure it's an absolute path.  Use this,
 * never set DataDir directly.
 */
void
SetDataDir(const char *dir)
{
	char	   *new;

	AssertArg(dir);

	/* If presented path is relative, convert to absolute */
	new = make_absolute_path(dir);

	if (DataDir)
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
	AssertState(DataDir);

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
 * changed by SET SESSION AUTHORIZATION (if AuthenticatedUserIsSuperuser).
 * This is the ID reported by the SESSION_USER SQL function.
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

/* We also have to remember the superuser state of some of these levels */
static bool AuthenticatedUserIsSuperuser = false;
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
	AssertState(OidIsValid(CurrentUserId));
	return CurrentUserId;
}


/*
 * GetOuterUserId/SetOuterUserId - get/set the outer-level user ID.
 */
Oid
GetOuterUserId(void)
{
	AssertState(OidIsValid(OuterUserId));
	return OuterUserId;
}


static void
SetOuterUserId(Oid userid)
{
	AssertState(SecurityRestrictionContext == 0);
	AssertArg(OidIsValid(userid));
	OuterUserId = userid;

	/* We force the effective user ID to match, too */
	CurrentUserId = userid;
}


/*
 * GetSessionUserId/SetSessionUserId - get/set the session user ID.
 */
Oid
GetSessionUserId(void)
{
	AssertState(OidIsValid(SessionUserId));
	return SessionUserId;
}


static void
SetSessionUserId(Oid userid, bool is_superuser)
{
	AssertState(SecurityRestrictionContext == 0);
	AssertArg(OidIsValid(userid));
	SessionUserId = userid;
	SessionUserIsSuperuser = is_superuser;
	SetRoleIsActive = false;

	/* We force the effective user IDs to match, too */
	OuterUserId = userid;
	CurrentUserId = userid;
}

/*
 * GetAuthenticatedUserId - get the authenticated user ID
 */
Oid
GetAuthenticatedUserId(void)
{
	AssertState(OidIsValid(AuthenticatedUserId));
	return AuthenticatedUserId;
}


/*
 * GetUserIdAndSecContext/SetUserIdAndSecContext - get/set the current user ID
 * and the SecurityRestrictionContext flags.
 *
 * Currently there are two valid bits in SecurityRestrictionContext:
 *
 * SECURITY_LOCAL_USERID_CHANGE indicates that we are inside an operation
 * that is temporarily changing CurrentUserId via these functions.  This is
 * needed to indicate that the actual value of CurrentUserId is not in sync
 * with guc.c's internal state, so SET ROLE has to be disallowed.
 *
 * SECURITY_RESTRICTED_OPERATION indicates that we are inside an operation
 * that does not wish to trust called user-defined functions at all.  This
 * bit prevents not only SET ROLE, but various other changes of session state
 * that normally is unprotected but might possibly be used to subvert the
 * calling session later.  An example is replacing an existing prepared
 * statement with new code, which will then be executed with the outer
 * session's permissions when the prepared statement is next used.  Since
 * these restrictions are fairly draconian, we apply them only in contexts
 * where the called functions are really supposed to be side-effect-free
 * anyway, such as VACUUM/ANALYZE/REINDEX.
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
InitializeSessionUserId(const char *rolename)
{
	HeapTuple	roleTup;
	Form_pg_authid rform;
	Oid			roleid;

	/*
	 * Don't do scans if we're bootstrapping, none of the system catalogs
	 * exist yet, and they should be owned by postgres anyway.
	 */
	AssertState(!IsBootstrapProcessingMode());

	/* call only once */
	AssertState(!OidIsValid(AuthenticatedUserId));

	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(rolename));
	if (!HeapTupleIsValid(roleTup))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("role \"%s\" does not exist", rolename)));

	rform = (Form_pg_authid) GETSTRUCT(roleTup);
	roleid = HeapTupleGetOid(roleTup);

	AuthenticatedUserId = roleid;
	AuthenticatedUserIsSuperuser = rform->rolsuper;

	/* This sets OuterUserId/CurrentUserId too */
	SetSessionUserId(roleid, AuthenticatedUserIsSuperuser);

	/* Also mark our PGPROC entry with the authenticated user id */
	/* (We assume this is an atomic store so no lock is needed) */
	MyProc->roleId = roleid;

	/*
	 * These next checks are not enforced when in standalone mode, so that
	 * there is a way to recover from sillinesses like "UPDATE pg_authid SET
	 * rolcanlogin = false;".
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Is role allowed to login at all?
		 */
		if (!rform->rolcanlogin)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role \"%s\" is not permitted to log in",
							rolename)));

		/*
		 * Check connection limit for this role.
		 *
		 * There is a race condition here --- we create our PGPROC before
		 * checking for other PGPROCs.  If two backends did this at about the
		 * same time, they might both think they were over the limit, while
		 * ideally one should succeed and one fail.  Getting that to work
		 * exactly seems more trouble than it is worth, however; instead we
		 * just document that the connection limit is approximate.
		 */
		if (rform->rolconnlimit >= 0 &&
			!AuthenticatedUserIsSuperuser &&
			CountUserBackends(roleid) > rform->rolconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for role \"%s\"",
							rolename)));
	}

	/* Record username and superuser status as GUC settings too */
	SetConfigOption("session_authorization", rolename,
					PGC_BACKEND, PGC_S_OVERRIDE);
	SetConfigOption("is_superuser",
					AuthenticatedUserIsSuperuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);

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
	 * workers, and in background workers.
	 */
	AssertState(!IsUnderPostmaster || IsAutoVacuumWorkerProcess() || IsBackgroundWorker);

	/* call only once */
	AssertState(!OidIsValid(AuthenticatedUserId));

	AuthenticatedUserId = BOOTSTRAP_SUPERUSERID;
	AuthenticatedUserIsSuperuser = true;

	SetSessionUserId(BOOTSTRAP_SUPERUSERID, true);
}


/*
 * Change session auth ID while running
 *
 * Only a superuser may set auth ID to something other than himself.  Note
 * that in case of multiple SETs in a single session, the original userid's
 * superuserness is what matters.  But we set the GUC variable is_superuser
 * to indicate whether the *current* session userid is a superuser.
 *
 * Note: this is not an especially clean place to do the permission check.
 * It's OK because the check does not require catalog access and can't
 * fail during an end-of-transaction GUC reversion, but we may someday
 * have to push it up into assign_session_authorization.
 */
void
SetSessionAuthorization(Oid userid, bool is_superuser)
{
	/* Must have authenticated already, else can't make permission check */
	AssertState(OidIsValid(AuthenticatedUserId));

	if (userid != AuthenticatedUserId &&
		!AuthenticatedUserIsSuperuser)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to set session authorization")));

	SetSessionUserId(userid, is_superuser);

	SetConfigOption("is_superuser",
					is_superuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);
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
	 * If SessionUserId hasn't been set yet, just do nothing --- the eventual
	 * SetSessionUserId call will fix everything.  This is needed since we
	 * will get called during GUC initialization.
	 */
	if (!OidIsValid(roleid))
	{
		if (!OidIsValid(SessionUserId))
			return;

		roleid = SessionUserId;
		is_superuser = SessionUserIsSuperuser;

		SetRoleIsActive = false;
	}
	else
		SetRoleIsActive = true;

	SetOuterUserId(roleid);

	SetConfigOption("is_superuser",
					is_superuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);
}


/*
 * Get user name from user oid
 */
char *
GetUserNameFromId(Oid roleid)
{
	HeapTuple	tuple;
	char	   *result;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid role OID: %u", roleid)));

	result = pstrdup(NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname));

	ReleaseSysCache(tuple);
	return result;
}


/*-------------------------------------------------------------------------
 *				Interlock-file support
 *
 * These routines are used to create both a data-directory lockfile
 * ($DATADIR/postmaster.pid) and Unix-socket-file lockfiles ($SOCKFILE.lock).
 * Both kinds of files contain the same info initially, although we can add
 * more information to a data-directory lockfile after it's created, using
 * AddToDataDirLockFile().  See miscadmin.h for documentation of the contents
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
		 * Think not to make the file protection weaker than 0600.  See
		 * comments below.
		 */
		fd = open(filename, O_RDWR | O_CREAT | O_EXCL, 0600);
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
		fd = open(filename, O_RDONLY, 0600);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* race condition; try again */
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open lock file \"%s\": %m",
							filename)));
		}
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read lock file \"%s\": %m",
							filename)));
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
		 * other than its own.  (This is now checked directly in
		 * checkDataDir(), but has been true for a long time because of the
		 * restriction that the data directory isn't group- or
		 * world-accessible.)  Also, since we create the lockfiles mode 600,
		 * we'd have failed above if the lockfile belonged to another userid
		 * --- which means that whatever process kill() is reporting about
		 * isn't the one that made the lockfile.  (NOTE: this last
		 * consideration is the only one that keeps us from blowing away a
		 * Unix socket file belonging to an instance of Postgres being run by
		 * someone else, at least on machines where /tmp hasn't got a
		 * stickybit.)
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
							 errmsg("pre-existing shared memory block "
									"(key %lu, ID %lu) is still in use",
									id1, id2),
							 errhint("If you're sure there are no old "
									 "server processes still running, remove "
									 "the shared memory block "
									 "or just delete the file \"%s\".",
									 filename)));
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
	 * Successfully created the file, now fill it.  See comment in miscadmin.h
	 * about the contents.  Note that we write the same first five lines into
	 * both datadir and socket lockfiles; although more stuff may get added to
	 * the datadir lockfile later.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n%s\n%ld\n%d\n%s\n",
			 amPostmaster ? (int) my_pid : -((int) my_pid),
			 DataDir,
			 (long) MyStartTime,
			 PostPortNumber,
			 socketDir);

	/*
	 * In a standalone backend, the next line (LOCK_FILE_LINE_LISTEN_ADDR)
	 * will never receive data, so fill it in as empty now.
	 */
	if (isDDLock && !amPostmaster)
		strlcat(buffer, "\n", sizeof(buffer));

	errno = 0;
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

	lock_files = lappend(lock_files, pstrdup(filename));
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

		/*
		 * utime() is POSIX standard, utimes() is a common alternative; if we
		 * have neither, fall back to actually reading the file (which only
		 * sets the access time not mod time, but that should be enough in
		 * most cases).  In all paths, we ignore errors.
		 */
#ifdef HAVE_UTIME
		utime(socketLockFile, NULL);
#else							/* !HAVE_UTIME */
#ifdef HAVE_UTIMES
		utimes(socketLockFile, NULL);
#else							/* !HAVE_UTIMES */
		int			fd;
		char		buffer[1];

		fd = open(socketLockFile, O_RDONLY | PG_BINARY, 0);
		if (fd >= 0)
		{
			read(fd, buffer, sizeof(buffer));
			close(fd);
		}
#endif   /* HAVE_UTIMES */
#endif   /* HAVE_UTIME */
	}
}


/*
 * Add (or replace) a line in the data directory lock file.
 * The given string should not include a trailing newline.
 *
 * Note: because we don't truncate the file, if we were to rewrite a line
 * with less data than it had before, there would be garbage after the last
 * line.  We don't ever actually do that, so not worth adding another kernel
 * call to cover the possibility.
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
	len = read(fd, srcbuffer, sizeof(srcbuffer) - 1);
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
		if ((srcptr = strchr(srcptr, '\n')) == NULL)
		{
			elog(LOG, "incomplete data in \"%s\": found only %d newlines while trying to add line %d",
				 DIRECTORY_LOCK_FILE, lineno - 1, target_line);
			close(fd);
			return;
		}
		srcptr++;
	}
	memcpy(destbuffer, srcbuffer, srcptr - srcbuffer);
	destptr = destbuffer + (srcptr - srcbuffer);

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
	if (lseek(fd, (off_t) 0, SEEK_SET) != 0 ||
		(int) write(fd, destbuffer, len) != len)
	{
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
	if (pg_fsync(fd) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
	}
	if (close(fd) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
	}
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
	long		file_major,
				file_minor;
	long		my_major = 0,
				my_minor = 0;
	char	   *endptr;
	const char *version_string = PG_VERSION;

	my_major = strtol(version_string, &endptr, 10);
	if (*endptr == '.')
		my_minor = strtol(endptr + 1, NULL, 10);

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

	ret = fscanf(file, "%ld.%ld", &file_major, &file_minor);
	if (ret != 2)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a valid data directory",
						path),
				 errdetail("File \"%s\" does not contain valid data.",
						   full_path),
				 errhint("You might need to initdb.")));

	FreeFile(file);

	if (my_major != file_major || my_minor != file_minor)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The data directory was initialized by PostgreSQL version %ld.%ld, "
						   "which is not compatible with this version %s.",
						   file_major, file_minor, version_string)));
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

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		list_free(elemlist);
		ereport(LOG,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid list syntax in parameter \"%s\"",
						gucname)));
		return;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);
		char	   *filename;

		filename = pstrdup(tok);
		canonicalize_path(filename);
		/* If restricting, insert $libdir/plugins if not mentioned already */
		if (restricted && first_dir_separator(filename) == NULL)
		{
			char	   *expanded;

			expanded = psprintf("$libdir/plugins/%s", filename);
			pfree(filename);
			filename = expanded;
		}
		load_file(filename, restricted);
		ereport(DEBUG1,
				(errmsg("loaded library \"%s\"", filename)));
		pfree(filename);
	}

	pfree(rawstring);
	list_free(elemlist);
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
