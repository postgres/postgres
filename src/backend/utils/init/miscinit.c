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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/miscinit.c,v 1.50 2000/06/14 18:17:46 petere Exp $
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
			if (dest)
				*buff = RecodeForwTable[*buff - 128];
			else
				*buff = RecodeBackTable[*buff - 128];
	}
	return ch;
}

#endif

/* ----------------
 *		GetPgUserName and SetPgUserName
 *
 *		SetPgUserName must be called before InitPostgres, since the setuid()
 *		is done there.
 *
 *		Replace GetPgUserName() with a lower-case version
 *		to allow use in new case-insensitive SQL (referenced
 *		in pg_proc.h). Define GetPgUserName() as a macro - tgl 97/04/26
 * ----------------
 */
char *
getpgusername()
{
	return UserName;
}

void
SetPgUserName()
{
#ifndef NO_SECURITY
	char	   *p;
	struct passwd *pw;

	if (IsUnderPostmaster)
	{
		/* use the (possibly) authenticated name that's provided */
		if (!(p = getenv("PG_USER")))
			elog(FATAL, "SetPgUserName: PG_USER environment variable is unset");
	}
	else
	{
		/* setuid() has not yet been done, see above comment */
		if (!(pw = getpwuid(geteuid())))
			elog(FATAL, "SetPgUserName: no entry in host passwd file");
		p = pw->pw_name;
	}
	if (UserName)
		free(UserName);
	UserName = malloc(strlen(p) + 1);
	strcpy(UserName, p);
#endif	 /* NO_SECURITY */
}

/* ----------------------------------------------------------------
 *		GetUserId and SetUserId
 * ----------------------------------------------------------------
 */
static Oid	UserId = InvalidOid;

int
GetUserId()
{
	AssertState(OidIsValid(UserId));
	return UserId;
}

void
SetUserId()
{
	HeapTuple	userTup;
	char	   *userName;

	AssertState(!OidIsValid(UserId));	/* only once */

	/*
	 * Don't do scans if we're bootstrapping, none of the system catalogs
	 * exist yet, and they should be owned by postgres anyway.
	 */
	if (IsBootstrapProcessingMode())
	{
		UserId = geteuid();
		return;
	}

	userName = GetPgUserName();
	userTup = SearchSysCacheTuple(SHADOWNAME,
								  PointerGetDatum(userName),
								  0, 0, 0);
	if (!HeapTupleIsValid(userTup))
		elog(FATAL, "SetUserId: user '%s' is not in '%s'",
			 userName,
			 ShadowRelationName);
	UserId = (Oid) ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid;
}

/*-------------------------------------------------------------------------
 *
 * posmaster pid file stuffs. $DATADIR/postmaster.pid is created when:
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
