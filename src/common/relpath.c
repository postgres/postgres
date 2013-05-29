/*-------------------------------------------------------------------------
 * relpath.c
 *		Shared frontend/backend code to find out pathnames of relation files
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/relpath.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "catalog/pg_tablespace.h"
#include "common/relpath.h"
#include "storage/backendid.h"

#define FORKNAMECHARS	4		/* max chars for a fork name */

/*
 * Lookup table of fork name by fork number.
 *
 * If you add a new entry, remember to update the errhint below, and the
 * documentation for pg_relation_size(). Also keep FORKNAMECHARS above
 * up-to-date.
 */
const char *forkNames[] = {
	"main",						/* MAIN_FORKNUM */
	"fsm",						/* FSM_FORKNUM */
	"vm",						/* VISIBILITYMAP_FORKNUM */
	"init"						/* INIT_FORKNUM */
};

/*
 * forkname_chars
 *		We use this to figure out whether a filename could be a relation
 *		fork (as opposed to an oddly named stray file that somehow ended
 *		up in the database directory).	If the passed string begins with
 *		a fork name (other than the main fork name), we return its length,
 *		and set *fork (if not NULL) to the fork number.  If not, we return 0.
 *
 * Note that the present coding assumes that there are no fork names which
 * are prefixes of other fork names.
 */
int
forkname_chars(const char *str, ForkNumber *fork)
{
	ForkNumber	forkNum;

	for (forkNum = 1; forkNum <= MAX_FORKNUM; forkNum++)
	{
		int			len = strlen(forkNames[forkNum]);

		if (strncmp(forkNames[forkNum], str, len) == 0)
		{
			if (fork)
				*fork = forkNum;
			return len;
		}
	}
	return 0;
}

/*
 * relpathbackend - construct path to a relation's file
 *
 * Result is a palloc'd string.
 */
char *
relpathbackend(RelFileNode rnode, BackendId backend, ForkNumber forknum)
{
	int			pathlen;
	char	   *path;

	if (rnode.spcNode == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(rnode.dbNode == 0);
		Assert(backend == InvalidBackendId);
		pathlen = 7 + OIDCHARS + 1 + FORKNAMECHARS + 1;
		path = (char *) palloc(pathlen);
		if (forknum != MAIN_FORKNUM)
			snprintf(path, pathlen, "global/%u_%s",
					 rnode.relNode, forkNames[forknum]);
		else
			snprintf(path, pathlen, "global/%u", rnode.relNode);
	}
	else if (rnode.spcNode == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		if (backend == InvalidBackendId)
		{
			pathlen = 5 + OIDCHARS + 1 + OIDCHARS + 1 + FORKNAMECHARS + 1;
			path = (char *) palloc(pathlen);
			if (forknum != MAIN_FORKNUM)
				snprintf(path, pathlen, "base/%u/%u_%s",
						 rnode.dbNode, rnode.relNode,
						 forkNames[forknum]);
			else
				snprintf(path, pathlen, "base/%u/%u",
						 rnode.dbNode, rnode.relNode);
		}
		else
		{
			/* OIDCHARS will suffice for an integer, too */
			pathlen = 5 + OIDCHARS + 2 + OIDCHARS + 1 + OIDCHARS + 1
				+ FORKNAMECHARS + 1;
			path = (char *) palloc(pathlen);
			if (forknum != MAIN_FORKNUM)
				snprintf(path, pathlen, "base/%u/t%d_%u_%s",
						 rnode.dbNode, backend, rnode.relNode,
						 forkNames[forknum]);
			else
				snprintf(path, pathlen, "base/%u/t%d_%u",
						 rnode.dbNode, backend, rnode.relNode);
		}
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		if (backend == InvalidBackendId)
		{
			pathlen = 9 + 1 + OIDCHARS + 1
				+ strlen(TABLESPACE_VERSION_DIRECTORY) + 1 + OIDCHARS + 1
				+ OIDCHARS + 1 + FORKNAMECHARS + 1;
			path = (char *) palloc(pathlen);
			if (forknum != MAIN_FORKNUM)
				snprintf(path, pathlen, "pg_tblspc/%u/%s/%u/%u_%s",
						 rnode.spcNode, TABLESPACE_VERSION_DIRECTORY,
						 rnode.dbNode, rnode.relNode,
						 forkNames[forknum]);
			else
				snprintf(path, pathlen, "pg_tblspc/%u/%s/%u/%u",
						 rnode.spcNode, TABLESPACE_VERSION_DIRECTORY,
						 rnode.dbNode, rnode.relNode);
		}
		else
		{
			/* OIDCHARS will suffice for an integer, too */
			pathlen = 9 + 1 + OIDCHARS + 1
				+ strlen(TABLESPACE_VERSION_DIRECTORY) + 1 + OIDCHARS + 2
				+ OIDCHARS + 1 + OIDCHARS + 1 + FORKNAMECHARS + 1;
			path = (char *) palloc(pathlen);
			if (forknum != MAIN_FORKNUM)
				snprintf(path, pathlen, "pg_tblspc/%u/%s/%u/t%d_%u_%s",
						 rnode.spcNode, TABLESPACE_VERSION_DIRECTORY,
						 rnode.dbNode, backend, rnode.relNode,
						 forkNames[forknum]);
			else
				snprintf(path, pathlen, "pg_tblspc/%u/%s/%u/t%d_%u",
						 rnode.spcNode, TABLESPACE_VERSION_DIRECTORY,
						 rnode.dbNode, backend, rnode.relNode);
		}
	}
	return path;
}
