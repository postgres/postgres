/*-------------------------------------------------------------------------
 * relpath.c
 *		Shared frontend/backend code to compute pathnames of relation files
 *
 * This module also contains some logic associated with fork names.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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

#include "catalog/pg_tablespace_d.h"
#include "common/relpath.h"
#include "storage/backendid.h"


/*
 * Lookup table of fork name by fork number.
 *
 * If you add a new entry, remember to update the errhint in
 * forkname_to_number() below, and update the SGML documentation for
 * pg_relation_size().
 */
const char *const forkNames[] = {
	"main",						/* MAIN_FORKNUM */
	"fsm",						/* FSM_FORKNUM */
	"vm",						/* VISIBILITYMAP_FORKNUM */
	"init"						/* INIT_FORKNUM */
};

StaticAssertDecl(lengthof(forkNames) == (MAX_FORKNUM + 1),
				 "array length mismatch");

/*
 * forkname_to_number - look up fork number by name
 *
 * In backend, we throw an error for no match; in frontend, we just
 * return InvalidForkNumber.
 */
ForkNumber
forkname_to_number(const char *forkName)
{
	ForkNumber	forkNum;

	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		if (strcmp(forkNames[forkNum], forkName) == 0)
			return forkNum;

#ifndef FRONTEND
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid fork name"),
			 errhint("Valid fork names are \"main\", \"fsm\", "
					 "\"vm\", and \"init\".")));
#endif

	return InvalidForkNumber;
}

/*
 * forkname_chars
 *		We use this to figure out whether a filename could be a relation
 *		fork (as opposed to an oddly named stray file that somehow ended
 *		up in the database directory).  If the passed string begins with
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
	if (fork)
		*fork = InvalidForkNumber;
	return 0;
}


/*
 * GetDatabasePath - construct path to a database directory
 *
 * Result is a palloc'd string.
 *
 * XXX this must agree with GetRelationPath()!
 */
char *
GetDatabasePath(Oid dbNode, Oid spcNode)
{
	if (spcNode == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(dbNode == 0);
		return pstrdup("global");
	}
	else if (spcNode == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		return psprintf("base/%u", dbNode);
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		return psprintf("pg_tblspc/%u/%s/%u",
						spcNode, TABLESPACE_VERSION_DIRECTORY, dbNode);
	}
}

/*
 * GetRelationPath - construct path to a relation's file
 *
 * Result is a palloc'd string.
 *
 * Note: ideally, backendId would be declared as type BackendId, but relpath.h
 * would have to include a backend-only header to do that; doesn't seem worth
 * the trouble considering BackendId is just int anyway.
 */
char *
GetRelationPath(Oid dbNode, Oid spcNode, Oid relNode,
				int backendId, ForkNumber forkNumber)
{
	char	   *path;

	if (spcNode == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(dbNode == 0);
		Assert(backendId == InvalidBackendId);
		if (forkNumber != MAIN_FORKNUM)
			path = psprintf("global/%u_%s",
							relNode, forkNames[forkNumber]);
		else
			path = psprintf("global/%u", relNode);
	}
	else if (spcNode == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		if (backendId == InvalidBackendId)
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("base/%u/%u_%s",
								dbNode, relNode,
								forkNames[forkNumber]);
			else
				path = psprintf("base/%u/%u",
								dbNode, relNode);
		}
		else
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("base/%u/t%d_%u_%s",
								dbNode, backendId, relNode,
								forkNames[forkNumber]);
			else
				path = psprintf("base/%u/t%d_%u",
								dbNode, backendId, relNode);
		}
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		if (backendId == InvalidBackendId)
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("pg_tblspc/%u/%s/%u/%u_%s",
								spcNode, TABLESPACE_VERSION_DIRECTORY,
								dbNode, relNode,
								forkNames[forkNumber]);
			else
				path = psprintf("pg_tblspc/%u/%s/%u/%u",
								spcNode, TABLESPACE_VERSION_DIRECTORY,
								dbNode, relNode);
		}
		else
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("pg_tblspc/%u/%s/%u/t%d_%u_%s",
								spcNode, TABLESPACE_VERSION_DIRECTORY,
								dbNode, backendId, relNode,
								forkNames[forkNumber]);
			else
				path = psprintf("pg_tblspc/%u/%s/%u/t%d_%u",
								spcNode, TABLESPACE_VERSION_DIRECTORY,
								dbNode, backendId, relNode);
		}
	}
	return path;
}
