/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/miscinit.c,v 1.104 2003/06/27 19:08:37 tgl Exp $
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
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#include "catalog/catname.h"
#include "catalog/pg_shadow.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


ProcessingMode Mode = InitProcessing;

/* Note: we rely on these to initialize as zeroes */
static char directoryLockFile[MAXPGPATH];
static char socketLockFile[MAXPGPATH];

#ifdef CYR_RECODE
static unsigned char RecodeForwTable[128];
static unsigned char RecodeBackTable[128];

static void GetCharSetByHost(char *TableName, int host, const char *DataDir);
#endif


/* ----------------------------------------------------------------
 *		ignoring system indexes support stuff
 * ----------------------------------------------------------------
 */

static bool isIgnoringSystemIndexes = false;

/*
 * IsIgnoringSystemIndexes
 *		True if ignoring system indexes.
 */
bool
IsIgnoringSystemIndexes()
{
	return isIgnoringSystemIndexes;
}

/*
 * IgnoreSystemIndexes
 *	Set true or false whether PostgreSQL ignores system indexes.
 *
 */
void
IgnoreSystemIndexes(bool mode)
{
	isIgnoringSystemIndexes = mode;
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
	int			newlen;

	AssertArg(dir);

	/* If presented path is relative, convert to absolute */
	if (!is_absolute_path(dir))
	{
		char	   *buf;
		size_t		buflen;

		buflen = MAXPGPATH;
		for (;;)
		{
			buf = malloc(buflen);
			if (!buf)
				elog(FATAL, "out of memory");

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
				elog(FATAL, "cannot get current working directory: %m");
			}
		}

		new = malloc(strlen(buf) + 1 + strlen(dir) + 1);
		if (!new)
			elog(FATAL, "out of memory");
		sprintf(new, "%s/%s", buf, dir);
		free(buf);
	}
	else
	{
		new = strdup(dir);
		if (!new)
			elog(FATAL, "out of memory");
	}

	/*
	 * Strip any trailing slash.  Not strictly necessary, but avoids
	 * generating funny-looking paths to individual files.
	 */
	newlen = strlen(new);
	if (newlen > 1 && new[newlen - 1] == '/'
#ifdef WIN32
		|| new[newlen - 1] == '\\'
#endif
		)
		new[newlen - 1] = '\0';

	if (DataDir)
		free(DataDir);
	DataDir = new;
}

/* ----------------------------------------------------------------
 *				CYR_RECODE support
 * ----------------------------------------------------------------
 */

#ifdef CYR_RECODE

void
SetCharSet(void)
{
	FILE	   *file;
	char	   *filename;
	char	   *map_file;
	char		buf[MAX_TOKEN];
	int			i;
	unsigned char FromChar,
				ToChar;
	char		ChTable[MAX_TOKEN];

	for (i = 0; i < 128; i++)
	{
		RecodeForwTable[i] = i + 128;
		RecodeBackTable[i] = i + 128;
	}

	if (IsUnderPostmaster)
	{
		GetCharSetByHost(ChTable, MyProcPort->raddr.in.sin_addr.s_addr, DataDir);
		filename = ChTable;
	}
	else
		filename = getenv("PG_RECODETABLE");

	if (filename && *filename != '\0')
	{
		map_file = palloc(strlen(DataDir) + strlen(filename) + 2);
		sprintf(map_file, "%s/%s", DataDir, filename);
		file = AllocateFile(map_file, "r");
		pfree(map_file);
		if (file == NULL)
			return;

		while (!feof(file))
		{
			next_token(file, buf, sizeof(buf));
			if (buf[0] != '\0')
			{
				FromChar = strtoul(buf, 0, 0);
				/* Read the ToChar */
				next_token(file, buf, sizeof(buf));
				if (buf[0] != '\0')
				{
					ToChar = strtoul(buf, 0, 0);
					RecodeForwTable[FromChar - 128] = ToChar;
					RecodeBackTable[ToChar - 128] = FromChar;

					/* read to EOL */
					while (!feof(file) && buf[0])
					{
						next_token(file, buf, sizeof(buf));
						elog(LOG, "SetCharSet: unknown tag %s in file %s",
							 buf, filename);
					}
				}
			}
		}
		FreeFile(file);
	}
}


char *
convertstr(unsigned char *buff, int len, int dest)
{
	int			i;
	char	   *ch = buff;

	for (i = 0; i < len; i++, buff++)
	{
		if (*buff > 127)
		{
			if (dest)
				*buff = RecodeForwTable[*buff - 128];
			else
				*buff = RecodeBackTable[*buff - 128];
		}
	}
	return ch;
}

#define CHARSET_FILE "charset.conf"
#define MAX_CHARSETS   10
#define KEY_HOST	   1
#define KEY_BASE	   2
#define KEY_TABLE	   3

struct CharsetItem
{
	char		Orig[MAX_TOKEN];
	char		Dest[MAX_TOKEN];
	char		Table[MAX_TOKEN];
};


static bool
CharSetInRange(char *buf, int host)
{
	int			valid,
				i,
				FromAddr,
				ToAddr,
				tmp;
	struct in_addr file_ip_addr;
	char	   *p;
	unsigned int one = 0x80000000,
				NetMask = 0;
	unsigned char mask;

	p = strchr(buf, '/');
	if (p)
	{
		*p++ = '\0';
		valid = inet_aton(buf, &file_ip_addr);
		if (valid)
		{
			mask = strtoul(p, 0, 0);
			FromAddr = ntohl(file_ip_addr.s_addr);
			ToAddr = ntohl(file_ip_addr.s_addr);
			for (i = 0; i < mask; i++)
			{
				NetMask |= one;
				one >>= 1;
			}
			FromAddr &= NetMask;
			ToAddr = ToAddr | ~NetMask;
			tmp = ntohl(host);
			return ((unsigned) tmp >= (unsigned) FromAddr &&
					(unsigned) tmp <= (unsigned) ToAddr);
		}
	}
	else
	{
		p = strchr(buf, '-');
		if (p)
		{
			*p++ = '\0';
			valid = inet_aton(buf, &file_ip_addr);
			if (valid)
			{
				FromAddr = ntohl(file_ip_addr.s_addr);
				valid = inet_aton(p, &file_ip_addr);
				if (valid)
				{
					ToAddr = ntohl(file_ip_addr.s_addr);
					tmp = ntohl(host);
					return ((unsigned) tmp >= (unsigned) FromAddr &&
							(unsigned) tmp <= (unsigned) ToAddr);
				}
			}
		}
		else
		{
			valid = inet_aton(buf, &file_ip_addr);
			if (valid)
			{
				FromAddr = file_ip_addr.s_addr;
				return (unsigned) FromAddr == (unsigned) host;
			}
		}
	}
	return false;
}


static void
GetCharSetByHost(char *TableName, int host, const char *DataDir)
{
	FILE	   *file;
	char		buf[MAX_TOKEN],
				BaseCharset[MAX_TOKEN],
				OrigCharset[MAX_TOKEN],
				DestCharset[MAX_TOKEN],
				HostCharset[MAX_TOKEN],
			   *map_file;
	int			key,
				ChIndex = 0,
				i,
				bufsize;
	struct CharsetItem *ChArray[MAX_CHARSETS];

	*TableName = '\0';
	bufsize = (strlen(DataDir) + strlen(CHARSET_FILE) + 2) * sizeof(char);
	map_file = (char *) palloc(bufsize);
	snprintf(map_file, bufsize, "%s/%s", DataDir, CHARSET_FILE);
	file = AllocateFile(map_file, "r");
	pfree(map_file);
	if (file == NULL)
	{
		/* XXX should we log a complaint? */
		return;
	}

	while (!feof(file))
	{
		next_token(file, buf, sizeof(buf));
		if (buf[0] != '\0')
		{
			key = 0;
			if (strcasecmp(buf, "HostCharset") == 0)
				key = KEY_HOST;
			else if (strcasecmp(buf, "BaseCharset") == 0)
				key = KEY_BASE;
			else if (strcasecmp(buf, "RecodeTable") == 0)
				key = KEY_TABLE;
			else
				elog(LOG, "GetCharSetByHost: unknown tag %s in file %s",
					 buf, CHARSET_FILE);

			switch (key)
			{
				case KEY_HOST:
					/* Read the host */
					next_token(file, buf, sizeof(buf));
					if (buf[0] != '\0')
					{
						if (CharSetInRange(buf, host))
						{
							/* Read the charset */
							next_token(file, buf, sizeof(buf));
							if (buf[0] != '\0')
								strcpy(HostCharset, buf);
						}
					}
					break;
				case KEY_BASE:
					/* Read the base charset */
					next_token(file, buf, sizeof(buf));
					if (buf[0] != '\0')
						strcpy(BaseCharset, buf);
					break;
				case KEY_TABLE:
					/* Read the original charset */
					next_token(file, buf, sizeof(buf));
					if (buf[0] != '\0')
					{
						strcpy(OrigCharset, buf);
						/* Read the destination charset */
						next_token(file, buf, sizeof(buf));
						if (buf[0] != '\0')
						{
							strcpy(DestCharset, buf);
							/* Read the table filename */
							next_token(file, buf, sizeof(buf));
							if (buf[0] != '\0')
							{
								ChArray[ChIndex] =
									(struct CharsetItem *) palloc(sizeof(struct CharsetItem));
								strcpy(ChArray[ChIndex]->Orig, OrigCharset);
								strcpy(ChArray[ChIndex]->Dest, DestCharset);
								strcpy(ChArray[ChIndex]->Table, buf);
								ChIndex++;
							}
						}
					}
					break;
			}

			/* read to EOL */
			while (!feof(file) && buf[0])
			{
				next_token(file, buf, sizeof(buf));
				elog(LOG, "GetCharSetByHost: unknown tag %s in file %s",
					 buf, CHARSET_FILE);
			}
		}
	}
	FreeFile(file);

	for (i = 0; i < ChIndex; i++)
	{
		if (strcasecmp(BaseCharset, ChArray[i]->Orig) == 0 &&
			strcasecmp(HostCharset, ChArray[i]->Dest) == 0)
			strncpy(TableName, ChArray[i]->Table, 79);
		pfree(ChArray[i]);
	}
}
#endif   /* CYR_RECODE */



/* ----------------------------------------------------------------
 *	User ID things
 *
 * The authenticated user is determined at connection start and never
 * changes.  The session user can be changed only by SET SESSION
 * AUTHORIZATION.  The current user may change when "setuid" functions
 * are implemented.  Conceptually there is a stack, whose bottom
 * is the session user.  You are yourself responsible to save and
 * restore the current user id if you need to change it.
 * ----------------------------------------------------------------
 */
static AclId	AuthenticatedUserId = 0;
static AclId	SessionUserId = 0;
static AclId	CurrentUserId = 0;

static bool AuthenticatedUserIsSuperuser = false;

/*
 * This function is relevant for all privilege checks.
 */
AclId
GetUserId(void)
{
	AssertState(AclIdIsValid(CurrentUserId));
	return CurrentUserId;
}


void
SetUserId(AclId newid)
{
	AssertArg(AclIdIsValid(newid));
	CurrentUserId = newid;
}


/*
 * This value is only relevant for informational purposes.
 */
AclId
GetSessionUserId(void)
{
	AssertState(AclIdIsValid(SessionUserId));
	return SessionUserId;
}


void
SetSessionUserId(AclId newid)
{
	AssertArg(AclIdIsValid(newid));
	SessionUserId = newid;
	/* Current user defaults to session user. */
	if (!AclIdIsValid(CurrentUserId))
		CurrentUserId = newid;
}


void
InitializeSessionUserId(const char *username)
{
	HeapTuple	userTup;
	Datum		datum;
	bool		isnull;
	AclId		usesysid;

	/*
	 * Don't do scans if we're bootstrapping, none of the system catalogs
	 * exist yet, and they should be owned by postgres anyway.
	 */
	AssertState(!IsBootstrapProcessingMode());

	/* call only once */
	AssertState(!OidIsValid(AuthenticatedUserId));

	userTup = SearchSysCache(SHADOWNAME,
							 PointerGetDatum(username),
							 0, 0, 0);
	if (!HeapTupleIsValid(userTup))
		elog(FATAL, "user \"%s\" does not exist", username);

	usesysid = ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid;

	AuthenticatedUserId = usesysid;
	AuthenticatedUserIsSuperuser = ((Form_pg_shadow) GETSTRUCT(userTup))->usesuper;

	SetSessionUserId(usesysid); /* sets CurrentUserId too */

	/* Record username and superuser status as GUC settings too */
	SetConfigOption("session_authorization", username,
					PGC_BACKEND, PGC_S_OVERRIDE);
	SetConfigOption("is_superuser",
					AuthenticatedUserIsSuperuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);

	/*
	 * Set up user-specific configuration variables.  This is a good place
	 * to do it so we don't have to read pg_shadow twice during session
	 * startup.
	 */
	datum = SysCacheGetAttr(SHADOWNAME, userTup,
							Anum_pg_shadow_useconfig, &isnull);
	if (!isnull)
	{
		ArrayType  *a = DatumGetArrayTypeP(datum);

		ProcessGUCArray(a, PGC_S_USER);
	}

	ReleaseSysCache(userTup);
}


void
InitializeSessionUserIdStandalone(void)
{
	/* This function should only be called in a single-user backend. */
	AssertState(!IsUnderPostmaster);

	/* call only once */
	AssertState(!OidIsValid(AuthenticatedUserId));

	AuthenticatedUserId = BOOTSTRAP_USESYSID;
	AuthenticatedUserIsSuperuser = true;

	SetSessionUserId(BOOTSTRAP_USESYSID);
}


/*
 * Change session auth ID while running
 *
 * Only a superuser may set auth ID to something other than himself.  Note
 * that in case of multiple SETs in a single session, the original userid's
 * superuserness is what matters.  But we set the GUC variable is_superuser
 * to indicate whether the *current* session userid is a superuser.
 */
void
SetSessionAuthorization(AclId userid, bool is_superuser)
{
	/* Must have authenticated already, else can't make permission check */
	AssertState(AclIdIsValid(AuthenticatedUserId));

	if (userid != AuthenticatedUserId &&
		!AuthenticatedUserIsSuperuser)
		elog(ERROR, "SET SESSION AUTHORIZATION: permission denied");

	SetSessionUserId(userid);
	SetUserId(userid);

	SetConfigOption("is_superuser",
					is_superuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);
}


/*
 * Get user name from user id
 */
char *
GetUserNameFromId(AclId userid)
{
	HeapTuple	tuple;
	char	   *result;

	tuple = SearchSysCache(SHADOWSYSID,
						   ObjectIdGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "invalid user id %d", userid);

	result = pstrdup(NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename));

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
	unlink((char *) DatumGetPointer(filename));
	/* Should we complain if the unlink fails? */
}

/*
 * Create a lockfile, if possible
 *
 * Call CreateLockFile with the name of the lockfile to be created.
 * Returns true if successful, false if not (with a message on stderr).
 *
 * amPostmaster is used to determine how to encode the output PID.
 * isDDLock and refName are used to determine what error message to produce.
 */
static bool
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
	 * We need a loop here because of race conditions.	But don't loop
	 * forever (for example, a non-writable $PGDATA directory might cause
	 * a failure that won't go away).  100 tries seems like plenty.
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 */
		fd = open(filename, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0)
			break;				/* Success; exit the retry loop */

		/*
		 * Couldn't create the pid file. Probably it already exists.
		 */
		if ((errno != EEXIST && errno != EACCES) || ntries > 100)
			elog(FATAL, "Can't create lock file %s: %m", filename);

		/*
		 * Read the file to get the old owner's PID.  Note race condition
		 * here: file might have been deleted since we tried to create it.
		 */
		fd = open(filename, O_RDONLY, 0600);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* race condition; try again */
			elog(FATAL, "Can't read lock file %s: %m", filename);
		}
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) <= 0)
			elog(FATAL, "Can't read lock file %s: %m", filename);
		close(fd);

		buffer[len] = '\0';
		encoded_pid = atoi(buffer);

		/* if pid < 0, the pid is for postgres, not postmaster */
		other_pid = (pid_t) (encoded_pid < 0 ? -encoded_pid : encoded_pid);

		if (other_pid <= 0)
			elog(FATAL, "Bogus data in lock file %s", filename);

		/*
		 * Check to see if the other process still exists
		 *
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist.  BeOS returns EINVAL for some silly reason, however.
		 */
		if (other_pid != my_pid)
		{
			if (kill(other_pid, 0) == 0 ||
				(errno != ESRCH
#ifdef __BEOS__
				 && errno != EINVAL
#endif
				 ))
			{
				/* lockfile belongs to a live process */
				fprintf(stderr, "Lock file \"%s\" already exists.\n",
						filename);
				if (isDDLock)
					fprintf(stderr,
							"Is another %s (pid %d) running in \"%s\"?\n",
							(encoded_pid < 0 ? "postgres" : "postmaster"),
							(int) other_pid, refName);
				else
					fprintf(stderr,
							"Is another %s (pid %d) using \"%s\"?\n",
							(encoded_pid < 0 ? "postgres" : "postmaster"),
							(int) other_pid, refName);
				return false;
			}
		}

		/*
		 * No, the creating process did not exist.	However, it could be
		 * that the postmaster crashed (or more likely was kill -9'd by a
		 * clueless admin) but has left orphan backends behind.  Check for
		 * this by looking to see if there is an associated shmem segment
		 * that is still in use.
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
					{
						fprintf(stderr,
								"Found a pre-existing shared memory block (key %lu, id %lu) still in use.\n"
								"If you're sure there are no old backends still running,\n"
								"remove the shared memory block with ipcrm(1), or just\n"
								"delete \"%s\".\n",
								id1, id2, filename);
						return false;
					}
				}
			}
		}

		/*
		 * Looks like nobody's home.  Unlink the file and try again to
		 * create it.  Need a loop because of possible race condition
		 * against other would-be creators.
		 */
		if (unlink(filename) < 0)
			elog(FATAL, "Can't remove old lock file %s: %m"
				 "\n\tThe file seems accidentally left, but I couldn't remove it."
				 "\n\tPlease remove the file by hand and try again.",
				 filename);
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
		elog(FATAL, "Can't write lock file %s: %m", filename);
	}
	close(fd);

	/*
	 * Arrange for automatic removal of lockfile at proc_exit.
	 */
	on_proc_exit(UnlinkLockFile, PointerGetDatum(strdup(filename)));

	return true;				/* Success! */
}

bool
CreateDataDirLockFile(const char *datadir, bool amPostmaster)
{
	char		lockfile[MAXPGPATH];

	snprintf(lockfile, sizeof(lockfile), "%s/postmaster.pid", datadir);
	if (!CreateLockFile(lockfile, amPostmaster, true, datadir))
		return false;
	/* Save name of lockfile for RecordSharedMemoryInLockFile */
	strcpy(directoryLockFile, lockfile);
	return true;
}

bool
CreateSocketLockFile(const char *socketfile, bool amPostmaster)
{
	char		lockfile[MAXPGPATH];

	snprintf(lockfile, sizeof(lockfile), "%s.lock", socketfile);
	if (!CreateLockFile(lockfile, amPostmaster, false, socketfile))
		return false;
	/* Save name of lockfile for TouchSocketLockFile */
	strcpy(socketLockFile, lockfile);
	return true;
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
		 * utime() is POSIX standard, utimes() is a common alternative;
		 * if we have neither, fall back to actually reading the file
		 * (which only sets the access time not mod time, but that should
		 * be enough in most cases).  In all paths, we ignore errors.
		 */
#ifdef HAVE_UTIME
		utime(socketLockFile, NULL);
#else /* !HAVE_UTIME */
#ifdef HAVE_UTIMES
		utimes(socketLockFile, NULL);
#else /* !HAVE_UTIMES */
		int			fd;
		char		buffer[1];

		fd = open(socketLockFile, O_RDONLY | PG_BINARY, 0);
		if (fd >= 0)
		{
			read(fd, buffer, sizeof(buffer));
			close(fd);
		}
#endif /* HAVE_UTIMES */
#endif /* HAVE_UTIME */
	}
}

/*
 * Append information about a shared memory segment to the data directory
 * lock file (if we have created one).
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

	/*
	 * Do nothing if we did not create a lockfile (probably because we are
	 * running standalone).
	 */
	if (directoryLockFile[0] == '\0')
		return;

	fd = open(directoryLockFile, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
	{
		elog(LOG, "Failed to rewrite %s: %m", directoryLockFile);
		return;
	}
	len = read(fd, buffer, sizeof(buffer) - 100);
	if (len <= 0)
	{
		elog(LOG, "Failed to read %s: %m", directoryLockFile);
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
		elog(LOG, "Bogus data in %s", directoryLockFile);
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
		elog(LOG, "Failed to write %s: %m", directoryLockFile);
		close(fd);
		return;
	}
	close(fd);
}


/*-------------------------------------------------------------------------
 *				Version checking support
 *-------------------------------------------------------------------------
 */

/*
 * Determine whether the PG_VERSION file in directory `path' indicates
 * a data version compatible with the version of this program.
 *
 * If compatible, return. Otherwise, elog(FATAL).
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

	snprintf(full_path, MAXPGPATH, "%s/PG_VERSION", path);

	file = AllocateFile(full_path, "r");
	if (!file)
	{
		if (errno == ENOENT)
			elog(FATAL, "File %s is missing. This is not a valid data directory.", full_path);
		else
			elog(FATAL, "cannot open %s: %m", full_path);
	}

	ret = fscanf(file, "%ld.%ld", &file_major, &file_minor);
	if (ret != 2)
		elog(FATAL, "File %s does not contain valid data. You need to initdb.", full_path);

	FreeFile(file);

	if (my_major != file_major || my_minor != file_minor)
		elog(FATAL, "The data directory was initialized by PostgreSQL version %ld.%ld, "
			 "which is not compatible with this version %s.",
			 file_major, file_minor, version_string);
}

/*-------------------------------------------------------------------------
 *				Library preload support
 *-------------------------------------------------------------------------
 */

#if defined(__mc68000__) && defined(__ELF__)
typedef int32 ((*func_ptr) ());
#else
typedef char *((*func_ptr) ());
#endif

/*
 * process any libraries that should be preloaded and
 * optionally pre-initialized
 */
void
process_preload_libraries(char *preload_libraries_string)
{
	char	   *rawstring;
	List	   *elemlist;
	List	   *l;

	if (preload_libraries_string == NULL)
		return;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(preload_libraries_string);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		freeList(elemlist);
		elog(LOG, "invalid list syntax for preload_libraries configuration option");
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);
		char	   *sep = strstr(tok, ":");
		char	   *filename = NULL;
		char	   *funcname = NULL;
		func_ptr	initfunc;

		if (sep)
		{
			/*
			 * a colon separator implies there is an initialization function
			 * that we need to run in addition to loading the library
			 */
			size_t		filename_len = sep - tok;
			size_t		funcname_len = strlen(tok) - filename_len - 1;

			filename = (char *) palloc(filename_len + 1);
			memset(filename, '\0', filename_len + 1);
			snprintf(filename, filename_len + 1, "%s", tok);

			funcname = (char *) palloc(funcname_len + 1);
			memset(funcname, '\0', funcname_len + 1);
			snprintf(funcname, funcname_len + 1, "%s", sep + 1);
		}
		else
		{
			/*
			 * no separator -- just load the library
			 */
			filename = pstrdup(tok);
			funcname = NULL;
		}

		initfunc = (func_ptr) load_external_function(filename, funcname, false, NULL);
		if (initfunc)
			(*initfunc)();

		elog(LOG, "preloaded library %s with initialization function %s", filename, funcname);

		if (filename != NULL)
			pfree(filename);

		if (funcname != NULL)
			pfree(funcname);
	}

	pfree(rawstring);
	freeList(elemlist);
}

