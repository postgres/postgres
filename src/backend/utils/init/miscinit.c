/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  miscellanious initialization support stuff
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/miscinit.c,v 1.57 2000/11/16 22:30:39 tgl Exp $
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

static char *GetPidFname(void);


#ifdef CYR_RECODE
unsigned char RecodeForwTable[128];
unsigned char RecodeBackTable[128];

#endif

ProcessingMode Mode = InitProcessing;

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

#ifndef MULTIBYTE
/* even if MULTIBYTE is not enabled, these functions are necessary
 * since pg_proc.h has references to them.
 */

Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	PG_RETURN_NAME("SQL_ASCII");
}

Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	PG_RETURN_NAME("SQL_ASCII");
}

Datum
PG_char_to_encoding(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

#endif

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
 * 	User ID things
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

	SetSessionUserId( ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid );

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

	result = pstrdup( NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename) );

	ReleaseSysCache(tuple);
	return result;
}



/*-------------------------------------------------------------------------
 * Set data directory, but make sure it's an absolute path.  Use this,
 * never set DataDir directly.
 *-------------------------------------------------------------------------
 */
void
SetDataDir(const char *dir)
{
	char *new;

	AssertArg(dir);
	if (DataDir)
		free(DataDir);

	if (dir[0] != '/')
	{
		char *buf;
		size_t buflen;

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
	}
	else
	{
		new = strdup(dir);
	}

	if (!new)
		elog(FATAL, "out of memory");
	DataDir = new;		
}



/*-------------------------------------------------------------------------
 *
 * postmaster pid file stuffs. $DATADIR/postmaster.pid is created when:
 *
 *	(1) postmaster starts. In this case pid > 0.
 *	(2) postgres starts in standalone mode. In this case
 *		pid < 0
 *
 * to gain an interlock.
 *
 *	SetPidFname(datadir)
 *		Remember the the pid file name. This is neccesary
 *		UnlinkPidFile() is called from proc_exit().
 *
 *	GetPidFname(datadir)
 *		Get the pid file name. SetPidFname() should be called
 *		before GetPidFname() gets called.
 *
 *	UnlinkPidFile()
 *		This is called from proc_exit() and unlink the pid file.
 *
 *	SetPidFile(pid_t pid)
 *		Create the pid file. On failure, it checks if the process
 *		actually exists or not. SetPidFname() should be called
 *		in prior to calling SetPidFile().
 *
 *-------------------------------------------------------------------------
 */

/*
 * Path to pid file. proc_exit() remember it to unlink the file.
 */
static char PidFile[MAXPGPATH];

/*
 * Remove the pid file. This function is called from proc_exit.
 */
void
UnlinkPidFile(void)
{
	unlink(PidFile);
}

/*
 * Set path to the pid file
 */
void
SetPidFname(char *datadir)
{
	snprintf(PidFile, sizeof(PidFile), "%s/%s", datadir, PIDFNAME);
}

/*
 * Get path to the pid file
 */
static char *
GetPidFname(void)
{
	return (PidFile);
}

/*
 * Create the pid file
 */
int
SetPidFile(pid_t pid)
{
	int			fd;
	char	   *pidfile;
	char		pidstr[32];
	int			len;
	pid_t		post_pid;
	int			is_postgres = 0;

	/*
	 * Creating pid file
	 */
	pidfile = GetPidFname();
	fd = open(pidfile, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
	{

		/*
		 * Couldn't create the pid file. Probably it already exists. Read
		 * the file to see if the process actually exists
		 */
		fd = open(pidfile, O_RDONLY, 0600);
		if (fd < 0)
		{
			fprintf(stderr, "Can't open pid file: %s\n", pidfile);
			fprintf(stderr, "Please check the permission and try again.\n");
			return (-1);
		}
		if ((len = read(fd, pidstr, sizeof(pidstr) - 1)) < 0)
		{
			fprintf(stderr, "Can't read pid file: %s\n", pidfile);
			fprintf(stderr, "Please check the permission and try again.\n");
			close(fd);
			return (-1);
		}
		close(fd);

		/*
		 * Check to see if the process actually exists
		 */
		pidstr[len] = '\0';
		post_pid = (pid_t) atoi(pidstr);

		/* if pid < 0, the pid is for postgres, not postmatser */
		if (post_pid < 0)
		{
			is_postgres++;
			post_pid = -post_pid;
		}

		if (post_pid == 0 || (post_pid > 0 && kill(post_pid, 0) < 0))
		{

			/*
			 * No, the process did not exist. Unlink the file and try to
			 * create it
			 */
			if (unlink(pidfile) < 0)
			{
				fprintf(stderr, "Can't remove pid file: %s\n", pidfile);
				fprintf(stderr, "The file seems accidently left, but I couldn't remove it.\n");
				fprintf(stderr, "Please remove the file by hand and try again.\n");
				return (-1);
			}
			fd = open(pidfile, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (fd < 0)
			{
				fprintf(stderr, "Can't create pid file: %s\n", pidfile);
				fprintf(stderr, "Please check the permission and try again.\n");
				return (-1);
			}
		}
		else
		{

			/*
			 * Another postmaster is running
			 */
			fprintf(stderr, "Can't create pid file: %s\n", pidfile);
			if (is_postgres)
				fprintf(stderr, "Is another postgres (pid: %d) running?\n", (int) post_pid);
			else
				fprintf(stderr, "Is another postmaster (pid: %s) running?\n", pidstr);
			return (-1);
		}
	}

	sprintf(pidstr, "%d", (int) pid);
	if (write(fd, pidstr, strlen(pidstr)) != strlen(pidstr))
	{
		fprintf(stderr, "Write to pid file failed\n");
		fprintf(stderr, "Please check the permission and try again.\n");
		close(fd);
		unlink(pidfile);
		return (-1);
	}
	close(fd);

	return (0);
}



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
	FILE       *file;
	int			ret;
	long        file_major, file_minor;
	long        my_major = 0, my_minor = 0;
	char       *endptr;
	const char *version_string = PG_VERSION;

	my_major = strtol(version_string, &endptr, 10);
	if (*endptr == '.')
		my_minor = strtol(endptr+1, NULL, 10);

	snprintf(full_path, MAXPGPATH, "%s/PG_VERSION", path);

	file = AllocateFile(full_path, "r");
	if (!file)
	{
		if (errno == ENOENT)
			elog(FATAL, "File %s is missing. This is not a valid data directory.", full_path);
		else
			elog(FATAL, "cannot open %s: %s", full_path, strerror(errno));
	}

	ret = fscanf(file, "%ld.%ld", &file_major, &file_minor);
	if (ret == EOF)
		elog(FATAL, "cannot read %s: %s", full_path, strerror(errno));
	else if (ret != 2)
		elog(FATAL, "`%s' does not have a valid format. You need to initdb.", full_path);

	FreeFile(file);

	if (my_major != file_major || my_minor != file_minor)
		elog(FATAL, "The data directory was initalized by PostgreSQL version %ld.%ld, "
			 "which is not compatible with this verion %s.",
			 file_major, file_minor, version_string);
}
