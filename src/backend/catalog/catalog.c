/*-------------------------------------------------------------------------
 *
 * catalog.c
 *		routines concerned with catalog naming conventions
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/catalog.c,v 1.49 2003/08/04 02:39:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/pg_namespace.h"
#include "miscadmin.h"


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
 * IsSystemRelation
 *		True iff the relation is a system catalog relation.
 *
 *		NB: TOAST relations are considered system relations by this test
 *		for compatibility with the old IsSystemRelationName function.
 *		This is appropriate in many places but not all.  Where it's not,
 *		also check IsToastRelation.
 *
 *		We now just test if the relation is in the system catalog namespace;
 *		so it's no longer necessary to forbid user relations from having
 *		names starting with pg_.  Now only schema names have the pg_
 *		distinction/requirement.
 */
bool
IsSystemRelation(Relation relation)
{
	return IsSystemNamespace(RelationGetNamespace(relation)) ||
		IsToastNamespace(RelationGetNamespace(relation));
}

/*
 * IsSystemClass
 *		Like the above, but takes a Form_pg_class as argument.
 *		Used when we do not want to open the relation and have to
 *		search pg_class directly.
 */
bool
IsSystemClass(Form_pg_class reltuple)
{
	Oid			relnamespace = reltuple->relnamespace;

	return IsSystemNamespace(relnamespace) ||
		IsToastNamespace(relnamespace);
}

/*
 * IsToastRelation
 *		True iff relation is a TOAST support relation (or index).
 */
bool
IsToastRelation(Relation relation)
{
	return IsToastNamespace(RelationGetNamespace(relation));
}

/*
 * IsToastClass
 *		Like the above, but takes a Form_pg_class as argument.
 *		Used when we do not want to open the relation and have to
 *		search pg_class directly.
 */
bool
IsToastClass(Form_pg_class reltuple)
{
	Oid			relnamespace = reltuple->relnamespace;

	return IsToastNamespace(relnamespace);
}

/*
 * IsSystemNamespace
 *		True iff namespace is pg_catalog.
 *
 * NOTE: the reason this isn't a macro is to avoid having to include
 * catalog/pg_namespace.h in a lot of places.
 */
bool
IsSystemNamespace(Oid namespaceId)
{
	return namespaceId == PG_CATALOG_NAMESPACE;
}

/*
 * IsToastNamespace
 *		True iff namespace is pg_toast.
 *
 * NOTE: the reason this isn't a macro is to avoid having to include
 * catalog/pg_namespace.h in a lot of places.
 */
bool
IsToastNamespace(Oid namespaceId)
{
	return namespaceId == PG_TOAST_NAMESPACE;
}


/*
 * IsReservedName
 *		True iff name starts with the pg_ prefix.
 *
 *		For some classes of objects, the prefix pg_ is reserved
 *		for system objects only.
 */
bool
IsReservedName(const char *name)
{
	/* ugly coding for speed */
	return (name[0] == 'p' &&
			name[1] == 'g' &&
			name[2] == '_');
}


/*
 *		newoid			- returns a unique identifier across all catalogs.
 *
 *		Object Id allocation is now done by GetNewObjectID in
 *		access/transam/varsup.
 *
 *		This code probably needs to change to generate OIDs separately
 *		for each table.
 */
Oid
newoid(void)
{
	return GetNewObjectId();
}
