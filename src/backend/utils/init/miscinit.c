/*-------------------------------------------------------------------------
 *
 * miscinit.c--
 *    miscellanious initialization support stuff
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/init/miscinit.c,v 1.5 1997/04/27 19:20:37 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <sys/param.h>		/* for MAXPATHLEN */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <stdio.h>
#include <unistd.h>
#include <grp.h>		/* for getgrgid */
#include <pwd.h>		/* for getpwuid */

#include "postgres.h"

#include "utils/portal.h"	/* for EnablePortalManager, etc. */
#include "utils/exc.h"		/* for EnableExceptionHandling, etc. */
#include "utils/mcxt.h"		/* for EnableMemoryContext, etc. */
#include "utils/elog.h"
#include "utils/builtins.h"

#include "miscadmin.h"		/* where the declarations go */

#include "catalog/catname.h"
#include "catalog/pg_user.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"

#include "storage/fd.h"		/* for O_ */

/*
 * EnableAbortEnvVarName --
 *	Enables system abort iff set to a non-empty string in environment.
 */
#define EnableAbortEnvVarName	"POSTGRESABORT"

extern	char *getenv(const char *name);	/* XXX STDLIB */

/*  from globals.c */
extern char *DatabaseName;
extern char *UserName;
extern char *DatabasePath;


/*
 * Define USE_ENVIRONMENT to get PGDATA, etc. from environment variables.
 * This is the default on UNIX platforms.
 */
#define USE_ENVIRONMENT

/* ----------------------------------------------------------------
 *		some of the 19 ways to leave postgres
 * ----------------------------------------------------------------
 */

/*
 * ExitPostgres --
 *	Exit POSTGRES with a status code.
 *
 * Note:
 *	This function never returns.
 *	...
 *
 * Side effects:
 *	...
 *
 * Exceptions:
 *	none
 */
void
ExitPostgres(ExitStatus status)
{
#ifdef	__SABER__
    saber_stop();
#endif
    exitpg(status);
}

/*
 * AbortPostgres --
 *	Abort POSTGRES dumping core.
 *
 * Note:
 *	This function never returns.
 *	...
 *
 * Side effects:
 *	Core is dumped iff EnableAbortEnvVarName is set to a non-empty string.
 *	...
 *
 * Exceptions:
 *	none
 */
void
AbortPostgres()
{
    char *abortValue = getenv(EnableAbortEnvVarName);
    
#ifdef	__SABER__
    saber_stop();
#endif
    
    if (PointerIsValid(abortValue) && abortValue[0] != '\0')
	abort();
    else
	exitpg(FatalExitStatus);
}

/* ----------------
 *	StatusBackendExit
 * ----------------
 */
void
StatusBackendExit(int status)
{
    /* someday, do some real cleanup and then call the LISP exit */
    /* someday, call StatusPostmasterExit if running without postmaster */
    exitpg(status);
}

/* ----------------
 *	StatusPostmasterExit
 * ----------------
 */
void
StatusPostmasterExit(int status)
{
    /* someday, do some real cleanup and then call the LISP exit */
    exitpg(status);
}

/* ----------------------------------------------------------------
 *	processing mode support stuff (used to be in pmod.c)
 * ----------------------------------------------------------------
 */
static ProcessingMode	Mode = NoProcessing;

/*
 * IsNoProcessingMode --
 *	True iff processing mode is NoProcessing.
 */
bool
IsNoProcessingMode()
{
    return ((bool)(Mode == NoProcessing));
}

/*
 * IsBootstrapProcessingMode --
 *	True iff processing mode is BootstrapProcessing.
 */
bool
IsBootstrapProcessingMode()
{
    return ((bool)(Mode == BootstrapProcessing));
}

/*
 * IsInitProcessingMode --
 *	True iff processing mode is InitProcessing.
 */
bool
IsInitProcessingMode()
{
    return ((bool)(Mode == InitProcessing));
}

/*
 * IsNormalProcessingMode --
 *	True iff processing mode is NormalProcessing.
 */
bool
IsNormalProcessingMode()
{
    return ((bool)(Mode == NormalProcessing));
}

/*
 * SetProcessingMode --
 *	Sets mode of processing as specified.
 *
 * Exceptions:
 *	BadArg if called with invalid mode.
 *
 * Note:
 *	Mode is NoProcessing before the first time this is called.
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
 *		database path / name support stuff
 * ----------------------------------------------------------------
 */

/*
 * GetDatabasePath --
 *	Returns path to database.
 *
 */
char*
GetDatabasePath()
{
    return DatabasePath;
}

/*
 * GetDatabaseName --
 *	Returns name of database.
 */
char*
GetDatabaseName()
{
    return DatabaseName;
}

void
SetDatabasePath(char *path)
{
    /* use malloc since this is done before memory contexts are set up */
    if (DatabasePath)
	free(DatabasePath);
    DatabasePath = malloc(strlen(path)+1);
    strcpy(DatabasePath, path);
}

void
SetDatabaseName(char *name)
{
    if (DatabaseName)
	free (DatabaseName);
    DatabaseName = malloc(strlen(name)+1);
    strcpy(DatabaseName, name);
}

/* ----------------
 *	GetPgUserName and SetPgUserName
 *
 *	SetPgUserName must be called before InitPostgres, since the setuid()
 *	is done there.
 *
 *	Replace GetPgUserName() with a lower-case version
 *	to allow use in new case-insensitive SQL (referenced
 *	in pg_proc.h). Define GetPgUserName() as a macro - tgl 97/04/26
 * ----------------
 */
char*
getpgusername()
{
    return UserName;
}

void
SetPgUserName()
{
#ifndef NO_SECURITY
    char *p;
    struct passwd *pw;
    
    if (IsUnderPostmaster) {
	/* use the (possibly) authenticated name that's provided */
	if (!(p = getenv("PG_USER")))
	    elog(FATAL, "SetPgUserName: PG_USER environment variable unset");
    } else {
	/* setuid() has not yet been done, see above comment */
	if (!(pw = getpwuid(geteuid())))
	    elog(FATAL, "SetPgUserName: no entry in passwd file");
	p = pw->pw_name;
    }
    if (UserName)
	free(UserName);
    UserName = malloc(strlen(p)+1);
    strcpy(UserName, p);
#endif /* NO_SECURITY */
}

/* ----------------------------------------------------------------
 *	GetUserId and SetUserId
 * ----------------------------------------------------------------
 */
static Oid	UserId = InvalidOid;

Oid
GetUserId()
{
    Assert(OidIsValid(UserId));
    return(UserId);
}

void
SetUserId()
{
    HeapTuple	userTup;
    char *userName;
    
    Assert(!OidIsValid(UserId));	/* only once */
    
    /*
     * Don't do scans if we're bootstrapping, none of the system
     * catalogs exist yet, and they should be owned by postgres
     * anyway.
     */
    if (IsBootstrapProcessingMode()) {
	UserId = geteuid();
	return;
    }
    
    userName = GetPgUserName();
    userTup = SearchSysCacheTuple(USENAME, PointerGetDatum(userName),
				  0,0,0);
    if (!HeapTupleIsValid(userTup))
	elog(FATAL, "SetUserId: user \"%s\" is not in \"%s\"",
	     userName, 
	     UserRelationName);
    UserId = (Oid) ((Form_pg_user) GETSTRUCT(userTup))->usesysid;
}
