/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/init/miscinit.c,v 1.159.2.1 2008/01/03 21:23:45 tgl Exp $
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

#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/syscache.h"


#define DIRECTORY_LOCK_FILE		"postmaster.pid"

ProcessingMode Mode = InitProcessing;

/* Note: we rely on this to initialize as zeroes */
static char socketLockFile[MAXPGPATH];


/* ----------------------------------------------------------------
 *		ignoring system indexes support stuff
 *
 * NOTE: "ignoring system indexes" means we do not use the system indexes
 * for lookups (either in hardwired catalog accesses or in planner-generated
 * plans).	We do, however, still update the indexes when a catalog
 * modification is made.
 * ----------------------------------------------------------------
 */

bool		IgnoreSystemIndexes = false;

/* ----------------------------------------------------------------
 *		system index reindexing support
 *
 * When we are busy reindexing a system index, this code provides support
 * for preventing catalog lookups from using that index.
 * ----------------------------------------------------------------
 */

static Oid	currentlyReindexedHeap = InvalidOid;
static Oid	currentlyReindexedIndex = InvalidOid;

/*
 * ReindexIsProcessingHeap
 *		True if heap specified by OID is currently being reindexed.
 */
bool
ReindexIsProcessingHeap(Oid heapOid)
{
	return heapOid == currentlyReindexedHeap;
}

/*
 * ReindexIsProcessingIndex
 *		True if index specified by OID is currently being reindexed.
 */
bool
ReindexIsProcessingIndex(Oid indexOid)
{
	return indexOid == currentlyReindexedIndex;
}

/*
 * SetReindexProcessing
 *		Set flag that specified heap/index are being reindexed.
 */
void
SetReindexProcessing(Oid heapOid, Oid indexOid)
{
	Assert(OidIsValid(heapOid) && OidIsValid(indexOid));
	/* Reindexing is not re-entrant. */
	if (OidIsValid(currentlyReindexedIndex))
		elog(ERROR, "cannot reindex while reindexing");
	currentlyReindexedHeap = heapOid;
	currentlyReindexedIndex = indexOid;
}

/*
 * ResetReindexProcessing
 *		Unset reindexing status.
 */
void
ResetReindexProcessing(void)
{
	currentlyReindexedHeap = InvalidOid;
	currentlyReindexedIndex = InvalidOid;
}

/* ----------------------------------------------------------------
 *				database path / name support stuff
 * ----------------------------------------------------------------
 */

void
SetDatabasePath(const char *path)
{
	if (DatabasePath)
	{
		free(DatabasePath);
		DatabasePath = NULL;
	}
	/* use strdup since this is done before memory contexts are set up */
	if (path)
	{
		DatabasePath = strdup(path);
		AssertState(DatabasePath);
	}
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

/*
 * If the given pathname isn't already absolute, make it so, interpreting
 * it relative to the current working directory.
 *
 * Also canonicalizes the path.  The result is always a malloc'd copy.
 *
 * Note: interpretation of relative-path arguments during postmaster startup
 * should happen before doing ChangeToDataDir(), else the user will probably
 * not like the results.
 */
char *
make_absolute_path(const char *path)
{
	char	   *new;

	/* Returning null for null input is convenient for some callers */
	if (path == NULL)
		return NULL;

	if (!is_absolute_path(path))
	{
		char	   *buf;
		size_t		buflen;

		buflen = MAXPGPATH;
		for (;;)
		{
			buf = malloc(buflen);
			if (!buf)
				ereport(FATAL,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));

			if (getcwd(buf, buflen))
				break;
			else if (errno == ERANGE)
			{
				free(buf);
				buflen *= 2;
				continue;
			}
			else
			{
				free(buf);
				elog(FATAL, "could not get current working directory: %m");
			}
		}

		new = malloc(strlen(buf) + strlen(path) + 2);
		if (!new)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		sprintf(new, "%s/%s", buf, path);
		free(buf);
	}
	else
	{
		new = strdup(path);
		if (!new)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	/* Make sure punctuation is canonical, too */
	canonicalize_path(new);

	return new;
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
 * SecurityDefinerContext is TRUE if we are within a SECURITY DEFINER function
 * or another context that temporarily changes CurrentUserId.
 * ----------------------------------------------------------------
 */
static Oid	AuthenticatedUserId = InvalidOid;
static Oid	SessionUserId = InvalidOid;
static Oid	OuterUserId = InvalidOid;
static Oid	CurrentUserId = InvalidOid;

/* We also have to remember the superuser state of some of these levels */
static bool AuthenticatedUserIsSuperuser = false;
static bool SessionUserIsSuperuser = false;

static bool SecurityDefinerContext = false;

/* We also remember if a SET ROLE is currently active */
static bool SetRoleIsActive = false;


/*
 * GetUserId - get the current effective user ID.
 *
 * Note: there's no SetUserId() anymore; use SetUserIdAndContext().
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
	AssertState(!SecurityDefinerContext);
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
	AssertState(!SecurityDefinerContext);
	AssertArg(OidIsValid(userid));
	SessionUserId = userid;
	SessionUserIsSuperuser = is_superuser;
	SetRoleIsActive = false;

	/* We force the effective user IDs to match, too */
	OuterUserId = userid;
	CurrentUserId = userid;
}


/*
 * GetUserIdAndContext/SetUserIdAndContext - get/set the current user ID
 * and the SecurityDefinerContext flag.
 *
 * Unlike GetUserId, GetUserIdAndContext does *not* Assert that the current
 * value of CurrentUserId is valid; nor does SetUserIdAndContext require
 * the new value to be valid.  In fact, these routines had better not
 * ever throw any kind of error.  This is because they are used by
 * StartTransaction and AbortTransaction to save/restore the settings,
 * and during the first transaction within a backend, the value to be saved
 * and perhaps restored is indeed invalid.  We have to be able to get
 * through AbortTransaction without asserting in case InitPostgres fails.
 */
void
GetUserIdAndContext(Oid *userid, bool *sec_def_context)
{
	*userid = CurrentUserId;
	*sec_def_context = SecurityDefinerContext;
}

void
SetUserIdAndContext(Oid userid, bool sec_def_context)
{
	CurrentUserId = userid;
	SecurityDefinerContext = sec_def_context;
}


/*
 * InSecurityDefinerContext - are we inside a SECURITY DEFINER context?
 */
bool
InSecurityDefinerContext(void)
{
	return SecurityDefinerContext;
}


/*
 * Initialize user identity during normal backend startup
 */
void
InitializeSessionUserId(const char *rolename)
{
	HeapTuple	roleTup;
	Form_pg_authid rform;
	Datum		datum;
	bool		isnull;
	Oid			roleid;

	/*
	 * Don't do scans if we're bootstrapping, none of the system catalogs
	 * exist yet, and they should be owned by postgres anyway.
	 */
	AssertState(!IsBootstrapProcessingMode());

	/* call only once */
	AssertState(!OidIsValid(AuthenticatedUserId));

	roleTup = SearchSysCache(AUTHNAME,
							 PointerGetDatum(rolename),
							 0, 0, 0);
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
	 *
	 * We do not enforce them for the autovacuum process either.
	 */
	if (IsUnderPostmaster && !IsAutoVacuumProcess())
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
		 * checking for other PGPROCs.	If two backends did this at about the
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

	/*
	 * Set up user-specific configuration variables.  This is a good place to
	 * do it so we don't have to read pg_authid twice during session startup.
	 */
	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolconfig, &isnull);
	if (!isnull)
	{
		ArrayType  *a = DatumGetArrayTypeP(datum);

		ProcessGUCArray(a, PGC_S_USER);
	}

	ReleaseSysCache(roleTup);
}


/*
 * Initialize user identity during special backend startup
 */
void
InitializeSessionUserIdStandalone(void)
{
	/* This function should only be called in a single-user backend. */
	AssertState(!IsUnderPostmaster || IsAutoVacuumProcess());

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
 * session user authorization.	In this case the is_superuser argument
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

	tuple = SearchSysCache(AUTHOID,
						   ObjectIdGetDatum(roleid),
						   0, 0, 0);
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
 * ($DATADIR/postmaster.pid) and a Unix-socket-file lockfile ($SOCKFILE.lock).
 * Both kinds of files contain the same info:
 *
 *		Owning process' PID
 *		Data directory path
 *
 * By convention, the owning process' PID is negated if it is a standalone
 * backend rather than a postmaster.  This is just for informational purposes.
 * The path is also just for informational purposes (so that a socket lockfile
 * can be more easily traced to the associated postmaster).
 *
 * A data-directory lockfile can optionally contain a third line, containing
 * the key and ID for the shared memory block used by this postmaster.
 *
 * On successful lockfile creation, a proc_exit callback to remove the
 * lockfile is automatically created.
 *-------------------------------------------------------------------------
 */

/*
 * proc_exit callback to remove a lockfile.
 */
static void
UnlinkLockFile(int status, Datum filename)
{
	char	   *fname = (char *) DatumGetPointer(filename);

	if (fname != NULL)
	{
		if (unlink(fname) != 0)
		{
			/* Should we complain if the unlink fails? */
		}
		free(fname);
	}
}

/*
 * Create a lockfile.
 *
 * filename is the name of the lockfile to create.
 * amPostmaster is used to determine how to encode the output PID.
 * isDDLock and refName are used to determine what error message to produce.
 */
static void
CreateLockFile(const char *filename, bool amPostmaster,
			   bool isDDLock, const char *refName)
{
	int			fd;
	char		buffer[MAXPGPATH + 100];
	int			ntries;
	int			len;
	int			encoded_pid;
	pid_t		other_pid;
	pid_t		my_pid = getpid();

	/*
	 * We need a loop here because of race conditions.	But don't loop forever
	 * (for example, a non-writable $PGDATA directory might cause a failure
	 * that won't go away).  100 tries seems like plenty.
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 *
		 * Think not to make the file protection weaker than 0600.	See
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
		 * If the PID in the lockfile is our own PID or our parent's PID, then
		 * the file must be stale (probably left over from a previous system
		 * boot cycle).  We need this test because of the likelihood that a
		 * reboot will assign exactly the same PID as we had in the previous
		 * reboot.	Also, if there is just one more process launch in this
		 * reboot than in the previous one, the lockfile might mention our
		 * parent's PID.  We can reject that since we'd never be launched
		 * directly by a competing postmaster.	We can't detect grandparent
		 * processes unfortunately, but if the init script is written
		 * carefully then all but the immediate parent shell will be
		 * root-owned processes and so the kill test will fail with EPERM.
		 *
		 * We can treat the EPERM-error case as okay because that error
		 * implies that the existing process has a different userid than we
		 * do, which means it cannot be a competing postmaster.  A postmaster
		 * cannot successfully attach to a data directory owned by a userid
		 * other than its own.	(This is now checked directly in
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
		 *
		 * Windows hasn't got getppid(), but doesn't need it since it's not
		 * using real kill() either...
		 *
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist.
		 */
		if (other_pid != my_pid
#ifndef WIN32
			&& other_pid != getppid()
#endif
			)
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
		 * No, the creating process did not exist.	However, it could be that
		 * the postmaster crashed (or more likely was kill -9'd by a clueless
		 * admin) but has left orphan backends behind.	Check for this by
		 * looking to see if there is an associated shmem segment that is
		 * still in use.
		 */
		if (isDDLock)
		{
			char	   *ptr;
			unsigned long id1,
						id2;

			ptr = strchr(buffer, '\n');
			if (ptr != NULL &&
				(ptr = strchr(ptr + 1, '\n')) != NULL)
			{
				ptr++;
				if (sscanf(ptr, "%lu %lu", &id1, &id2) == 2)
				{
					if (PGSharedMemoryIsInUse(id1, id2))
						ereport(FATAL,
								(errcode(ERRCODE_LOCK_FILE_EXISTS),
								 errmsg("pre-existing shared memory block "
										"(key %lu, ID %lu) is still in use",
										id1, id2),
								 errhint("If you're sure there are no old "
									"server processes still running, remove "
										 "the shared memory block with "
									  "the command \"ipcclean\", \"ipcrm\", "
										 "or just delete the file \"%s\".",
										 filename)));
				}
			}
		}

		/*
		 * Looks like nobody's home.  Unlink the file and try again to create
		 * it.	Need a loop because of possible race condition against other
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
	 * Successfully created the file, now fill it.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n%s\n",
			 amPostmaster ? (int) my_pid : -((int) my_pid),
			 DataDir);
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
	if (close(fd))
	{
		int			save_errno = errno;

		unlink(filename);
		errno = save_errno;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}

	/*
	 * Arrange for automatic removal of lockfile at proc_exit.
	 */
	on_proc_exit(UnlinkLockFile, PointerGetDatum(strdup(filename)));
}

/*
 * Create the data directory lockfile.
 *
 * When this is called, we must have already switched the working
 * directory to DataDir, so we can just use a relative path.  This
 * helps ensure that we are locking the directory we should be.
 */
void
CreateDataDirLockFile(bool amPostmaster)
{
	CreateLockFile(DIRECTORY_LOCK_FILE, amPostmaster, true, DataDir);
}

/*
 * Create a lockfile for the specified Unix socket file.
 */
void
CreateSocketLockFile(const char *socketfile, bool amPostmaster)
{
	char		lockfile[MAXPGPATH];

	snprintf(lockfile, sizeof(lockfile), "%s.lock", socketfile);
	CreateLockFile(lockfile, amPostmaster, false, socketfile);
	/* Save name of lockfile for TouchSocketLockFile */
	strcpy(socketLockFile, lockfile);
}

/*
 * TouchSocketLockFile -- mark socket lock file as recently accessed
 *
 * This routine should be called every so often to ensure that the lock file
 * has a recent mod or access date.  That saves it
 * from being removed by overenthusiastic /tmp-directory-cleaner daemons.
 * (Another reason we should never have put the socket file in /tmp...)
 */
void
TouchSocketLockFile(void)
{
	/* Do nothing if we did not create a socket... */
	if (socketLockFile[0] != '\0')
	{
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
 * Append information about a shared memory segment to the data directory
 * lock file.
 *
 * This may be called multiple times in the life of a postmaster, if we
 * delete and recreate shmem due to backend crash.	Therefore, be prepared
 * to overwrite existing information.  (As of 7.1, a postmaster only creates
 * one shm seg at a time; but for the purposes here, if we did have more than
 * one then any one of them would do anyway.)
 */
void
RecordSharedMemoryInLockFile(unsigned long id1, unsigned long id2)
{
	int			fd;
	int			len;
	char	   *ptr;
	char		buffer[BLCKSZ];

	fd = open(DIRECTORY_LOCK_FILE, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		return;
	}
	len = read(fd, buffer, sizeof(buffer) - 100);
	if (len < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return;
	}
	buffer[len] = '\0';

	/*
	 * Skip over first two lines (PID and path).
	 */
	ptr = strchr(buffer, '\n');
	if (ptr == NULL ||
		(ptr = strchr(ptr + 1, '\n')) == NULL)
	{
		elog(LOG, "bogus data in \"%s\"", DIRECTORY_LOCK_FILE);
		close(fd);
		return;
	}
	ptr++;

	/*
	 * Append key information.	Format to try to keep it the same length
	 * always (trailing junk won't hurt, but might confuse humans).
	 */
	sprintf(ptr, "%9lu %9lu\n", id1, id2);

	/*
	 * And rewrite the data.  Since we write in a single kernel call, this
	 * update should appear atomic to onlookers.
	 */
	len = strlen(buffer);
	errno = 0;
	if (lseek(fd, (off_t) 0, SEEK_SET) != 0 ||
		(int) write(fd, buffer, len) != len)
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
	if (close(fd))
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
				 errhint("You may need to initdb.")));

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
char	   *shared_preload_libraries_string = NULL;
char	   *local_preload_libraries_string = NULL;

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

			expanded = palloc(strlen("$libdir/plugins/") + strlen(filename) + 1);
			strcpy(expanded, "$libdir/plugins/");
			strcat(expanded, filename);
			pfree(filename);
			filename = expanded;
		}
		load_file(filename, restricted);
		ereport(LOG,
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
	load_libraries(shared_preload_libraries_string,
				   "shared_preload_libraries",
				   false);
}

/*
 * process any libraries that should be preloaded at backend start
 */
void
process_local_preload_libraries(void)
{
	load_libraries(local_preload_libraries_string,
				   "local_preload_libraries",
				   true);
}
