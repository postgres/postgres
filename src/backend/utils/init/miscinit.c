/*-------------------------------------------------------------------------
 *
 * miscinit.c--
 *	  miscellanious initialization support stuff
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/miscinit.c,v 1.19 1998/08/11 18:28:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <sys/param.h>			/* for MAXPATHLEN */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <stdio.h>
#include <unistd.h>
#include <grp.h>				/* for getgrgid */
#include <pwd.h>				/* for getpwuid */

#include "postgres.h"

#include "utils/portal.h"		/* for EnablePortalManager, etc. */
#include "utils/exc.h"			/* for EnableExceptionHandling, etc. */
#include "utils/mcxt.h"			/* for EnableMemoryContext, etc. */
#include "utils/elog.h"
#include "utils/builtins.h"

#include "miscadmin.h"			/* where the declarations go */

#include "catalog/catname.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"

#include "storage/fd.h"			/* for O_ */

/*
 * EnableAbortEnvVarName --
 *		Enables system abort iff set to a non-empty string in environment.
 */
#define EnableAbortEnvVarName	"POSTGRESABORT"

extern char *getenv(const char *name);	/* XXX STDLIB */

/*	from globals.c */
extern char *UserName;

#ifdef CYR_RECODE
unsigned char RecodeForwTable[128];
unsigned char RecodeBackTable[128];

#endif


/*
 * Define USE_ENVIRONMENT to get PGDATA, etc. from environment variables.
 * This is the default on UNIX platforms.
 */
#define USE_ENVIRONMENT

/* ----------------------------------------------------------------
 *				some of the 19 ways to leave postgres
 * ----------------------------------------------------------------
 */

/*
 * ExitPostgres --
 *		Exit POSTGRES with a status code.
 *
 * Note:
 *		This function never returns.
 *		...
 *
 * Side effects:
 *		...
 *
 * Exceptions:
 *		none
 */
void
ExitPostgres(ExitStatus status)
{
	proc_exit(status);
}

/*
 * AbortPostgres --
 *		Abort POSTGRES dumping core.
 *
 * Note:
 *		This function never returns.
 *		...
 *
 * Side effects:
 *		Core is dumped iff EnableAbortEnvVarName is set to a non-empty string.
 *		...
 *
 * Exceptions:
 *		none
 */
#ifdef NOT_USED
void
AbortPostgres()
{
	char	   *abortValue = getenv(EnableAbortEnvVarName);

	if (PointerIsValid(abortValue) && abortValue[0] != '\0')
		abort();
	else
		proc_exit(FatalExitStatus);
}

#endif

/* ----------------
 *		StatusBackendExit
 * ----------------
 */
void
StatusBackendExit(int status)
{
	/* someday, do some real cleanup and then call the LISP exit */
	/* someday, call StatusPostmasterExit if running without postmaster */
	proc_exit(status);
}

/* ----------------
 *		StatusPostmasterExit
 * ----------------
 */
void
StatusPostmasterExit(int status)
{
	/* someday, do some real cleanup and then call the LISP exit */
	proc_exit(status);
}

/* ----------------------------------------------------------------
 *		processing mode support stuff (used to be in pmod.c)
 * ----------------------------------------------------------------
 */
static ProcessingMode Mode = NoProcessing;

/*
 * IsNoProcessingMode --
 *		True iff processing mode is NoProcessing.
 */
bool
IsNoProcessingMode()
{
	return ((bool) (Mode == NoProcessing));
}

/*
 * IsBootstrapProcessingMode --
 *		True iff processing mode is BootstrapProcessing.
 */
bool
IsBootstrapProcessingMode()
{
	return ((bool) (Mode == BootstrapProcessing));
}

/*
 * IsInitProcessingMode --
 *		True iff processing mode is InitProcessing.
 */
bool
IsInitProcessingMode()
{
	return ((bool) (Mode == InitProcessing));
}

/*
 * IsNormalProcessingMode --
 *		True iff processing mode is NormalProcessing.
 */
bool
IsNormalProcessingMode()
{
	return ((bool) (Mode == NormalProcessing));
}

/*
 * SetProcessingMode --
 *		Sets mode of processing as specified.
 *
 * Exceptions:
 *		BadArg if called with invalid mode.
 *
 * Note:
 *		Mode is NoProcessing before the first time this is called.
 */
void
SetProcessingMode(ProcessingMode mode)
{
	AssertArg(mode == NoProcessing || mode == BootstrapProcessing ||
			  mode == InitProcessing || mode == NormalProcessing);

	Mode = mode;
}

ProcessingMode
GetProcessingMode()
{
	return (Mode);
}

/* ----------------------------------------------------------------
 *				database path / name support stuff
 * ----------------------------------------------------------------
 */

void
SetDatabasePath(char *path)
{
	/* use malloc since this is done before memory contexts are set up */
	if (DatabasePath)
		free(DatabasePath);
	DatabasePath = malloc(strlen(path) + 1);
	strcpy(DatabasePath, path);
}

void
SetDatabaseName(char *name)
{
	if (DatabaseName)
		free(DatabaseName);
	DatabaseName = malloc(strlen(name) + 1);
	strcpy(DatabaseName, name);
}

#ifndef MULTIBYTE
/* even if MULTIBYTE is not enabled, this function is neccesary
 * since pg_proc.h does have.
 */
const char *
getdatabaseencoding()
{
  elog(ERROR, "you need to enable MB to use this function");
  return("");
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
	return (c == ' ' || c == 9 /* tab */ );
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
		file = fopen(map_file, "r");
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
		fclose(file);
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
			elog(FATAL, "SetPgUserName: PG_USER environment variable unset");
	}
	else
	{
		/* setuid() has not yet been done, see above comment */
		if (!(pw = getpwuid(geteuid())))
			elog(FATAL, "SetPgUserName: no entry in passwd file");
		p = pw->pw_name;
	}
	if (UserName)
		free(UserName);
	UserName = malloc(strlen(p) + 1);
	strcpy(UserName, p);
#endif							/* NO_SECURITY */
}

/* ----------------------------------------------------------------
 *		GetUserId and SetUserId
 * ----------------------------------------------------------------
 */
static Oid	UserId = InvalidOid;

int
GetUserId()
{
	Assert(OidIsValid(UserId));
	return (UserId);
}

void
SetUserId()
{
	HeapTuple	userTup;
	char	   *userName;

	Assert(!OidIsValid(UserId));/* only once */

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
	userTup = SearchSysCacheTuple(USENAME, PointerGetDatum(userName),
								  0, 0, 0);
	if (!HeapTupleIsValid(userTup))
		elog(FATAL, "SetUserId: user \"%s\" is not in \"%s\"",
			 userName,
			 ShadowRelationName);
	UserId = (Oid) ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid;
}
