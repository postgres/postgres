/*-------------------------------------------------------------------------
 *
 * pg_constraint.c
 *	  routines to support manipulation of the pg_constraint relation
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_constraint.c,v 1.16 2003/08/04 02:39:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


/*
 * CreateConstraintEntry
 *	Create a constraint table entry.
 *
 * Subsidiary records (such as triggers or indexes to implement the
 * constraint) are *not* created here.	But we do make dependency links
 * from the constraint to the things it depends on.
 */
Oid
CreateConstraintEntry(const char *constraintName,
					  Oid constraintNamespace,
					  char constraintType,
					  bool isDeferrable,
					  bool isDeferred,
					  Oid relId,
					  const int16 *constraintKey,
					  int constraintNKeys,
					  Oid domainId,
					  Oid foreignRelId,
					  const int16 *foreignKey,
					  int foreignNKeys,
					  char foreignUpdateType,
					  char foreignDeleteType,
					  char foreignMatchType,
					  Oid indexRelId,
					  Node *conExpr,
					  const char *conBin,
					  const char *conSrc)
{
	Relation	conDesc;
	Oid			conOid;
	HeapTuple	tup;
	char		nulls[Natts_pg_constraint];
	Datum		values[Natts_pg_constraint];
	ArrayType  *conkeyArray;
	ArrayType  *confkeyArray;
	NameData	cname;
	int			i;
	ObjectAddress conobject;

	conDesc = heap_openr(ConstraintRelationName, RowExclusiveLock);

	Assert(constraintName);
	namestrcpy(&cname, constraintName);

	/*
	 * Convert C arrays into Postgres arrays.
	 */
	if (constraintNKeys > 0)
	{
		Datum	   *conkey;

		conkey = (Datum *) palloc(constraintNKeys * sizeof(Datum));
		for (i = 0; i < constraintNKeys; i++)
			conkey[i] = Int16GetDatum(constraintKey[i]);
		conkeyArray = construct_array(conkey, constraintNKeys,
									  INT2OID, 2, true, 's');
	}
	else
		conkeyArray = NULL;

	if (foreignNKeys > 0)
	{
		Datum	   *confkey;

		confkey = (Datum *) palloc(foreignNKeys * sizeof(Datum));
		for (i = 0; i < foreignNKeys; i++)
			confkey[i] = Int16GetDatum(foreignKey[i]);
		confkeyArray = construct_array(confkey, foreignNKeys,
									   INT2OID, 2, true, 's');
	}
	else
		confkeyArray = NULL;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_constraint; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}

	values[Anum_pg_constraint_conname - 1] = NameGetDatum(&cname);
	values[Anum_pg_constraint_connamespace - 1] = ObjectIdGetDatum(constraintNamespace);
	values[Anum_pg_constraint_contype - 1] = CharGetDatum(constraintType);
	values[Anum_pg_constraint_condeferrable - 1] = BoolGetDatum(isDeferrable);
	values[Anum_pg_constraint_condeferred - 1] = BoolGetDatum(isDeferred);
	values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(relId);
	values[Anum_pg_constraint_contypid - 1] = ObjectIdGetDatum(domainId);
	values[Anum_pg_constraint_confrelid - 1] = ObjectIdGetDatum(foreignRelId);
	values[Anum_pg_constraint_confupdtype - 1] = CharGetDatum(foreignUpdateType);
	values[Anum_pg_constraint_confdeltype - 1] = CharGetDatum(foreignDeleteType);
	values[Anum_pg_constraint_confmatchtype - 1] = CharGetDatum(foreignMatchType);

	if (conkeyArray)
		values[Anum_pg_constraint_conkey - 1] = PointerGetDatum(conkeyArray);
	else
		nulls[Anum_pg_constraint_conkey - 1] = 'n';

	if (confkeyArray)
		values[Anum_pg_constraint_confkey - 1] = PointerGetDatum(confkeyArray);
	else
		nulls[Anum_pg_constraint_confkey - 1] = 'n';

	/*
	 * initialize the binary form of the check constraint.
	 */
	if (conBin)
		values[Anum_pg_constraint_conbin - 1] = DirectFunctionCall1(textin,
												CStringGetDatum(conBin));
	else
		nulls[Anum_pg_constraint_conbin - 1] = 'n';

	/*
	 * initialize the text form of the check constraint
	 */
	if (conSrc)
		values[Anum_pg_constraint_consrc - 1] = DirectFunctionCall1(textin,
												CStringGetDatum(conSrc));
	else
		nulls[Anum_pg_constraint_consrc - 1] = 'n';

	tup = heap_formtuple(RelationGetDescr(conDesc), values, nulls);

	conOid = simple_heap_insert(conDesc, tup);

	/* update catalog indexes */
	CatalogUpdateIndexes(conDesc, tup);

	conobject.classId = RelationGetRelid(conDesc);
	conobject.objectId = conOid;
	conobject.objectSubId = 0;

	heap_close(conDesc, RowExclusiveLock);

	if (OidIsValid(relId))
	{
		/*
		 * Register auto dependency from constraint to owning relation, or
		 * to specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		relobject.classId = RelOid_pg_class;
		relobject.objectId = relId;
		if (constraintNKeys > 0)
		{
			for (i = 0; i < constraintNKeys; i++)
			{
				relobject.objectSubId = constraintKey[i];

				recordDependencyOn(&conobject, &relobject, DEPENDENCY_AUTO);
			}
		}
		else
		{
			relobject.objectSubId = 0;

			recordDependencyOn(&conobject, &relobject, DEPENDENCY_AUTO);
		}
	}

	if (OidIsValid(domainId))
	{
		/*
		 * Register auto dependency from constraint to owning domain
		 */
		ObjectAddress domobject;

		domobject.classId = RelOid_pg_type;
		domobject.objectId = domainId;
		domobject.objectSubId = 0;

		recordDependencyOn(&conobject, &domobject, DEPENDENCY_AUTO);
	}

	if (OidIsValid(foreignRelId))
	{
		/*
		 * Register normal dependency from constraint to foreign relation,
		 * or to specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		relobject.classId = RelOid_pg_class;
		relobject.objectId = foreignRelId;
		if (foreignNKeys > 0)
		{
			for (i = 0; i < foreignNKeys; i++)
			{
				relobject.objectSubId = foreignKey[i];

				recordDependencyOn(&conobject, &relobject, DEPENDENCY_NORMAL);
			}
		}
		else
		{
			relobject.objectSubId = 0;

			recordDependencyOn(&conobject, &relobject, DEPENDENCY_NORMAL);
		}
	}

	if (OidIsValid(indexRelId))
	{
		/*
		 * Register normal dependency on the unique index that supports a
		 * foreign-key constraint.
		 */
		ObjectAddress relobject;

		relobject.classId = RelOid_pg_class;
		relobject.objectId = indexRelId;
		relobject.objectSubId = 0;

		recordDependencyOn(&conobject, &relobject, DEPENDENCY_NORMAL);
	}

	if (conExpr != NULL)
	{
		/*
		 * Register dependencies from constraint to objects mentioned in
		 * CHECK expression.
		 */
		recordDependencyOnSingleRelExpr(&conobject, conExpr, relId,
										DEPENDENCY_NORMAL,
										DEPENDENCY_NORMAL);
	}

	return conOid;
}


/*
 * Test whether given name is currently used as a constraint name
 * for the given relation.
 *
 * NB: Caller should hold exclusive lock on the given relation, else
 * this test is not very meaningful.
 */
bool
ConstraintNameIsUsed(CONSTRAINTCATEGORY conCat, Oid objId, Oid objNamespace, const char *cname)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];
	HeapTuple	tup;

	conDesc = heap_openr(ConstraintRelationName, RowExclusiveLock);

	found = false;

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   Anum_pg_constraint_conname, F_NAMEEQ,
						   CStringGetDatum(cname));

	ScanKeyEntryInitialize(&skey[1], 0x0,
						   Anum_pg_constraint_connamespace, F_OIDEQ,
						   ObjectIdGetDatum(objNamespace));

	conscan = systable_beginscan(conDesc, ConstraintNameNspIndex, true,
								 SnapshotNow, 2, skey);

	while (HeapTupleIsValid(tup = systable_getnext(conscan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

		if (conCat == CONSTRAINT_RELATION && con->conrelid == objId)
		{
			found = true;
			break;
		}
		else if (conCat == CONSTRAINT_DOMAIN && con->contypid == objId)
		{
			found = true;
			break;
		}
	}

	systable_endscan(conscan);
	heap_close(conDesc, RowExclusiveLock);

	return found;
}

/*
 * Generate a currently-unused constraint name for the given relation.
 *
 * The passed counter should be initialized to 0 the first time through.
 * If multiple constraint names are to be generated in a single command,
 * pass the new counter value to each successive call, else the same
 * name will be generated each time.
 *
 * NB: Caller should hold exclusive lock on the given relation, else
 * someone else might choose the same name concurrently!
 */
char *
GenerateConstraintName(CONSTRAINTCATEGORY conCat, Oid objId, Oid objNamespace, int *counter)
{
	bool		found;
	Relation	conDesc;
	char	   *cname;

	cname = (char *) palloc(NAMEDATALEN * sizeof(char));

	conDesc = heap_openr(ConstraintRelationName, RowExclusiveLock);

	/* Loop until we find a non-conflicting constraint name */
	/* We assume there will be one eventually ... */
	do
	{
		SysScanDesc conscan;
		ScanKeyData skey[2];
		HeapTuple	tup;

		++(*counter);
		snprintf(cname, NAMEDATALEN, "$%d", *counter);

		/*
		 * This duplicates ConstraintNameIsUsed() so that we can avoid
		 * re-opening pg_constraint for each iteration.
		 */
		found = false;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   Anum_pg_constraint_conname, F_NAMEEQ,
							   CStringGetDatum(cname));

		ScanKeyEntryInitialize(&skey[1], 0x0,
							   Anum_pg_constraint_connamespace, F_OIDEQ,
							   ObjectIdGetDatum(objNamespace));

		conscan = systable_beginscan(conDesc, ConstraintNameNspIndex, true,
									 SnapshotNow, 2, skey);

		while (HeapTupleIsValid(tup = systable_getnext(conscan)))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

			if (conCat == CONSTRAINT_RELATION && con->conrelid == objId)
			{
				found = true;
				break;
			}
			else if (conCat == CONSTRAINT_DOMAIN && con->contypid == objId)
			{
				found = true;
				break;
			}
		}

		systable_endscan(conscan);
	} while (found);

	heap_close(conDesc, RowExclusiveLock);

	return cname;
}

/*
 * Does the given name look like a generated constraint name?
 *
 * This is a test on the form of the name, *not* on whether it has
 * actually been assigned.
 */
bool
ConstraintNameIsGenerated(const char *cname)
{
	if (cname[0] != '$')
		return false;
	if (strspn(cname + 1, "0123456789") != strlen(cname + 1))
		return false;
	return true;
}

/*
 * Delete a single constraint record.
 */
void
RemoveConstraintById(Oid conId)
{
	Relation	conDesc;
	ScanKeyData skey[1];
	SysScanDesc conscan;
	HeapTuple	tup;
	Form_pg_constraint con;

	conDesc = heap_openr(ConstraintRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(conId));

	conscan = systable_beginscan(conDesc, ConstraintOidIndex, true,
								 SnapshotNow, 1, skey);

	tup = systable_getnext(conscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tup);

	/*
	 * Special processing depending on what the constraint is for.
	 */
	if (OidIsValid(con->conrelid))
	{
		Relation	rel;

		/*
		 * If the constraint is for a relation, open and exclusive-lock
		 * the relation it's for.
		 */
		rel = heap_open(con->conrelid, AccessExclusiveLock);

		/*
		 * We need to update the relcheck count if it is a check
		 * constraint being dropped.  This update will force backends to
		 * rebuild relcache entries when we commit.
		 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Relation	pgrel;
			HeapTuple	relTup;
			Form_pg_class classForm;

			pgrel = heap_openr(RelationRelationName, RowExclusiveLock);
			relTup = SearchSysCacheCopy(RELOID,
										ObjectIdGetDatum(con->conrelid),
										0, 0, 0);
			if (!HeapTupleIsValid(relTup))
				elog(ERROR, "cache lookup failed for relation %u",
					 con->conrelid);
			classForm = (Form_pg_class) GETSTRUCT(relTup);

			if (classForm->relchecks == 0)		/* should not happen */
				elog(ERROR, "relation \"%s\" has relchecks = 0",
					 RelationGetRelationName(rel));
			classForm->relchecks--;

			simple_heap_update(pgrel, &relTup->t_self, relTup);

			CatalogUpdateIndexes(pgrel, relTup);

			heap_freetuple(relTup);

			heap_close(pgrel, RowExclusiveLock);
		}

		/* Keep lock on constraint's rel until end of xact */
		heap_close(rel, NoLock);
	}
	else if (OidIsValid(con->contypid))
	{
		/*
		 * XXX for now, do nothing special when dropping a domain
		 * constraint
		 *
		 * Probably there should be some form of locking on the domain type,
		 * but we have no such concept at the moment.
		 */
	}
	else
		elog(ERROR, "constraint %u is not of a known type", conId);

	/* Fry the constraint itself */
	simple_heap_delete(conDesc, &tup->t_self);

	/* Clean up */
	systable_endscan(conscan);
	heap_close(conDesc, RowExclusiveLock);
}
