/*-------------------------------------------------------------------------
 *
 * relpath.h
 *		Declarations for GetRelationPath() and friends
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/relpath.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELPATH_H
#define RELPATH_H

/*
 *	Required here; note that CppAsString2() does not throw an error if the
 *	symbol is not defined.
 */
#include "catalog/catversion.h"

/*
 * RelFileNumber data type identifies the specific relation file name.
 */
typedef Oid RelFileNumber;
#define InvalidRelFileNumber		((RelFileNumber) InvalidOid)
#define RelFileNumberIsValid(relnumber) \
				((bool) ((relnumber) != InvalidRelFileNumber))

/*
 * Name of major-version-specific tablespace subdirectories
 */
#define TABLESPACE_VERSION_DIRECTORY	"PG_" PG_MAJORVERSION "_" \
									CppAsString2(CATALOG_VERSION_NO)

/*
 * Tablespace path (relative to installation's $PGDATA).
 *
 * These values should not be changed as many tools rely on it.
 */
#define PG_TBLSPC_DIR "pg_tblspc"
#define PG_TBLSPC_DIR_SLASH "pg_tblspc/"	/* required for strings
											 * comparisons */

/* Characters to allow for an OID in a relation path */
#define OIDCHARS		10		/* max chars printed by %u */

/*
 * Stuff for fork names.
 *
 * The physical storage of a relation consists of one or more forks.
 * The main fork is always created, but in addition to that there can be
 * additional forks for storing various metadata. ForkNumber is used when
 * we need to refer to a specific fork in a relation.
 */
typedef enum ForkNumber
{
	InvalidForkNumber = -1,
	MAIN_FORKNUM = 0,
	FSM_FORKNUM,
	VISIBILITYMAP_FORKNUM,
	INIT_FORKNUM,

	/*
	 * NOTE: if you add a new fork, change MAX_FORKNUM and possibly
	 * FORKNAMECHARS below, and update the forkNames array in
	 * src/common/relpath.c
	 */
} ForkNumber;

#define MAX_FORKNUM		INIT_FORKNUM

#define FORKNAMECHARS	4		/* max chars for a fork name */

extern PGDLLIMPORT const char *const forkNames[];

extern ForkNumber forkname_to_number(const char *forkName);
extern int	forkname_chars(const char *str, ForkNumber *fork);


/*
 * Unfortunately, there's no easy way to derive PROCNUMBER_CHARS from
 * MAX_BACKENDS. MAX_BACKENDS is 2^18-1. Crosschecked in test_relpath().
 */
#define PROCNUMBER_CHARS	6

/*
 * The longest possible relation path lengths is from the following format:
 * sprintf(rp.path, "%s/%u/%s/%u/t%d_%u",
 *         PG_TBLSPC_DIR, spcOid,
 *         TABLESPACE_VERSION_DIRECTORY,
 *         dbOid, procNumber, relNumber);
 *
 * Note this does *not* include the trailing null-byte, to make it easier to
 * combine it with other lengths.
 */
#define REL_PATH_STR_MAXLEN \
	( \
		sizeof(PG_TBLSPC_DIR) - 1 \
		+ sizeof((char)'/') \
		+ OIDCHARS /* spcOid */ \
		+ sizeof((char)'/') \
		+ sizeof(TABLESPACE_VERSION_DIRECTORY) - 1 \
		+ sizeof((char)'/') \
		+ OIDCHARS /* dbOid */ \
		+ sizeof((char)'/') \
		+ sizeof((char)'t') /* temporary table indicator */ \
		+ PROCNUMBER_CHARS /* procNumber */ \
		+ sizeof((char)'_') \
		+ OIDCHARS /* relNumber */ \
		+ sizeof((char)'_') \
		+ FORKNAMECHARS /* forkNames[forkNumber] */ \
	)

/*
 * String of the exact length required to represent a relation path. We return
 * this struct, instead of char[REL_PATH_STR_MAXLEN + 1], as the pointer would
 * decay to a plain char * too easily, possibly preventing the compiler from
 * detecting invalid references to the on-stack return value of
 * GetRelationPath().
 */
typedef struct RelPathStr
{
	char		str[REL_PATH_STR_MAXLEN + 1];
} RelPathStr;


/*
 * Stuff for computing filesystem pathnames for relations.
 */
extern char *GetDatabasePath(Oid dbOid, Oid spcOid);

extern RelPathStr GetRelationPath(Oid dbOid, Oid spcOid, RelFileNumber relNumber,
								  int procNumber, ForkNumber forkNumber);

/*
 * Wrapper macros for GetRelationPath.  Beware of multiple
 * evaluation of the RelFileLocator or RelFileLocatorBackend argument!
 */

/* First argument is a RelFileLocator */
#define relpathbackend(rlocator, backend, forknum) \
	GetRelationPath((rlocator).dbOid, (rlocator).spcOid, (rlocator).relNumber, \
					backend, forknum)

/* First argument is a RelFileLocator */
#define relpathperm(rlocator, forknum) \
	relpathbackend(rlocator, INVALID_PROC_NUMBER, forknum)

/* First argument is a RelFileLocatorBackend */
#define relpath(rlocator, forknum) \
	relpathbackend((rlocator).locator, (rlocator).backend, forknum)

#endif							/* RELPATH_H */
