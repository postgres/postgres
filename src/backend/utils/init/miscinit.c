/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/miscinit.c,v 1.65 2001/04/16 02:42:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/param.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <errno.h>

#include "catalog/catname.h"
#include "catalog/pg_shadow.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


#ifdef CYR_RECODE
unsigned char RecodeForwTable[128];
unsigned char RecodeBackTable[128];

#endif

ProcessingMode Mode = InitProcessing;

/* Note: we rely on these to initialize as zeroes */
static char directoryLockFile[MAXPGPATH];
static char socketLockFile[MAXPGPATH];


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
	free(DatabasePath);
	/* use strdup since this is done before memory contexts are set up */
	if (path)
	{
		DatabasePath = strdup(path);
		AssertState(DatabasePath);
	}
}

void
SetDatabaseName(const char *name)
{
	free(DatabaseName);
	if (name)
	{
		DatabaseName = strdup(name);
		AssertState(DatabaseName);
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
	if (DataDir)
		free(DataDir);

	if (dir[0] != '/')
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
		sprintf(new, "%s/%s", buf, dir);
		free(buf);
	}
	else
		new = strdup(dir);

	if (!new)
		elog(FATAL, "out of memory");
	DataDir = new;
}


/* ----------------------------------------------------------------
 *				MULTIBYTE stub code
 *
 * Even if MULTIBYTE is not enabled, these functions are necessary
 * since pg_proc.h has references to them.
 * ----------------------------------------------------------------
 */

#ifndef MULTIBYTE

Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(namein, CStringGetDatum("SQL_ASCII"));
}

Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(namein, CStringGetDatum("SQL_ASCII"));
}

Datum
PG_char_to_encoding(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

#endif

/* ----------------------------------------------------------------
 *				CYR_RECODE support
 * ----------------------------------------------------------------
 */

#ifdef CYR_RECODE

#define MAX_TOKEN	80

/* Some standard C libraries, including GNU, have an isblank() function.
   Others, including Solaris, do not.  So we have our own.
*/
static bool
isblank(const char c)
{
	return c == ' ' || c == 9 /* tab */ ;
}

static void
next_token(FILE *fp, char *buf, const int bufsz)
{
/*--------------------------------------------------------------------------
  Grab one token out of fp.  Tokens are strings of non-blank
  characters bounded by blank characters, beginning of line, and end
  of line.	Blank means space or tab.  Return the token as *buf.
  Leave file positioned to character immediately after the token or
  EOF, whichever comes first.  If no more tokens on line, return null
  string as *buf and position file to beginning of next line or EOF,
  whichever comes first.
--------------------------------------------------------------------------*/
	int			c;
	char	   *eb = buf + (bufsz - 1);

	/* Move over inital token-delimiting blanks */
	while (isblank(c = getc(fp)));

	if (c != '\n')
	{

		/*
		 * build a token in buf of next characters up to EOF, eol, or
		 * blank.
		 */
		while (c != EOF && c != '\n' && !isblank(c))
		{
			if (buf < eb)
				*buf++ = c;
			c = getc(fp);

			/*
			 * Put back the char right after the token (putting back EOF
			 * is ok)
			 */
		}
		ungetc(c, fp);
	}
	*buf = '\0';
}

static void
read_through_eol(FILE *file)
{
	int			c;

	do
		c = getc(file);
	while (c != '\n' && c != EOF);
}

void
SetCharSet()
{
	FILE	   *file;
	char	   *p,
				c,
				eof = false;
	char	   *map_file;
	char		buf[MAX_TOKEN];
	int			i;
	unsigned char FromChar,
				ToChar;

	for (i = 0; i < 128; i++)
	{
		RecodeForwTable[i] = i + 128;
		RecodeBackTable[i] = i + 128;
	}

	p = getenv("PG_RECODETABLE");
	if (p && *p != '\0')
	{
		map_file = (char *) malloc((strlen(DataDir) +
									strlen(p) + 2) * sizeof(char));
		sprintf(map_file, "%s/%s", DataDir, p);
		file = AllocateFile(map_file, PG_BINARY_R);
		if (file == NULL)
			return;
		eof = false;
		while (!eof)
		{
			c = getc(file);
			ungetc(c, file);
			if (c == EOF)
				eof = true;
			else
			{
				if (c == '#')
					read_through_eol(file);
				else
				{
					/* Read the FromChar */
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
						}
						read_through_eol(file);
					}
				}
			}
		}
		FreeFile(file);
		free(map_file);
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

#endif



/* ----------------------------------------------------------------
 *	User ID things
 *
 * The session user is determined at connection start and never
 * changes.  The current user may change when "setuid" functions
 * are implemented.  Conceptually there is a stack, whose bottom
 * is the session user.  You are yourself responsible to save and
 * restore the current user id if you need to change it.
 * ----------------------------------------------------------------
 */
static Oid	CurrentUserId = InvalidOid;
static Oid	SessionUserId = InvalidOid;


/*
 * This function is relevant for all privilege checks.
 */
Oid
GetUserId(void)
{
	AssertState(OidIsValid(CurrentUserId));
	return CurrentUserId;
}


void
SetUserId(Oid newid)
{
	AssertArg(OidIsValid(newid));
	CurrentUserId = newid;
}


/*
 * This value is only relevant for informational purposes.
 */
Oid
GetSessionUserId(void)
{
	AssertState(OidIsValid(SessionUserId));
	return SessionUserId;
}


void
SetSessionUserId(Oid newid)
{
	AssertArg(OidIsValid(newid));
	SessionUserId = newid;
	/* Current user defaults to session user. */
	if (!OidIsValid(CurrentUserId))
		CurrentUserId = newid;
}


void
SetSessionUserIdFromUserName(const char *username)
{
	HeapTuple	userTup;

	/*
	 * Don't do scans if we're bootstrapping, none of the system catalogs
	 * exist yet, and they should be owned by postgres anyway.
	 */
	AssertState(!IsBootstrapProcessingMode());

	userTup = SearchSysCache(SHADOWNAME,
							 PointerGetDatum(username),
							 0, 0, 0);
	if (!HeapTupleIsValid(userTup))
		elog(FATAL, "user \"%s\" does not exist", username);

	SetSessionUserId(((Form_pg_shadow) GETSTRUCT(userTup))->usesysid);

	ReleaseSysCache(userTup);
}


/*
 * Get user name from user id
 */
char *
GetUserName(Oid userid)
{
	HeapTuple	tuple;
	char	   *result;

	tuple = SearchSysCache(SHADOWSYSID,
						   ObjectIdGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "invalid user id %u", (unsigned) userid);

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
	int			len;
	int			encoded_pid;
	pid_t		other_pid;
	pid_t		my_pid = getpid();

	/*
	 * We need a loop here because of race conditions.
	 */
	for (;;)
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
		if (errno != EEXIST && errno != EACCES)
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
							other_pid, refName);
				else
					fprintf(stderr,
							"Is another %s (pid %d) using \"%s\"?\n",
							(encoded_pid < 0 ? "postgres" : "postmaster"),
							other_pid, refName);
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
			unsigned long shmKey,
						shmId;

			ptr = strchr(buffer, '\n');
			if (ptr != NULL &&
				(ptr = strchr(ptr + 1, '\n')) != NULL)
			{
				ptr++;
				if (sscanf(ptr, "%lu %lu", &shmKey, &shmId) == 2)
				{
					if (SharedMemoryIsInUse((IpcMemoryKey) shmKey,
											(IpcMemoryId) shmId))
					{
						fprintf(stderr,
								"Found a pre-existing shared memory block (ID %d) still in use.\n"
								"If you're sure there are no old backends still running,\n"
								"remove the shared memory block with ipcrm(1), or just\n"
								"delete \"%s\".\n",
								(int) shmId, filename);
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
	if (write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int			save_errno = errno;

		close(fd);
		unlink(filename);
		errno = save_errno;
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
 * Re-read the socket lock file.  This should be called every so often
 * to ensure that the lock file has a recent access date.  That saves it
 * from being removed by overenthusiastic /tmp-directory-cleaner daemons.
 * (Another reason we should never have put the socket file in /tmp...)
 */
void
TouchSocketLockFile(void)
{
	int			fd;
	char		buffer[1];

	/* Do nothing if we did not create a socket... */
	if (socketLockFile[0] != '\0')
	{
		/* XXX any need to complain about errors here? */
		fd = open(socketLockFile, O_RDONLY | PG_BINARY, 0);
		if (fd >= 0)
		{
			read(fd, buffer, sizeof(buffer));
			close(fd);
		}
	}
}

/*
 * Append information about a shared memory segment to the data directory
 * lock file (if we have created one).
 *
 * This may be called multiple times in the life of a postmaster, if we
 * delete and recreate shmem due to backend crash.	Therefore, be prepared
 * to overwrite existing information.  (As of 7.1, a postmaster only creates
 * one shm seg anyway; but for the purposes here, if we did have more than
 * one then any one of them would do anyway.)
 */
void
RecordSharedMemoryInLockFile(IpcMemoryKey shmKey, IpcMemoryId shmId)
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
		elog(DEBUG, "Failed to rewrite %s: %m", directoryLockFile);
		return;
	}
	len = read(fd, buffer, sizeof(buffer) - 100);
	if (len <= 0)
	{
		elog(DEBUG, "Failed to read %s: %m", directoryLockFile);
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
		elog(DEBUG, "Bogus data in %s", directoryLockFile);
		close(fd);
		return;
	}
	ptr++;

	/*
	 * Append shm key and ID.  Format to try to keep it the same length
	 * always (trailing junk won't hurt, but might confuse humans).
	 */
	sprintf(ptr, "%9lu %9lu\n",
			(unsigned long) shmKey, (unsigned long) shmId);

	/*
	 * And rewrite the data.  Since we write in a single kernel call, this
	 * update should appear atomic to onlookers.
	 */
	len = strlen(buffer);
	if (lseek(fd, (off_t) 0, SEEK_SET) != 0 ||
		(int) write(fd, buffer, len) != len)
	{
		elog(DEBUG, "Failed to write %s: %m", directoryLockFile);
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
	if (ret == EOF)
		elog(FATAL, "cannot read %s: %m", full_path);
	else if (ret != 2)
		elog(FATAL, "`%s' does not have a valid format. You need to initdb.", full_path);

	FreeFile(file);

	if (my_major != file_major || my_minor != file_minor)
		elog(FATAL, "The data directory was initialized by PostgreSQL version %ld.%ld, "
			 "which is not compatible with this version %s.",
			 file_major, file_minor, version_string);
}
