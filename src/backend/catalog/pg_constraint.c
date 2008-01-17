/*-------------------------------------------------------------------------
 *
 * pg_constraint.c
 *	  routines to support manipulation of the pg_constraint relation
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_constraint.c,v 1.38 2008/01/17 18:56:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
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
					  const Oid *pfEqOp,
					  const Oid *ppEqOp,
					  const Oid *ffEqOp,
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
	ArrayType  *conpfeqopArray;
	ArrayType  *conppeqopArray;
	ArrayType  *conffeqopArray;
	NameData	cname;
	int			i;
	ObjectAddress conobject;

	conDesc = heap_open(ConstraintRelationId, RowExclusiveLock);

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
		Datum	   *fkdatums;

		fkdatums = (Datum *) palloc(foreignNKeys * sizeof(Datum));
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = Int16GetDatum(foreignKey[i]);
		confkeyArray = construct_array(fkdatums, foreignNKeys,
									   INT2OID, 2, true, 's');
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(pfEqOp[i]);
		conpfeqopArray = construct_array(fkdatums, foreignNKeys,
										 OIDOID, sizeof(Oid), true, 'i');
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(ppEqOp[i]);
		conppeqopArray = construct_array(fkdatums, foreignNKeys,
										 OIDOID, sizeof(Oid), true, 'i');
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(ffEqOp[i]);
		conffeqopArray = construct_array(fkdatums, foreignNKeys,
										 OIDOID, sizeof(Oid), true, 'i');
	}
	else
	{
		confkeyArray = NULL;
		conpfeqopArray = NULL;
		conppeqopArray = NULL;
		conffeqopArray = NULL;
	}

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

	if (conpfeqopArray)
		values[Anum_pg_constraint_conpfeqop - 1] = PointerGetDatum(conpfeqopArray);
	else
		nulls[Anum_pg_constraint_conpfeqop - 1] = 'n';

	if (conppeqopArray)
		values[Anum_pg_constraint_conppeqop - 1] = PointerGetDatum(conppeqopArray);
	else
		nulls[Anum_pg_constraint_conppeqop - 1] = 'n';

	if (conffeqopArray)
		values[Anum_pg_constraint_conffeqop - 1] = PointerGetDatum(conffeqopArray);
	else
		nulls[Anum_pg_constraint_conffeqop - 1] = 'n';

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

	conobject.classId = ConstraintRelationId;
	conobject.objectId = conOid;
	conobject.objectSubId = 0;

	heap_close(conDesc, RowExclusiveLock);

	if (OidIsValid(relId))
	{
		/*
		 * Register auto dependency from constraint to owning relation, or to
		 * specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		relobject.classId = RelationRelationId;
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

		domobject.classId = TypeRelationId;
		domobject.objectId = domainId;
		domobject.objectSubId = 0;

		recordDependencyOn(&conobject, &domobject, DEPENDENCY_AUTO);
	}

	if (OidIsValid(foreignRelId))
	{
		/*
		 * Register normal dependency from constraint to foreign relation, or
		 * to specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		relobject.classId = RelationRelationId;
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

		relobject.classId = RelationRelationId;
		relobject.objectId = indexRelId;
		relobject.objectSubId = 0;

		recordDependencyOn(&conobject, &relobject, DEPENDENCY_NORMAL);
	}

	if (foreignNKeys > 0)
	{
		/*
		 * Register normal dependencies on the equality operators that support
		 * a foreign-key constraint.  If the PK and FK types are the same then
		 * all three operators for a column are the same; otherwise they are
		 * different.
		 */
		ObjectAddress oprobject;

		oprobject.classId = OperatorRelationId;
		oprobject.objectSubId = 0;

		for (i = 0; i < foreignNKeys; i++)
		{
			oprobject.objectId = pfEqOp[i];
			recordDependencyOn(&conobject, &oprobject, DEPENDENCY_NORMAL);
			if (ppEqOp[i] != pfEqOp[i])
			{
				oprobject.objectId = ppEqOp[i];
				recordDependencyOn(&conobject, &oprobject, DEPENDENCY_NORMAL);
			}
			if (ffEqOp[i] != pfEqOp[i])
			{
				oprobject.objectId = ffEqOp[i];
				recordDependencyOn(&conobject, &oprobject, DEPENDENCY_NORMAL);
			}
		}
	}

	if (conExpr != NULL)
	{
		/*
		 * Register dependencies from constraint to objects mentioned in CHECK
		 * expression.
		 */
		recordDependencyOnSingleRelExpr(&conobject, conExpr, relId,
										DEPENDENCY_NORMAL,
										DEPENDENCY_NORMAL);
	}

	return conOid;
}


/*
 * Test whether given name is currently used as a constraint name
 * for the given object (relation or domain).
 *
 * This is used to decide whether to accept a user-specified constraint name.
 * It is deliberately not the same test as ChooseConstraintName uses to decide
 * whether an auto-generated name is OK: here, we will allow it unless there
 * is an identical constraint name in use *on the same object*.
 *
 * NB: Caller should hold exclusive lock on the given object, else
 * this test can be fooled by concurrent additions.
 */
bool
ConstraintNameIsUsed(ConstraintCategory conCat, Oid objId,
					 Oid objNamespace, const char *conname)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];
	HeapTuple	tup;

	conDesc = heap_open(ConstraintRelationId, AccessShareLock);

	found = false;

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	ScanKeyInit(&skey[1],
				Anum_pg_constraint_connamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objNamespace));

	conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
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
	heap_close(conDesc, AccessShareLock);

	return found;
}

/*
 * Select a nonconflicting name for a new constraint.
 *
 * The objective here is to choose a name that is unique within the
 * specified namespace.  Postgres does not require this, but the SQL
 * spec does, and some apps depend on it.  Therefore we avoid choosing
 * default names that so conflict.
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * 'others' can be a list of string names already chosen within the current
 * command (but not yet reflected into the catalogs); we will not choose
 * a duplicate of one of these either.
 *
 * Note: it is theoretically possible to get a collision anyway, if someone
 * else chooses the same name concurrently.  This is fairly unlikely to be
 * a problem in practice, especially if one is holding an exclusive lock on
 * the relation identified by name1.
 *
 * Returns a palloc'd string.
 */
char *
ChooseConstraintName(const char *name1, const char *name2,
					 const char *label, Oid namespace,
					 List *others)
{
	int			pass = 0;
	char	   *conname = NULL;
	char		modlabel[NAMEDATALEN];
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];
	bool		found;
	ListCell   *l;

	conDesc = heap_open(ConstraintRelationId, AccessShareLock);

	/* try the unmodified label first */
	StrNCpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		conname = makeObjectName(name1, name2, modlabel);

		found = false;

		foreach(l, others)
		{
			if (strcmp((char *) lfirst(l), conname) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			ScanKeyInit(&skey[0],
						Anum_pg_constraint_conname,
						BTEqualStrategyNumber, F_NAMEEQ,
						CStringGetDatum(conname));

			ScanKeyInit(&skey[1],
						Anum_pg_constraint_connamespace,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(namespace));

			conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
										 SnapshotNow, 2, skey);

			found = (HeapTupleIsValid(systable_getnext(conscan)));

			systable_endscan(conscan);
		}

		if (!found)
			break;

		/* found a conflict, so try a new name component */
		pfree(conname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	heap_close(conDesc, AccessShareLock);

	return conname;
}

/*
 * Delete a single constraint record.
 */
void
RemoveConstraintById(Oid conId)
{
	Relation	conDesc;
	HeapTuple	tup;
	Form_pg_constraint con;

	conDesc = heap_open(ConstraintRelationId, RowExclusiveLock);

	tup = SearchSysCache(CONSTROID,
						 ObjectIdGetDatum(conId),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tup);

	/*
	 * Special processing depending on what the constraint is for.
	 */
	if (OidIsValid(con->conrelid))
	{
		Relation	rel;

		/*
		 * If the constraint is for a relation, open and exclusive-lock the
		 * relation it's for.
		 */
		rel = heap_open(con->conrelid, AccessExclusiveLock);

		/*
		 * We need to update the relcheck count if it is a check constraint
		 * being dropped.  This update will force backends to rebuild relcache
		 * entries when we commit.
		 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Relation	pgrel;
			HeapTuple	relTup;
			Form_pg_class classForm;

			pgrel = heap_open(RelationRelationId, RowExclusiveLock);
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
		 * XXX for now, do nothing special when dropping a domain constraint
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
	ReleaseSysCache(tup);
	heap_close(conDesc, RowExclusiveLock);
}

/*
 * RenameConstraintById
 *		Rename a constraint.
 *
 * Note: this isn't intended to be a user-exposed function; it doesn't check
 * permissions etc.  Currently this is only invoked when renaming an index
 * that is associated with a constraint, but it's made a little more general
 * than that with the expectation of someday having ALTER TABLE RENAME
 * CONSTRAINT.
 */
void
RenameConstraintById(Oid conId, const char *newname)
{
	Relation	conDesc;
	HeapTuple	tuple;
	Form_pg_constraint con;

	conDesc = heap_open(ConstraintRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy(CONSTROID,
							   ObjectIdGetDatum(conId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	/*
	 * We need to check whether the name is already in use --- note that
	 * there currently is not a unique index that would catch this.
	 */
	if (OidIsValid(con->conrelid) &&
		ConstraintNameIsUsed(CONSTRAINT_RELATION,
							 con->conrelid,
							 con->connamespace,
							 newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for relation \"%s\" already exists",
						newname, get_rel_name(con->conrelid))));
	if (OidIsValid(con->contypid) &&
		ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
							 con->contypid,
							 con->connamespace,
							 newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for domain \"%s\" already exists",
						newname, format_type_be(con->contypid))));

	/* OK, do the rename --- tuple is a copy, so OK to scribble on it */
	namestrcpy(&(con->conname), newname);

	simple_heap_update(conDesc, &tuple->t_self, tuple);

	/* update the system catalog indexes */
	CatalogUpdateIndexes(conDesc, tuple);

	heap_freetuple(tuple);
	heap_close(conDesc, RowExclusiveLock);
}

/*
 * AlterConstraintNamespaces
 *		Find any constraints belonging to the specified object,
 *		and move them to the specified new namespace.
 *
 * isType indicates whether the owning object is a type or a relation.
 */
void
AlterConstraintNamespaces(Oid ownerId, Oid oldNspId,
						  Oid newNspId, bool isType)
{
	Relation	conRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	conRel = heap_open(ConstraintRelationId, RowExclusiveLock);

	if (isType)
	{
		ScanKeyInit(&key[0],
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ownerId));

		scan = systable_beginscan(conRel, ConstraintTypidIndexId, true,
								  SnapshotNow, 1, key);
	}
	else
	{
		ScanKeyInit(&key[0],
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ownerId));

		scan = systable_beginscan(conRel, ConstraintRelidIndexId, true,
								  SnapshotNow, 1, key);
	}

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(tup);

		if (conform->connamespace == oldNspId)
		{
			tup = heap_copytuple(tup);
			conform = (Form_pg_constraint) GETSTRUCT(tup);

			conform->connamespace = newNspId;

			simple_heap_update(conRel, &tup->t_self, tup);
			CatalogUpdateIndexes(conRel, tup);

			/*
			 * Note: currently, the constraint will not have its own
			 * dependency on the namespace, so we don't need to do
			 * changeDependencyFor().
			 */
		}
	}

	systable_endscan(scan);

	heap_close(conRel, RowExclusiveLock);
}
