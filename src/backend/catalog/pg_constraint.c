/*-------------------------------------------------------------------------
 *
 * pg_constraint.c
 *	  routines to support manipulation of the pg_constraint relation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_constraint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "common/int.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CreateConstraintEntry
 *	Create a constraint table entry.
 *
 * Subsidiary records (such as triggers or indexes to implement the
 * constraint) are *not* created here.  But we do make dependency links
 * from the constraint to the things it depends on.
 *
 * The new constraint's OID is returned.
 */
Oid
CreateConstraintEntry(const char *constraintName,
					  Oid constraintNamespace,
					  char constraintType,
					  bool isDeferrable,
					  bool isDeferred,
					  bool isEnforced,
					  bool isValidated,
					  Oid parentConstrId,
					  Oid relId,
					  const int16 *constraintKey,
					  int constraintNKeys,
					  int constraintNTotalKeys,
					  Oid domainId,
					  Oid indexRelId,
					  Oid foreignRelId,
					  const int16 *foreignKey,
					  const Oid *pfEqOp,
					  const Oid *ppEqOp,
					  const Oid *ffEqOp,
					  int foreignNKeys,
					  char foreignUpdateType,
					  char foreignDeleteType,
					  const int16 *fkDeleteSetCols,
					  int numFkDeleteSetCols,
					  char foreignMatchType,
					  const Oid *exclOp,
					  Node *conExpr,
					  const char *conBin,
					  bool conIsLocal,
					  int16 conInhCount,
					  bool conNoInherit,
					  bool conPeriod,
					  bool is_internal)
{
	Relation	conDesc;
	Oid			conOid;
	HeapTuple	tup;
	bool		nulls[Natts_pg_constraint];
	Datum		values[Natts_pg_constraint];
	ArrayType  *conkeyArray;
	ArrayType  *confkeyArray;
	ArrayType  *conpfeqopArray;
	ArrayType  *conppeqopArray;
	ArrayType  *conffeqopArray;
	ArrayType  *conexclopArray;
	ArrayType  *confdelsetcolsArray;
	NameData	cname;
	int			i;
	ObjectAddress conobject;
	ObjectAddresses *addrs_auto;
	ObjectAddresses *addrs_normal;

	/* Only CHECK or FOREIGN KEY constraint can be not enforced */
	Assert(isEnforced || constraintType == CONSTRAINT_CHECK ||
		   constraintType == CONSTRAINT_FOREIGN);
	/* NOT ENFORCED constraint must be NOT VALID */
	Assert(isEnforced || !isValidated);

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

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
		conkeyArray = construct_array_builtin(conkey, constraintNKeys, INT2OID);
	}
	else
		conkeyArray = NULL;

	if (foreignNKeys > 0)
	{
		Datum	   *fkdatums;
		int			nkeys = Max(foreignNKeys, numFkDeleteSetCols);

		fkdatums = (Datum *) palloc(nkeys * sizeof(Datum));
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = Int16GetDatum(foreignKey[i]);
		confkeyArray = construct_array_builtin(fkdatums, foreignNKeys, INT2OID);
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(pfEqOp[i]);
		conpfeqopArray = construct_array_builtin(fkdatums, foreignNKeys, OIDOID);
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(ppEqOp[i]);
		conppeqopArray = construct_array_builtin(fkdatums, foreignNKeys, OIDOID);
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(ffEqOp[i]);
		conffeqopArray = construct_array_builtin(fkdatums, foreignNKeys, OIDOID);

		if (numFkDeleteSetCols > 0)
		{
			for (i = 0; i < numFkDeleteSetCols; i++)
				fkdatums[i] = Int16GetDatum(fkDeleteSetCols[i]);
			confdelsetcolsArray = construct_array_builtin(fkdatums, numFkDeleteSetCols, INT2OID);
		}
		else
			confdelsetcolsArray = NULL;
	}
	else
	{
		confkeyArray = NULL;
		conpfeqopArray = NULL;
		conppeqopArray = NULL;
		conffeqopArray = NULL;
		confdelsetcolsArray = NULL;
	}

	if (exclOp != NULL)
	{
		Datum	   *opdatums;

		opdatums = (Datum *) palloc(constraintNKeys * sizeof(Datum));
		for (i = 0; i < constraintNKeys; i++)
			opdatums[i] = ObjectIdGetDatum(exclOp[i]);
		conexclopArray = construct_array_builtin(opdatums, constraintNKeys, OIDOID);
	}
	else
		conexclopArray = NULL;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_constraint; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	conOid = GetNewOidWithIndex(conDesc, ConstraintOidIndexId,
								Anum_pg_constraint_oid);
	values[Anum_pg_constraint_oid - 1] = ObjectIdGetDatum(conOid);
	values[Anum_pg_constraint_conname - 1] = NameGetDatum(&cname);
	values[Anum_pg_constraint_connamespace - 1] = ObjectIdGetDatum(constraintNamespace);
	values[Anum_pg_constraint_contype - 1] = CharGetDatum(constraintType);
	values[Anum_pg_constraint_condeferrable - 1] = BoolGetDatum(isDeferrable);
	values[Anum_pg_constraint_condeferred - 1] = BoolGetDatum(isDeferred);
	values[Anum_pg_constraint_conenforced - 1] = BoolGetDatum(isEnforced);
	values[Anum_pg_constraint_convalidated - 1] = BoolGetDatum(isValidated);
	values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(relId);
	values[Anum_pg_constraint_contypid - 1] = ObjectIdGetDatum(domainId);
	values[Anum_pg_constraint_conindid - 1] = ObjectIdGetDatum(indexRelId);
	values[Anum_pg_constraint_conparentid - 1] = ObjectIdGetDatum(parentConstrId);
	values[Anum_pg_constraint_confrelid - 1] = ObjectIdGetDatum(foreignRelId);
	values[Anum_pg_constraint_confupdtype - 1] = CharGetDatum(foreignUpdateType);
	values[Anum_pg_constraint_confdeltype - 1] = CharGetDatum(foreignDeleteType);
	values[Anum_pg_constraint_confmatchtype - 1] = CharGetDatum(foreignMatchType);
	values[Anum_pg_constraint_conislocal - 1] = BoolGetDatum(conIsLocal);
	values[Anum_pg_constraint_coninhcount - 1] = Int16GetDatum(conInhCount);
	values[Anum_pg_constraint_connoinherit - 1] = BoolGetDatum(conNoInherit);
	values[Anum_pg_constraint_conperiod - 1] = BoolGetDatum(conPeriod);

	if (conkeyArray)
		values[Anum_pg_constraint_conkey - 1] = PointerGetDatum(conkeyArray);
	else
		nulls[Anum_pg_constraint_conkey - 1] = true;

	if (confkeyArray)
		values[Anum_pg_constraint_confkey - 1] = PointerGetDatum(confkeyArray);
	else
		nulls[Anum_pg_constraint_confkey - 1] = true;

	if (conpfeqopArray)
		values[Anum_pg_constraint_conpfeqop - 1] = PointerGetDatum(conpfeqopArray);
	else
		nulls[Anum_pg_constraint_conpfeqop - 1] = true;

	if (conppeqopArray)
		values[Anum_pg_constraint_conppeqop - 1] = PointerGetDatum(conppeqopArray);
	else
		nulls[Anum_pg_constraint_conppeqop - 1] = true;

	if (conffeqopArray)
		values[Anum_pg_constraint_conffeqop - 1] = PointerGetDatum(conffeqopArray);
	else
		nulls[Anum_pg_constraint_conffeqop - 1] = true;

	if (confdelsetcolsArray)
		values[Anum_pg_constraint_confdelsetcols - 1] = PointerGetDatum(confdelsetcolsArray);
	else
		nulls[Anum_pg_constraint_confdelsetcols - 1] = true;

	if (conexclopArray)
		values[Anum_pg_constraint_conexclop - 1] = PointerGetDatum(conexclopArray);
	else
		nulls[Anum_pg_constraint_conexclop - 1] = true;

	if (conBin)
		values[Anum_pg_constraint_conbin - 1] = CStringGetTextDatum(conBin);
	else
		nulls[Anum_pg_constraint_conbin - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(conDesc), values, nulls);

	CatalogTupleInsert(conDesc, tup);

	ObjectAddressSet(conobject, ConstraintRelationId, conOid);

	table_close(conDesc, RowExclusiveLock);

	/* Handle set of auto dependencies */
	addrs_auto = new_object_addresses();

	if (OidIsValid(relId))
	{
		/*
		 * Register auto dependency from constraint to owning relation, or to
		 * specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		if (constraintNTotalKeys > 0)
		{
			for (i = 0; i < constraintNTotalKeys; i++)
			{
				ObjectAddressSubSet(relobject, RelationRelationId, relId,
									constraintKey[i]);
				add_exact_object_address(&relobject, addrs_auto);
			}
		}
		else
		{
			ObjectAddressSet(relobject, RelationRelationId, relId);
			add_exact_object_address(&relobject, addrs_auto);
		}
	}

	if (OidIsValid(domainId))
	{
		/*
		 * Register auto dependency from constraint to owning domain
		 */
		ObjectAddress domobject;

		ObjectAddressSet(domobject, TypeRelationId, domainId);
		add_exact_object_address(&domobject, addrs_auto);
	}

	record_object_address_dependencies(&conobject, addrs_auto,
									   DEPENDENCY_AUTO);
	free_object_addresses(addrs_auto);

	/* Handle set of normal dependencies */
	addrs_normal = new_object_addresses();

	if (OidIsValid(foreignRelId))
	{
		/*
		 * Register normal dependency from constraint to foreign relation, or
		 * to specific column(s) if any are mentioned.
		 */
		ObjectAddress relobject;

		if (foreignNKeys > 0)
		{
			for (i = 0; i < foreignNKeys; i++)
			{
				ObjectAddressSubSet(relobject, RelationRelationId,
									foreignRelId, foreignKey[i]);
				add_exact_object_address(&relobject, addrs_normal);
			}
		}
		else
		{
			ObjectAddressSet(relobject, RelationRelationId, foreignRelId);
			add_exact_object_address(&relobject, addrs_normal);
		}
	}

	if (OidIsValid(indexRelId) && constraintType == CONSTRAINT_FOREIGN)
	{
		/*
		 * Register normal dependency on the unique index that supports a
		 * foreign-key constraint.  (Note: for indexes associated with unique
		 * or primary-key constraints, the dependency runs the other way, and
		 * is not made here.)
		 */
		ObjectAddress relobject;

		ObjectAddressSet(relobject, RelationRelationId, indexRelId);
		add_exact_object_address(&relobject, addrs_normal);
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
			add_exact_object_address(&oprobject, addrs_normal);
			if (ppEqOp[i] != pfEqOp[i])
			{
				oprobject.objectId = ppEqOp[i];
				add_exact_object_address(&oprobject, addrs_normal);
			}
			if (ffEqOp[i] != pfEqOp[i])
			{
				oprobject.objectId = ffEqOp[i];
				add_exact_object_address(&oprobject, addrs_normal);
			}
		}
	}

	record_object_address_dependencies(&conobject, addrs_normal,
									   DEPENDENCY_NORMAL);
	free_object_addresses(addrs_normal);

	/*
	 * We don't bother to register dependencies on the exclusion operators of
	 * an exclusion constraint.  We assume they are members of the opclass
	 * supporting the index, so there's an indirect dependency via that. (This
	 * would be pretty dicey for cross-type operators, but exclusion operators
	 * can never be cross-type.)
	 */

	if (conExpr != NULL)
	{
		/*
		 * Register dependencies from constraint to objects mentioned in CHECK
		 * expression.
		 */
		recordDependencyOnSingleRelExpr(&conobject, conExpr, relId,
										DEPENDENCY_NORMAL,
										DEPENDENCY_NORMAL, false);
	}

	/* Post creation hook for new constraint */
	InvokeObjectPostCreateHookArg(ConstraintRelationId, conOid, 0,
								  is_internal);

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
					 const char *conname)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[3];

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum((conCat == CONSTRAINT_RELATION)
								 ? objId : InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum((conCat == CONSTRAINT_DOMAIN)
								 ? objId : InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	conscan = systable_beginscan(conDesc, ConstraintRelidTypidNameIndexId,
								 true, NULL, 3, skey);

	/* There can be at most one matching row */
	found = (HeapTupleIsValid(systable_getnext(conscan)));

	systable_endscan(conscan);
	table_close(conDesc, AccessShareLock);

	return found;
}

/*
 * Does any constraint of the given name exist in the given namespace?
 *
 * This is used for code that wants to match ChooseConstraintName's rule
 * that we should avoid autogenerating duplicate constraint names within a
 * namespace.
 */
bool
ConstraintNameExists(const char *conname, Oid namespaceid)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	ScanKeyInit(&skey[1],
				Anum_pg_constraint_connamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(namespaceid));

	conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
								 NULL, 2, skey);

	found = (HeapTupleIsValid(systable_getnext(conscan)));

	systable_endscan(conscan);
	table_close(conDesc, AccessShareLock);

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
 * If the given label is empty, we only consider names that include at least
 * one added digit.
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
					 const char *label, Oid namespaceid,
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

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	/* try the unmodified label first, unless it's empty */
	if (label[0] != '\0')
		strlcpy(modlabel, label, sizeof(modlabel));
	else
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);

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
						ObjectIdGetDatum(namespaceid));

			conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
										 NULL, 2, skey);

			found = (HeapTupleIsValid(systable_getnext(conscan)));

			systable_endscan(conscan);
		}

		if (!found)
			break;

		/* found a conflict, so try a new name component */
		pfree(conname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	table_close(conDesc, AccessShareLock);

	return conname;
}

/*
 * Find and return a copy of the pg_constraint tuple that implements a
 * (possibly not valid) not-null constraint for the given column of the
 * given relation.  If no such constraint exists, return NULL.
 *
 * XXX This would be easier if we had pg_attribute.notnullconstr with the OID
 * of the constraint that implements the not-null constraint for that column.
 * I'm not sure it's worth the catalog bloat and de-normalization, however.
 */
HeapTuple
findNotNullConstraintAttnum(Oid relid, AttrNumber attnum)
{
	Relation	pg_constraint;
	HeapTuple	conTup,
				retval = NULL;
	SysScanDesc scan;
	ScanKeyData key;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(conTup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);
		AttrNumber	conkey;

		/*
		 * We're looking for a NOTNULL constraint with the column we're
		 * looking for as the sole element in conkey.
		 */
		if (con->contype != CONSTRAINT_NOTNULL)
			continue;

		conkey = extractNotNullColumn(conTup);
		if (conkey != attnum)
			continue;

		/* Found it */
		retval = heap_copytuple(conTup);
		break;
	}

	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	return retval;
}

/*
 * Find and return a copy of the pg_constraint tuple that implements a
 * (possibly not valid) not-null constraint for the given column of the
 * given relation.
 * If no such column or no such constraint exists, return NULL.
 */
HeapTuple
findNotNullConstraint(Oid relid, const char *colname)
{
	AttrNumber	attnum;

	attnum = get_attnum(relid, colname);
	if (attnum <= InvalidAttrNumber)
		return NULL;

	return findNotNullConstraintAttnum(relid, attnum);
}

/*
 * Find and return the pg_constraint tuple that implements a validated
 * not-null constraint for the given domain.
 */
HeapTuple
findDomainNotNullConstraint(Oid typid)
{
	Relation	pg_constraint;
	HeapTuple	conTup,
				retval = NULL;
	SysScanDesc scan;
	ScanKeyData key;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));
	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(conTup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);

		/*
		 * We're looking for a NOTNULL constraint that's marked validated.
		 */
		if (con->contype != CONSTRAINT_NOTNULL)
			continue;
		if (!con->convalidated)
			continue;

		/* Found it */
		retval = heap_copytuple(conTup);
		break;
	}

	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	return retval;
}

/*
 * Given a pg_constraint tuple for a not-null constraint, return the column
 * number it is for.
 */
AttrNumber
extractNotNullColumn(HeapTuple constrTup)
{
	Datum		adatum;
	ArrayType  *arr;

	/* only tuples for not-null constraints should be given */
	Assert(((Form_pg_constraint) GETSTRUCT(constrTup))->contype == CONSTRAINT_NOTNULL);

	adatum = SysCacheGetAttrNotNull(CONSTROID, constrTup,
									Anum_pg_constraint_conkey);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID ||
		ARR_DIMS(arr)[0] != 1)
		elog(ERROR, "conkey is not a 1-D smallint array");

	/* We leak the detoasted datum, but we don't care */

	return ((AttrNumber *) ARR_DATA_PTR(arr))[0];
}

/*
 * AdjustNotNullInheritance
 *		Adjust inheritance status for a single not-null constraint
 *
 * If no not-null constraint is found for the column, return false.
 * Caller can create one.
 *
 * If a constraint exists but the connoinherit flag is not what the caller
 * wants, throw an error about the incompatibility.  If the desired
 * constraint is valid but the existing constraint is not valid, also
 * throw an error about that (the opposite case is acceptable).
 *
 * If everything checks out, we adjust conislocal/coninhcount and return
 * true.  If is_local is true we flip conislocal true, or do nothing if
 * it's already true; otherwise we increment coninhcount by 1.
 */
bool
AdjustNotNullInheritance(Oid relid, AttrNumber attnum,
						 bool is_local, bool is_no_inherit, bool is_notvalid)
{
	HeapTuple	tup;

	tup = findNotNullConstraintAttnum(relid, attnum);
	if (HeapTupleIsValid(tup))
	{
		Relation	pg_constraint;
		Form_pg_constraint conform;
		bool		changed = false;

		pg_constraint = table_open(ConstraintRelationId, RowExclusiveLock);
		conform = (Form_pg_constraint) GETSTRUCT(tup);

		/*
		 * If the NO INHERIT flag we're asked for doesn't match what the
		 * existing constraint has, throw an error.
		 */
		if (is_no_inherit != conform->connoinherit)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot change NO INHERIT status of NOT NULL constraint \"%s\" on relation \"%s\"",
						   NameStr(conform->conname), get_rel_name(relid)),
					errhint("You might need to make the existing constraint inheritable using %s.",
							"ALTER TABLE ... ALTER CONSTRAINT ... INHERIT"));

		/*
		 * Throw an error if the existing constraint is NOT VALID and caller
		 * wants a valid one.
		 */
		if (!is_notvalid && !conform->convalidated)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("incompatible NOT VALID constraint \"%s\" on relation \"%s\"",
						   NameStr(conform->conname), get_rel_name(relid)),
					errhint("You might need to validate it using %s.",
							"ALTER TABLE ... VALIDATE CONSTRAINT"));

		if (!is_local)
		{
			if (pg_add_s16_overflow(conform->coninhcount, 1,
									&conform->coninhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
			changed = true;
		}
		else if (!conform->conislocal)
		{
			conform->conislocal = true;
			changed = true;
		}

		if (changed)
			CatalogTupleUpdate(pg_constraint, &tup->t_self, tup);

		table_close(pg_constraint, RowExclusiveLock);

		return true;
	}

	return false;
}

/*
 * RelationGetNotNullConstraints
 *		Return the list of not-null constraints for the given rel
 *
 * Caller can request cooked constraints, or raw.
 *
 * This is seldom needed, so we just scan pg_constraint each time.
 *
 * 'include_noinh' determines whether to include NO INHERIT constraints or not.
 */
List *
RelationGetNotNullConstraints(Oid relid, bool cooked, bool include_noinh)
{
	List	   *notnulls = NIL;
	Relation	constrRel;
	HeapTuple	htup;
	SysScanDesc conscan;
	ScanKeyData skey;

	constrRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	conscan = systable_beginscan(constrRel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(conscan)))
	{
		Form_pg_constraint conForm = (Form_pg_constraint) GETSTRUCT(htup);
		AttrNumber	colnum;

		if (conForm->contype != CONSTRAINT_NOTNULL)
			continue;
		if (conForm->connoinherit && !include_noinh)
			continue;

		colnum = extractNotNullColumn(htup);

		if (cooked)
		{
			CookedConstraint *cooked;

			cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));

			cooked->contype = CONSTR_NOTNULL;
			cooked->conoid = conForm->oid;
			cooked->name = pstrdup(NameStr(conForm->conname));
			cooked->attnum = colnum;
			cooked->expr = NULL;
			cooked->is_enforced = true;
			cooked->skip_validation = !conForm->convalidated;
			cooked->is_local = true;
			cooked->inhcount = 0;
			cooked->is_no_inherit = conForm->connoinherit;

			notnulls = lappend(notnulls, cooked);
		}
		else
		{
			Constraint *constr;

			constr = makeNode(Constraint);
			constr->contype = CONSTR_NOTNULL;
			constr->conname = pstrdup(NameStr(conForm->conname));
			constr->deferrable = false;
			constr->initdeferred = false;
			constr->location = -1;
			constr->keys = list_make1(makeString(get_attname(relid, colnum,
															 false)));
			constr->is_enforced = true;
			constr->skip_validation = !conForm->convalidated;
			constr->initially_valid = true;
			constr->is_no_inherit = conForm->connoinherit;
			notnulls = lappend(notnulls, constr);
		}
	}

	systable_endscan(conscan);
	table_close(constrRel, AccessShareLock);

	return notnulls;
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

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conId));
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
		rel = table_open(con->conrelid, AccessExclusiveLock);

		/*
		 * We need to update the relchecks count if it is a check constraint
		 * being dropped.  This update will force backends to rebuild relcache
		 * entries when we commit.
		 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Relation	pgrel;
			HeapTuple	relTup;
			Form_pg_class classForm;

			pgrel = table_open(RelationRelationId, RowExclusiveLock);
			relTup = SearchSysCacheCopy1(RELOID,
										 ObjectIdGetDatum(con->conrelid));
			if (!HeapTupleIsValid(relTup))
				elog(ERROR, "cache lookup failed for relation %u",
					 con->conrelid);
			classForm = (Form_pg_class) GETSTRUCT(relTup);

			if (classForm->relchecks == 0)	/* should not happen */
				elog(ERROR, "relation \"%s\" has relchecks = 0",
					 RelationGetRelationName(rel));
			classForm->relchecks--;

			CatalogTupleUpdate(pgrel, &relTup->t_self, relTup);

			heap_freetuple(relTup);

			table_close(pgrel, RowExclusiveLock);
		}

		/* Keep lock on constraint's rel until end of xact */
		table_close(rel, NoLock);
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
	CatalogTupleDelete(conDesc, &tup->t_self);

	/* Clean up */
	ReleaseSysCache(tup);
	table_close(conDesc, RowExclusiveLock);
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

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(CONSTROID, ObjectIdGetDatum(conId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	/*
	 * For user-friendliness, check whether the name is already in use.
	 */
	if (OidIsValid(con->conrelid) &&
		ConstraintNameIsUsed(CONSTRAINT_RELATION,
							 con->conrelid,
							 newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for relation \"%s\" already exists",
						newname, get_rel_name(con->conrelid))));
	if (OidIsValid(con->contypid) &&
		ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
							 con->contypid,
							 newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for domain %s already exists",
						newname, format_type_be(con->contypid))));

	/* OK, do the rename --- tuple is a copy, so OK to scribble on it */
	namestrcpy(&(con->conname), newname);

	CatalogTupleUpdate(conDesc, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, conId, 0);

	heap_freetuple(tuple);
	table_close(conDesc, RowExclusiveLock);
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
						  Oid newNspId, bool isType, ObjectAddresses *objsMoved)
{
	Relation	conRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	conRel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(isType ? InvalidOid : ownerId));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(isType ? ownerId : InvalidOid));

	scan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(tup);
		ObjectAddress thisobj;

		ObjectAddressSet(thisobj, ConstraintRelationId, conform->oid);

		if (object_address_present(&thisobj, objsMoved))
			continue;

		/* Don't update if the object is already part of the namespace */
		if (conform->connamespace == oldNspId && oldNspId != newNspId)
		{
			tup = heap_copytuple(tup);
			conform = (Form_pg_constraint) GETSTRUCT(tup);

			conform->connamespace = newNspId;

			CatalogTupleUpdate(conRel, &tup->t_self, tup);

			/*
			 * Note: currently, the constraint will not have its own
			 * dependency on the namespace, so we don't need to do
			 * changeDependencyFor().
			 */
		}

		InvokeObjectPostAlterHook(ConstraintRelationId, thisobj.objectId, 0);

		add_exact_object_address(&thisobj, objsMoved);
	}

	systable_endscan(scan);

	table_close(conRel, RowExclusiveLock);
}

/*
 * ConstraintSetParentConstraint
 *		Set a partition's constraint as child of its parent constraint,
 *		or remove the linkage if parentConstrId is InvalidOid.
 *
 * This updates the constraint's pg_constraint row to show it as inherited, and
 * adds PARTITION dependencies to prevent the constraint from being deleted
 * on its own.  Alternatively, reverse that.
 */
void
ConstraintSetParentConstraint(Oid childConstrId,
							  Oid parentConstrId,
							  Oid childTableId)
{
	Relation	constrRel;
	Form_pg_constraint constrForm;
	HeapTuple	tuple,
				newtup;
	ObjectAddress depender;
	ObjectAddress referenced;

	constrRel = table_open(ConstraintRelationId, RowExclusiveLock);
	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(childConstrId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", childConstrId);
	newtup = heap_copytuple(tuple);
	constrForm = (Form_pg_constraint) GETSTRUCT(newtup);
	if (OidIsValid(parentConstrId))
	{
		/* don't allow setting parent for a constraint that already has one */
		Assert(constrForm->coninhcount == 0);
		if (constrForm->conparentid != InvalidOid)
			elog(ERROR, "constraint %u already has a parent constraint",
				 childConstrId);

		constrForm->conislocal = false;
		if (pg_add_s16_overflow(constrForm->coninhcount, 1,
								&constrForm->coninhcount))
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("too many inheritance parents"));

		constrForm->conparentid = parentConstrId;

		CatalogTupleUpdate(constrRel, &tuple->t_self, newtup);

		ObjectAddressSet(depender, ConstraintRelationId, childConstrId);

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstrId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_PRI);

		ObjectAddressSet(referenced, RelationRelationId, childTableId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_SEC);
	}
	else
	{
		constrForm->coninhcount--;
		constrForm->conislocal = true;
		constrForm->conparentid = InvalidOid;

		/* Make sure there's no further inheritance. */
		Assert(constrForm->coninhcount == 0);

		CatalogTupleUpdate(constrRel, &tuple->t_self, newtup);

		deleteDependencyRecordsForClass(ConstraintRelationId, childConstrId,
										ConstraintRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(ConstraintRelationId, childConstrId,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);
	}

	ReleaseSysCache(tuple);
	table_close(constrRel, RowExclusiveLock);
}


/*
 * get_relation_constraint_oid
 *		Find a constraint on the specified relation with the specified name.
 *		Returns constraint's OID.
 */
Oid
get_relation_constraint_oid(Oid relid, const char *conname, bool missing_ok)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[3];
	Oid			conOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		conOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

	systable_endscan(scan);

	/* If no such constraint exists, complain */
	if (!OidIsValid(conOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for table \"%s\" does not exist",
						conname, get_rel_name(relid))));

	table_close(pg_constraint, AccessShareLock);

	return conOid;
}

/*
 * get_relation_constraint_attnos
 *		Find a constraint on the specified relation with the specified name
 *		and return the constrained columns.
 *
 * Returns a Bitmapset of the column attnos of the constrained columns, with
 * attnos being offset by FirstLowInvalidHeapAttributeNumber so that system
 * columns can be represented.
 *
 * *constraintOid is set to the OID of the constraint, or InvalidOid on
 * failure.
 */
Bitmapset *
get_relation_constraint_attnos(Oid relid, const char *conname,
							   bool missing_ok, Oid *constraintOid)
{
	Bitmapset  *conattnos = NULL;
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[3];

	/* Set *constraintOid, to avoid complaints about uninitialized vars */
	*constraintOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Datum		adatum;
		bool		isNull;

		*constraintOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

		/* Extract the conkey array, ie, attnums of constrained columns */
		adatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pg_constraint), &isNull);
		if (!isNull)
		{
			ArrayType  *arr;
			int			numcols;
			int16	   *attnums;
			int			i;

			arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
			numcols = ARR_DIMS(arr)[0];
			if (ARR_NDIM(arr) != 1 ||
				numcols < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != INT2OID)
				elog(ERROR, "conkey is not a 1-D smallint array");
			attnums = (int16 *) ARR_DATA_PTR(arr);

			/* Construct the result value */
			for (i = 0; i < numcols; i++)
			{
				conattnos = bms_add_member(conattnos,
										   attnums[i] - FirstLowInvalidHeapAttributeNumber);
			}
		}
	}

	systable_endscan(scan);

	/* If no such constraint exists, complain */
	if (!OidIsValid(*constraintOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for table \"%s\" does not exist",
						conname, get_rel_name(relid))));

	table_close(pg_constraint, AccessShareLock);

	return conattnos;
}

/*
 * Return the OID of the constraint enforced by the given index in the
 * given relation; or InvalidOid if no such index is cataloged.
 *
 * Much like get_constraint_index, this function is concerned only with the
 * one constraint that "owns" the given index.  Therefore, constraints of
 * types other than unique, primary-key, and exclusion are ignored.
 */
Oid
get_relation_idx_constraint_oid(Oid relationId, Oid indexId)
{
	Relation	pg_constraint;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Oid			constraintId = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relationId));
	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, &key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm;

		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* See above */
		if (constrForm->contype != CONSTRAINT_PRIMARY &&
			constrForm->contype != CONSTRAINT_UNIQUE &&
			constrForm->contype != CONSTRAINT_EXCLUSION)
			continue;

		if (constrForm->conindid == indexId)
		{
			constraintId = constrForm->oid;
			break;
		}
	}
	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);
	return constraintId;
}

/*
 * get_domain_constraint_oid
 *		Find a constraint on the specified domain with the specified name.
 *		Returns constraint's OID.
 */
Oid
get_domain_constraint_oid(Oid typid, const char *conname, bool missing_ok)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[3];
	Oid			conOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		conOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

	systable_endscan(scan);

	/* If no such constraint exists, complain */
	if (!OidIsValid(conOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for domain %s does not exist",
						conname, format_type_be(typid))));

	table_close(pg_constraint, AccessShareLock);

	return conOid;
}

/*
 * get_primary_key_attnos
 *		Identify the columns in a relation's primary key, if any.
 *
 * Returns a Bitmapset of the column attnos of the primary key's columns,
 * with attnos being offset by FirstLowInvalidHeapAttributeNumber so that
 * system columns can be represented.
 *
 * If there is no primary key, return NULL.  We also return NULL if the pkey
 * constraint is deferrable and deferrableOk is false.
 *
 * *constraintOid is set to the OID of the pkey constraint, or InvalidOid
 * on failure.
 */
Bitmapset *
get_primary_key_attnos(Oid relid, bool deferrableOk, Oid *constraintOid)
{
	Bitmapset  *pkattnos = NULL;
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/* Set *constraintOid, to avoid complaints about uninitialized vars */
	*constraintOid = InvalidOid;

	/* Scan pg_constraint for constraints of the target rel */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
		Datum		adatum;
		bool		isNull;
		ArrayType  *arr;
		int16	   *attnums;
		int			numkeys;
		int			i;

		/* Skip constraints that are not PRIMARY KEYs */
		if (con->contype != CONSTRAINT_PRIMARY)
			continue;

		/*
		 * If the primary key is deferrable, but we've been instructed to
		 * ignore deferrable constraints, then we might as well give up
		 * searching, since there can only be a single primary key on a table.
		 */
		if (con->condeferrable && !deferrableOk)
			break;

		/* Extract the conkey array, ie, attnums of PK's columns */
		adatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pg_constraint), &isNull);
		if (isNull)
			elog(ERROR, "null conkey for constraint %u",
				 ((Form_pg_constraint) GETSTRUCT(tuple))->oid);
		arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
		numkeys = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numkeys < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != INT2OID)
			elog(ERROR, "conkey is not a 1-D smallint array");
		attnums = (int16 *) ARR_DATA_PTR(arr);

		/* Construct the result value */
		for (i = 0; i < numkeys; i++)
		{
			pkattnos = bms_add_member(pkattnos,
									  attnums[i] - FirstLowInvalidHeapAttributeNumber);
		}
		*constraintOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

		/* No need to search further */
		break;
	}

	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);

	return pkattnos;
}

/*
 * Extract data from the pg_constraint tuple of a foreign-key constraint.
 *
 * All arguments save the first are output arguments.  All output arguments
 * other than numfks, conkey and confkey can be passed as NULL if caller
 * doesn't need them.
 */
void
DeconstructFkConstraintRow(HeapTuple tuple, int *numfks,
						   AttrNumber *conkey, AttrNumber *confkey,
						   Oid *pf_eq_oprs, Oid *pp_eq_oprs, Oid *ff_eq_oprs,
						   int *num_fk_del_set_cols, AttrNumber *fk_del_set_cols)
{
	Datum		adatum;
	bool		isNull;
	ArrayType  *arr;
	int			numkeys;

	/*
	 * We expect the arrays to be 1-D arrays of the right types; verify that.
	 * We don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of values.
	 */
	adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
									Anum_pg_constraint_conkey);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID)
		elog(ERROR, "conkey is not a 1-D smallint array");
	numkeys = ARR_DIMS(arr)[0];
	if (numkeys <= 0 || numkeys > INDEX_MAX_KEYS)
		elog(ERROR, "foreign key constraint cannot have %d columns", numkeys);
	memcpy(conkey, ARR_DATA_PTR(arr), numkeys * sizeof(int16));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
									Anum_pg_constraint_confkey);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	if (ARR_NDIM(arr) != 1 ||
		ARR_DIMS(arr)[0] != numkeys ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID)
		elog(ERROR, "confkey is not a 1-D smallint array");
	memcpy(confkey, ARR_DATA_PTR(arr), numkeys * sizeof(int16));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	if (pf_eq_oprs)
	{
		adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
										Anum_pg_constraint_conpfeqop);
		arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
		/* see TryReuseForeignKey if you change the test below */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numkeys ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conpfeqop is not a 1-D Oid array");
		memcpy(pf_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
		if ((Pointer) arr != DatumGetPointer(adatum))
			pfree(arr);			/* free de-toasted copy, if any */
	}

	if (pp_eq_oprs)
	{
		adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
										Anum_pg_constraint_conppeqop);
		arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numkeys ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conppeqop is not a 1-D Oid array");
		memcpy(pp_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
		if ((Pointer) arr != DatumGetPointer(adatum))
			pfree(arr);			/* free de-toasted copy, if any */
	}

	if (ff_eq_oprs)
	{
		adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
										Anum_pg_constraint_conffeqop);
		arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numkeys ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conffeqop is not a 1-D Oid array");
		memcpy(ff_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
		if ((Pointer) arr != DatumGetPointer(adatum))
			pfree(arr);			/* free de-toasted copy, if any */
	}

	if (fk_del_set_cols)
	{
		adatum = SysCacheGetAttr(CONSTROID, tuple,
								 Anum_pg_constraint_confdelsetcols, &isNull);
		if (isNull)
		{
			*num_fk_del_set_cols = 0;
		}
		else
		{
			int			num_delete_cols;

			arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
			if (ARR_NDIM(arr) != 1 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != INT2OID)
				elog(ERROR, "confdelsetcols is not a 1-D smallint array");
			num_delete_cols = ARR_DIMS(arr)[0];
			memcpy(fk_del_set_cols, ARR_DATA_PTR(arr), num_delete_cols * sizeof(int16));
			if ((Pointer) arr != DatumGetPointer(adatum))
				pfree(arr);		/* free de-toasted copy, if any */

			*num_fk_del_set_cols = num_delete_cols;
		}
	}

	*numfks = numkeys;
}

/*
 * FindFKPeriodOpers -
 *
 * Looks up the operator oids used for the PERIOD part of a temporal foreign key.
 * The opclass should be the opclass of that PERIOD element.
 * Everything else is an output: containedbyoperoid is the ContainedBy operator for
 * types matching the PERIOD element.
 * aggedcontainedbyoperoid is also a ContainedBy operator,
 * but one whose rhs is a multirange.
 * That way foreign keys can compare fkattr <@ range_agg(pkattr).
 * intersectoperoid is used by NO ACTION constraints to trim the range being considered
 * to just what was updated/deleted.
 */
void
FindFKPeriodOpers(Oid opclass,
				  Oid *containedbyoperoid,
				  Oid *aggedcontainedbyoperoid,
				  Oid *intersectoperoid)
{
	Oid			opfamily = InvalidOid;
	Oid			opcintype = InvalidOid;
	StrategyNumber strat;

	/* Make sure we have a range or multirange. */
	if (get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		if (opcintype != ANYRANGEOID && opcintype != ANYMULTIRANGEOID)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("invalid type for PERIOD part of foreign key"),
					errdetail("Only range and multirange are supported."));

	}
	else
		elog(ERROR, "cache lookup failed for opclass %u", opclass);

	/*
	 * Look up the ContainedBy operator whose lhs and rhs are the opclass's
	 * type. We use this to optimize RI checks: if the new value includes all
	 * of the old value, then we can treat the attribute as if it didn't
	 * change, and skip the RI check.
	 */
	GetOperatorFromCompareType(opclass,
							   InvalidOid,
							   COMPARE_CONTAINED_BY,
							   containedbyoperoid,
							   &strat);

	/*
	 * Now look up the ContainedBy operator. Its left arg must be the type of
	 * the column (or rather of the opclass). Its right arg must match the
	 * return type of the support proc.
	 */
	GetOperatorFromCompareType(opclass,
							   ANYMULTIRANGEOID,
							   COMPARE_CONTAINED_BY,
							   aggedcontainedbyoperoid,
							   &strat);

	switch (opcintype)
	{
		case ANYRANGEOID:
			*intersectoperoid = OID_RANGE_INTERSECT_RANGE_OP;
			break;
		case ANYMULTIRANGEOID:
			*intersectoperoid = OID_MULTIRANGE_INTERSECT_MULTIRANGE_OP;
			break;
		default:
			elog(ERROR, "unexpected opcintype: %u", opcintype);
	}
}

/*
 * Determine whether a relation can be proven functionally dependent on
 * a set of grouping columns.  If so, return true and add the pg_constraint
 * OIDs of the constraints needed for the proof to the *constraintDeps list.
 *
 * grouping_columns is a list of grouping expressions, in which columns of
 * the rel of interest are Vars with the indicated varno/varlevelsup.
 *
 * Currently we only check to see if the rel has a primary key that is a
 * subset of the grouping_columns.  We could also use plain unique constraints
 * if all their columns are known not null, but there's a problem: we need
 * to be able to represent the not-null-ness as part of the constraints added
 * to *constraintDeps.  FIXME whenever not-null constraints get represented
 * in pg_constraint.
 */
bool
check_functional_grouping(Oid relid,
						  Index varno, Index varlevelsup,
						  List *grouping_columns,
						  List **constraintDeps)
{
	Bitmapset  *pkattnos;
	Bitmapset  *groupbyattnos;
	Oid			constraintOid;
	ListCell   *gl;

	/* If the rel has no PK, then we can't prove functional dependency */
	pkattnos = get_primary_key_attnos(relid, false, &constraintOid);
	if (pkattnos == NULL)
		return false;

	/* Identify all the rel's columns that appear in grouping_columns */
	groupbyattnos = NULL;
	foreach(gl, grouping_columns)
	{
		Var		   *gvar = (Var *) lfirst(gl);

		if (IsA(gvar, Var) &&
			gvar->varno == varno &&
			gvar->varlevelsup == varlevelsup)
			groupbyattnos = bms_add_member(groupbyattnos,
										   gvar->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	if (bms_is_subset(pkattnos, groupbyattnos))
	{
		/* The PK is a subset of grouping_columns, so we win */
		*constraintDeps = lappend_oid(*constraintDeps, constraintOid);
		return true;
	}

	return false;
}
