/*-------------------------------------------------------------------------
 * relpath.c
 *		Shared frontend/backend code to compute pathnames of relation files
 *
 * This module also contains some logic associated with fork names.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "storage/procnumber.h"


/*
 * Lookup table of fork name by fork number.
 *
 * If you add a new entry, remember to update the errhint in
 * forkname_to_number() below, and update the SGML documentation for
 * pg_relation_size().
 */
const char *const forkNames[] = {
	[MAIN_FORKNUM] = "main",
	[FSM_FORKNUM] = "fsm",
	[VISIBILITYMAP_FORKNUM] = "vm",
	[INIT_FORKNUM] = "init",
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
GetDatabasePath(Oid dbOid, Oid spcOid)
{
	if (spcOid == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(dbOid == 0);
		return pstrdup("global");
	}
	else if (spcOid == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		return psprintf("base/%u", dbOid);
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		return psprintf("%s/%u/%s/%u",
						PG_TBLSPC_DIR, spcOid,
						TABLESPACE_VERSION_DIRECTORY, dbOid);
	}
}

/*
 * GetRelationPath - construct path to a relation's file
 *
 * Result is a palloc'd string.
 *
 * Note: ideally, procNumber would be declared as type ProcNumber, but
 * relpath.h would have to include a backend-only header to do that; doesn't
 * seem worth the trouble considering ProcNumber is just int anyway.
 */
char *
GetRelationPath(Oid dbOid, Oid spcOid, RelFileNumber relNumber,
				int procNumber, ForkNumber forkNumber)
{
	char	   *path;

	if (spcOid == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(dbOid == 0);
		Assert(procNumber == INVALID_PROC_NUMBER);
		if (forkNumber != MAIN_FORKNUM)
			path = psprintf("global/%u_%s",
							relNumber, forkNames[forkNumber]);
		else
			path = psprintf("global/%u", relNumber);
	}
	else if (spcOid == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		if (procNumber == INVALID_PROC_NUMBER)
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("base/%u/%u_%s",
								dbOid, relNumber,
								forkNames[forkNumber]);
			else
				path = psprintf("base/%u/%u",
								dbOid, relNumber);
		}
		else
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("base/%u/t%d_%u_%s",
								dbOid, procNumber, relNumber,
								forkNames[forkNumber]);
			else
				path = psprintf("base/%u/t%d_%u",
								dbOid, procNumber, relNumber);
		}
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		if (procNumber == INVALID_PROC_NUMBER)
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("%s/%u/%s/%u/%u_%s",
								PG_TBLSPC_DIR, spcOid,
								TABLESPACE_VERSION_DIRECTORY,
								dbOid, relNumber,
								forkNames[forkNumber]);
			else
				path = psprintf("%s/%u/%s/%u/%u",
								PG_TBLSPC_DIR, spcOid,
								TABLESPACE_VERSION_DIRECTORY,
								dbOid, relNumber);
		}
		else
		{
			if (forkNumber != MAIN_FORKNUM)
				path = psprintf("%s/%u/%s/%u/t%d_%u_%s",
								PG_TBLSPC_DIR, spcOid,
								TABLESPACE_VERSION_DIRECTORY,
								dbOid, procNumber, relNumber,
								forkNames[forkNumber]);
			else
				path = psprintf("%s/%u/%s/%u/t%d_%u",
								PG_TBLSPC_DIR, spcOid,
								TABLESPACE_VERSION_DIRECTORY,
								dbOid, procNumber, relNumber);
		}
	}
	return path;
}
