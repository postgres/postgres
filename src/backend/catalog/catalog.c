/*-------------------------------------------------------------------------
 *
 * catalog.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/catalog.c,v 1.36 2000/10/22 05:14:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/syscache.h"

#ifdef OLD_FILE_NAMING
/*
 * relpath				- construct path to a relation's file
 *
 * Note that this only works with relations that are visible to the current
 * backend, ie, either in the current database or shared system relations.
 *
 * Result is a palloc'd string.
 */
char *
relpath(const char *relname)
{
	char	   *path;

	if (IsSharedSystemRelationName(relname))
	{
		/* Shared system relations live in {datadir}/global */
		size_t		bufsize = strlen(DataDir) + 8 + sizeof(NameData) + 1;

		path = (char *) palloc(bufsize);
		snprintf(path, bufsize, "%s/global/%s", DataDir, relname);
		return path;
	}

	/*
	 * If it is in the current database, assume it is in current working
	 * directory.  NB: this does not work during bootstrap!
	 */
	return pstrdup(relname);
}

/*
 * relpath_blind			- construct path to a relation's file
 *
 * Construct the path using only the info available to smgrblindwrt,
 * namely the names and OIDs of the database and relation.	(Shared system
 * relations are identified with dbid = 0.)  Note that we may have to
 * access a relation belonging to a different database!
 *
 * Result is a palloc'd string.
 */

char *
relpath_blind(const char *dbname, const char *relname,
			  Oid dbid, Oid relid)
{
	char	   *path;

	if (dbid == (Oid) 0)
	{
		/* Shared system relations live in {datadir}/global */
		path = (char *) palloc(strlen(DataDir) + 8 + sizeof(NameData) + 1);
		sprintf(path, "%s/global/%s", DataDir, relname);
	}
	else if (dbid == MyDatabaseId)
	{
		/* XXX why is this inconsistent with relpath() ? */
		path = (char *) palloc(strlen(DatabasePath) + sizeof(NameData) + 2);
		sprintf(path, "%s%c%s", DatabasePath, SEP_CHAR, relname);
	}
	else
	{
		/* this is work around only !!! */
		char		dbpathtmp[MAXPGPATH];
		Oid			id;
		char	   *dbpath;

		GetRawDatabaseInfo(dbname, &id, dbpathtmp);

		if (id != dbid)
			elog(FATAL, "relpath_blind: oid of db %s is not %u",
				 dbname, dbid);
		dbpath = ExpandDatabasePath(dbpathtmp);
		if (dbpath == NULL)
			elog(FATAL, "relpath_blind: can't expand path for db %s",
				 dbname);
		path = (char *) palloc(strlen(dbpath) + sizeof(NameData) + 2);
		sprintf(path, "%s%c%s", dbpath, SEP_CHAR, relname);
		pfree(dbpath);
	}
	return path;
}

#else	/* ! OLD_FILE_NAMING */

/*
 * relpath			- construct path to a relation's file
 *
 * Result is a palloc'd string.
 */

char *
relpath(RelFileNode rnode)
{
	char	   *path;

	if (rnode.tblNode == (Oid) 0)	/* "global tablespace" */
	{
		/* Shared system relations live in {datadir}/global */
		path = (char *) palloc(strlen(DataDir) + 8 + sizeof(NameData) + 1);
		sprintf(path, "%s%cglobal%c%u", DataDir, SEP_CHAR, SEP_CHAR, rnode.relNode);
	}
	else
	{
		path = (char *) palloc(strlen(DataDir) + 6 + 2 * sizeof(NameData) + 3);
		sprintf(path, "%s%cbase%c%u%c%u", DataDir, SEP_CHAR, SEP_CHAR, 
			rnode.tblNode, SEP_CHAR, rnode.relNode);
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

	if (tblNode == (Oid) 0)	/* "global tablespace" */
	{
		/* Shared system relations live in {datadir}/global */
		path = (char *) palloc(strlen(DataDir) + 8);
		sprintf(path, "%s%cglobal", DataDir, SEP_CHAR);
	}
	else
	{
		path = (char *) palloc(strlen(DataDir) + 6 + sizeof(NameData) + 1);
		sprintf(path, "%s%cbase%c%u", DataDir, SEP_CHAR, SEP_CHAR, tblNode);
	}
	return path;
}

#endif	/* OLD_FILE_NAMING */

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
newoid()
{
	Oid			lastoid;

	GetNewObjectId(&lastoid);
	if (!OidIsValid(lastoid))
		elog(ERROR, "newoid: GetNewObjectId returns invalid oid");
	return lastoid;
}

/*
 *		fillatt			- fills the ATTRIBUTE relation fields from the TYP
 *
 *		Expects that the atttypid domain is set for each att[].
 *		Returns with the attnum, and attlen domains set.
 *		attnum, attproc, atttyparg, ... should be set by the user.
 *
 *		In the future, attnum may not be set?!? or may be passed as an arg?!?
 *
 *		Current implementation is very inefficient--should cashe the
 *		information if this is at all possible.
 *
 *		Check to see if this is really needed, and especially in the case
 *		of index tuples.
 */
void
fillatt(TupleDesc tupleDesc)
{
	Form_pg_attribute *attributeP;
	Form_pg_type typp;
	HeapTuple	tuple;
	int			i;
	int			natts = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	if (natts < 0 || natts > MaxHeapAttributeNumber)
		elog(ERROR, "fillatt: %d attributes is too large", natts);
	if (natts == 0)
	{
		elog(DEBUG, "fillatt: called with natts == 0");
		return;
	}

	attributeP = &att[0];

	for (i = 0; i < natts;)
	{
		tuple = SearchSysCacheTuple(TYPEOID,
							   ObjectIdGetDatum((*attributeP)->atttypid),
									0, 0, 0);
		if (!HeapTupleIsValid(tuple))
		{
			elog(ERROR, "fillatt: unknown atttypid %d",
				 (*attributeP)->atttypid);
		}
		else
		{
			(*attributeP)->attnum = (int16) ++i;

			/*
			 * Check if the attr is a set before messing with the length
			 * and byval, since those were already set in
			 * TupleDescInitEntry.	In fact, this seems redundant here,
			 * but who knows what I'll break if I take it out...
			 */
			if (!(*attributeP)->attisset)
			{
				typp = (Form_pg_type) GETSTRUCT(tuple); /* XXX */
				(*attributeP)->attlen = typp->typlen;
				(*attributeP)->attbyval = typp->typbyval;
			}
		}
		attributeP += 1;
	}
}
