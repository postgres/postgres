/*-------------------------------------------------------------------------
 *
 * catalog.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/catalog.c,v 1.42 2001/05/30 20:52:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"

/*
 * relpath			- construct path to a relation's file
 *
 * Result is a palloc'd string.
 */

char *
relpath(RelFileNode rnode)
{
	char	   *path;

	if (rnode.tblNode == (Oid) 0)		/* "global tablespace" */
	{
		/* Shared system relations live in {datadir}/global */
		path = (char *) palloc(strlen(DataDir) + 8 + sizeof(NameData) + 1);
		sprintf(path, "%s/global/%u", DataDir, rnode.relNode);
	}
	else
	{
		path = (char *) palloc(strlen(DataDir) + 6 + 2 * sizeof(NameData) + 3);
		sprintf(path, "%s/base/%u/%u", DataDir, rnode.tblNode, rnode.relNode);
	}
	return path;
}

/*
 * GetDatabasePath			- construct path to a database dir
 *
 * Result is a palloc'd string.
 */

char *
GetDatabasePath(Oid tblNode)
{
	char	   *path;

	if (tblNode == (Oid) 0)		/* "global tablespace" */
	{
		/* Shared system relations live in {datadir}/global */
		path = (char *) palloc(strlen(DataDir) + 8);
		sprintf(path, "%s/global", DataDir);
	}
	else
	{
		path = (char *) palloc(strlen(DataDir) + 6 + sizeof(NameData) + 1);
		sprintf(path, "%s/base/%u", DataDir, tblNode);
	}
	return path;
}


/*
 * IsSystemRelationName
 *		True iff name is the name of a system catalog relation.
 *
 *		We now make a new requirement where system catalog relns must begin
 *		with pg_ while user relns are forbidden to do so.  Make the test
 *		trivial and instantaneous.
 *
 *		XXX this is way bogus. -- pma
 */
bool
IsSystemRelationName(const char *relname)
{
	if (relname[0] && relname[1] && relname[2])
		return (relname[0] == 'p' &&
				relname[1] == 'g' &&
				relname[2] == '_');
	else
		return FALSE;
}

/*
 * IsSharedSystemRelationName
 *		True iff name is the name of a shared system catalog relation.
 */
bool
IsSharedSystemRelationName(const char *relname)
{
	int			i;

	/*
	 * Quick out: if it's not a system relation, it can't be a shared
	 * system relation.
	 */
	if (!IsSystemRelationName(relname))
		return FALSE;

	i = 0;
	while (SharedSystemRelationNames[i] != NULL)
	{
		if (strcmp(SharedSystemRelationNames[i], relname) == 0)
			return TRUE;
		i++;
	}
	return FALSE;
}

/*
 *		newoid			- returns a unique identifier across all catalogs.
 *
 *		Object Id allocation is now done by GetNewObjectID in
 *		access/transam/varsup.c.  oids are now allocated correctly.
 *
 * old comments:
 *		This needs to change soon, it fails if there are too many more
 *		than one call per second when postgres restarts after it dies.
 *
 *		The distribution of OID's should be done by the POSTMASTER.
 *		Also there needs to be a facility to preallocate OID's.  Ie.,
 *		for a block of OID's to be declared as invalid ones to allow
 *		user programs to use them for temporary object identifiers.
 */
Oid
newoid(void)
{
	Oid			lastoid;

	GetNewObjectId(&lastoid);
	if (!OidIsValid(lastoid))
		elog(ERROR, "newoid: GetNewObjectId returns invalid oid");
	return lastoid;
}
