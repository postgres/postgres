/*-------------------------------------------------------------------------
 *
 * opclasscmds.c
 *
 *	  Routines for opclass manipulation commands
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/opclasscmds.c,v 1.21 2003/09/26 15:27:31 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void storeOperators(Oid opclassoid, int numOperators,
			   Oid *operators, bool *recheck);
static void storeProcedures(Oid opclassoid, int numProcs, Oid *procedures);


/*
 * DefineOpClass
 *		Define a new index operator class.
 */
void
DefineOpClass(CreateOpClassStmt *stmt)
{
	char	   *opcname;		/* name of opclass we're creating */
	Oid			amoid,			/* our AM's oid */
				typeoid,		/* indexable datatype oid */
				storageoid,		/* storage datatype oid, if any */
				namespaceoid,	/* namespace to create opclass in */
				opclassoid;		/* oid of opclass we create */
	int			numOperators,	/* amstrategies value */
				numProcs;		/* amsupport value */
	Oid		   *operators,		/* oids of operators, by strategy num */
			   *procedures;		/* oids of support procs */
	bool	   *recheck;		/* do operators need recheck */
	List	   *iteml;
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_opclass];
	char		nulls[Natts_pg_opclass];
	AclResult	aclresult;
	NameData	opcName;
	int			i;
	ObjectAddress myself,
				referenced;

	/* Convert list of names to a name and namespace */
	namespaceoid = QualifiedNameGetCreationNamespace(stmt->opclassname,
													 &opcname);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceoid));

	/* Get necessary info about access method */
	tup = SearchSysCache(AMNAME,
						 CStringGetDatum(stmt->amname),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						stmt->amname)));

	amoid = HeapTupleGetOid(tup);
	numOperators = ((Form_pg_am) GETSTRUCT(tup))->amstrategies;
	numProcs = ((Form_pg_am) GETSTRUCT(tup))->amsupport;

	/* XXX Should we make any privilege check against the AM? */

	ReleaseSysCache(tup);

	/*
	 * Currently, we require superuser privileges to create an opclass.
	 * This seems necessary because we have no way to validate that the
	 * offered set of operators and functions are consistent with the AM's
	 * expectations.  It would be nice to provide such a check someday, if
	 * it can be done without solving the halting problem :-(
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			   errmsg("must be superuser to create an operator class")));

	/* Look up the datatype */
	typeoid = typenameTypeId(stmt->datatype);

#ifdef NOT_USED
	/* XXX this is unnecessary given the superuser check above */
	/* Check we have ownership of the datatype */
	if (!pg_type_ownercheck(typeoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TYPE,
					   format_type_be(typeoid));
#endif

	/* Storage datatype is optional */
	storageoid = InvalidOid;

	/*
	 * Create work arrays to hold info about operators and procedures. We
	 * do this mainly so that we can detect duplicate strategy numbers and
	 * support-proc numbers.
	 */
	operators = (Oid *) palloc0(sizeof(Oid) * numOperators);
	procedures = (Oid *) palloc0(sizeof(Oid) * numProcs);
	recheck = (bool *) palloc0(sizeof(bool) * numOperators);

	/*
	 * Scan the "items" list to obtain additional info.
	 */
	foreach(iteml, stmt->items)
	{
		CreateOpClassItem *item = lfirst(iteml);
		Oid			operOid;
		Oid			funcOid;
		AclResult	aclresult;

		Assert(IsA(item, CreateOpClassItem));
		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > numOperators)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, numOperators)));
				if (operators[item->number - 1] != InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					  errmsg("operator number %d appears more than once",
							 item->number)));
				if (item->args != NIL)
				{
					TypeName   *typeName1 = (TypeName *) lfirst(item->args);
					TypeName   *typeName2 = (TypeName *) lsecond(item->args);

					operOid = LookupOperNameTypeNames(item->name,
													  typeName1,
													  typeName2,
													  false);
				}
				else
				{
					/* Default to binary op on input datatype */
					operOid = LookupOperName(item->name, typeoid, typeoid,
											 false);
				}
				/* Caller must have execute permission on operators */
				funcOid = get_opcode(operOid);
				aclresult = pg_proc_aclcheck(funcOid, GetUserId(),
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_PROC,
								   get_func_name(funcOid));
				operators[item->number - 1] = operOid;
				recheck[item->number - 1] = item->recheck;
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > numProcs)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid procedure number %d,"
									" must be between 1 and %d",
									item->number, numProcs)));
				if (procedures[item->number - 1] != InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("procedure number %d appears more than once",
									item->number)));
				funcOid = LookupFuncNameTypeNames(item->name, item->args,
												  false);
				/* Caller must have execute permission on functions */
				aclresult = pg_proc_aclcheck(funcOid, GetUserId(),
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_PROC,
								   get_func_name(funcOid));
				procedures[item->number - 1] = funcOid;
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				if (OidIsValid(storageoid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					   errmsg("storage type specified more than once")));
				storageoid = typenameTypeId(item->storedtype);
				break;
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * If storagetype is specified, make sure it's legal.
	 */
	if (OidIsValid(storageoid))
	{
		/* Just drop the spec if same as column datatype */
		if (storageoid == typeoid)
			storageoid = InvalidOid;
		else
		{
			/*
			 * Currently, only GiST allows storagetype different from
			 * datatype.  This hardcoded test should be eliminated in
			 * favor of adding another boolean column to pg_am ...
			 */
			if (amoid != GIST_AM_OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("storage type may not be different from data type for access method \"%s\"",
								stmt->amname)));
		}
	}

	rel = heap_openr(OperatorClassRelationName, RowExclusiveLock);

	/*
	 * Make sure there is no existing opclass of this name (this is just
	 * to give a more friendly error message than "duplicate key").
	 */
	if (SearchSysCacheExists(CLAAMNAMENSP,
							 ObjectIdGetDatum(amoid),
							 CStringGetDatum(opcname),
							 ObjectIdGetDatum(namespaceoid),
							 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator class \"%s\" for access method \"%s\" already exists",
						opcname, stmt->amname)));

	/*
	 * If we are creating a default opclass, check there isn't one
	 * already.  (Note we do not restrict this test to visible opclasses;
	 * this ensures that typcache.c can find unique solutions to its
	 * questions.)
	 */
	if (stmt->isDefault)
	{
		ScanKeyData skey[1];
		SysScanDesc scan;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   Anum_pg_opclass_opcamid, F_OIDEQ,
							   ObjectIdGetDatum(amoid));

		scan = systable_beginscan(rel, OpclassAmNameNspIndex, true,
								  SnapshotNow, 1, skey);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

			if (opclass->opcintype == typeoid && opclass->opcdefault)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("could not make operator class \"%s\" be default for type %s",
								opcname,
								TypeNameToString(stmt->datatype)),
						 errdetail("Operator class \"%s\" already is the default.",
								   NameStr(opclass->opcname))));
		}

		systable_endscan(scan);
	}

	/*
	 * Okay, let's create the pg_opclass entry.
	 */
	for (i = 0; i < Natts_pg_opclass; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;		/* redundant, but safe */
	}

	i = 0;
	values[i++] = ObjectIdGetDatum(amoid);		/* opcamid */
	namestrcpy(&opcName, opcname);
	values[i++] = NameGetDatum(&opcName);		/* opcname */
	values[i++] = ObjectIdGetDatum(namespaceoid);		/* opcnamespace */
	values[i++] = Int32GetDatum(GetUserId());	/* opcowner */
	values[i++] = ObjectIdGetDatum(typeoid);	/* opcintype */
	values[i++] = BoolGetDatum(stmt->isDefault);		/* opcdefault */
	values[i++] = ObjectIdGetDatum(storageoid); /* opckeytype */

	tup = heap_formtuple(rel->rd_att, values, nulls);

	opclassoid = simple_heap_insert(rel, tup);

	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);

	/*
	 * Now add tuples to pg_amop and pg_amproc tying in the operators and
	 * functions.
	 */
	storeOperators(opclassoid, numOperators, operators, recheck);
	storeProcedures(opclassoid, numProcs, procedures);

	/*
	 * Create dependencies.  Note: we do not create a dependency link to
	 * the AM, because we don't currently support DROP ACCESS METHOD.
	 */
	myself.classId = RelationGetRelid(rel);
	myself.objectId = opclassoid;
	myself.objectSubId = 0;

	/* dependency on namespace */
	referenced.classId = get_system_catalog_relid(NamespaceRelationName);
	referenced.objectId = namespaceoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on indexed datatype */
	referenced.classId = RelOid_pg_type;
	referenced.objectId = typeoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on storage datatype */
	if (OidIsValid(storageoid))
	{
		referenced.classId = RelOid_pg_type;
		referenced.objectId = storageoid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* dependencies on operators */
	referenced.classId = get_system_catalog_relid(OperatorRelationName);
	for (i = 0; i < numOperators; i++)
	{
		if (operators[i] == InvalidOid)
			continue;
		referenced.objectId = operators[i];
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* dependencies on procedures */
	for (i = 0; i < numProcs; i++)
	{
		if (procedures[i] == InvalidOid)
			continue;
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = procedures[i];
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	heap_close(rel, RowExclusiveLock);
}

/*
 * Dump the operators to pg_amop
 */
static void
storeOperators(Oid opclassoid, int numOperators,
			   Oid *operators, bool *recheck)
{
	Relation	rel;
	Datum		values[Natts_pg_amop];
	char		nulls[Natts_pg_amop];
	HeapTuple	tup;
	int			i,
				j;

	rel = heap_openr(AccessMethodOperatorRelationName, RowExclusiveLock);

	for (j = 0; j < numOperators; j++)
	{
		if (operators[j] == InvalidOid)
			continue;

		for (i = 0; i < Natts_pg_amop; ++i)
		{
			nulls[i] = ' ';
			values[i] = (Datum) NULL;
		}

		i = 0;
		values[i++] = ObjectIdGetDatum(opclassoid);		/* amopclaid */
		values[i++] = Int16GetDatum(j + 1);		/* amopstrategy */
		values[i++] = BoolGetDatum(recheck[j]); /* amopreqcheck */
		values[i++] = ObjectIdGetDatum(operators[j]);	/* amopopr */

		tup = heap_formtuple(rel->rd_att, values, nulls);

		simple_heap_insert(rel, tup);

		CatalogUpdateIndexes(rel, tup);

		heap_freetuple(tup);
	}

	heap_close(rel, RowExclusiveLock);
}

/*
 * Dump the procedures (support routines) to pg_amproc
 */
static void
storeProcedures(Oid opclassoid, int numProcs, Oid *procedures)
{
	Relation	rel;
	Datum		values[Natts_pg_amproc];
	char		nulls[Natts_pg_amproc];
	HeapTuple	tup;
	int			i,
				j;

	rel = heap_openr(AccessMethodProcedureRelationName, RowExclusiveLock);

	for (j = 0; j < numProcs; j++)
	{
		if (procedures[j] == InvalidOid)
			continue;

		for (i = 0; i < Natts_pg_amproc; ++i)
		{
			nulls[i] = ' ';
			values[i] = (Datum) NULL;
		}

		i = 0;
		values[i++] = ObjectIdGetDatum(opclassoid);		/* amopclaid */
		values[i++] = Int16GetDatum(j + 1);		/* amprocnum */
		values[i++] = ObjectIdGetDatum(procedures[j]);	/* amproc */

		tup = heap_formtuple(rel->rd_att, values, nulls);

		simple_heap_insert(rel, tup);

		CatalogUpdateIndexes(rel, tup);

		heap_freetuple(tup);
	}

	heap_close(rel, RowExclusiveLock);
}


/*
 * RemoveOpClass
 *		Deletes an opclass.
 */
void
RemoveOpClass(RemoveOpClassStmt *stmt)
{
	Oid			amID,
				opcID;
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	ObjectAddress object;

	/*
	 * Get the access method's OID.
	 */
	amID = GetSysCacheOid(AMNAME,
						  CStringGetDatum(stmt->amname),
						  0, 0, 0);
	if (!OidIsValid(amID))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						stmt->amname)));

	/*
	 * Look up the opclass.
	 */

	/* deconstruct the name list */
	DeconstructQualifiedName(stmt->opclassname, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname);
		tuple = SearchSysCache(CLAAMNAMENSP,
							   ObjectIdGetDatum(amID),
							   PointerGetDatum(opcname),
							   ObjectIdGetDatum(namespaceId),
							   0);
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		opcID = OpclassnameGetOpcid(amID, opcname);
		if (!OidIsValid(opcID))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, stmt->amname)));
		tuple = SearchSysCache(CLAOID,
							   ObjectIdGetDatum(opcID),
							   0, 0, 0);
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
					NameListToString(stmt->opclassname), stmt->amname)));

	opcID = HeapTupleGetOid(tuple);

	/* Permission check: must own opclass or its namespace */
	if (!pg_opclass_ownercheck(opcID, GetUserId()) &&
		!pg_namespace_ownercheck(((Form_pg_opclass) GETSTRUCT(tuple))->opcnamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPCLASS,
					   NameListToString(stmt->opclassname));

	ReleaseSysCache(tuple);

	/*
	 * Do the deletion
	 */
	object.classId = get_system_catalog_relid(OperatorClassRelationName);
	object.objectId = opcID;
	object.objectSubId = 0;

	performDeletion(&object, stmt->behavior);
}

/*
 * Guts of opclass deletion.
 */
void
RemoveOpClassById(Oid opclassOid)
{
	Relation	rel;
	HeapTuple	tup;
	ScanKeyData skey[1];
	SysScanDesc scan;

	/*
	 * First remove the pg_opclass entry itself.
	 */
	rel = heap_openr(OperatorClassRelationName, RowExclusiveLock);

	tup = SearchSysCache(CLAOID,
						 ObjectIdGetDatum(opclassOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for opclass %u", opclassOid);

	simple_heap_delete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(rel, RowExclusiveLock);

	/*
	 * Remove associated entries in pg_amop.
	 */
	ScanKeyEntryInitialize(&skey[0], 0,
						   Anum_pg_amop_amopclaid, F_OIDEQ,
						   ObjectIdGetDatum(opclassOid));

	rel = heap_openr(AccessMethodOperatorRelationName, RowExclusiveLock);

	scan = systable_beginscan(rel, AccessMethodStrategyIndex, true,
							  SnapshotNow, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		simple_heap_delete(rel, &tup->t_self);

	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Remove associated entries in pg_amproc.
	 */
	ScanKeyEntryInitialize(&skey[0], 0,
						   Anum_pg_amproc_amopclaid, F_OIDEQ,
						   ObjectIdGetDatum(opclassOid));

	rel = heap_openr(AccessMethodProcedureRelationName, RowExclusiveLock);

	scan = systable_beginscan(rel, AccessMethodProcedureIndex, true,
							  SnapshotNow, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		simple_heap_delete(rel, &tup->t_self);

	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);
}


/*
 * Rename opclass
 */
void
RenameOpClass(List *name, const char *access_method, const char *newname)
{
	Oid			opcOid;
	Oid			amOid;
	Oid			namespaceOid;
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;

	amOid = GetSysCacheOid(AMNAME,
						   CStringGetDatum(access_method),
						   0, 0, 0);
	if (!OidIsValid(amOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						access_method)));

	rel = heap_openr(OperatorClassRelationName, RowExclusiveLock);

	/*
	 * Look up the opclass
	 */
	DeconstructQualifiedName(name, &schemaname, &opcname);

	if (schemaname)
	{
		namespaceOid = LookupExplicitNamespace(schemaname);

		tup = SearchSysCacheCopy(CLAAMNAMENSP,
								 ObjectIdGetDatum(amOid),
								 PointerGetDatum(opcname),
								 ObjectIdGetDatum(namespaceOid),
								 0);
		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, access_method)));

		opcOid = HeapTupleGetOid(tup);
	}
	else
	{
		opcOid = OpclassnameGetOpcid(amOid, opcname);
		if (!OidIsValid(opcOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, access_method)));

		tup = SearchSysCacheCopy(CLAOID,
								 ObjectIdGetDatum(opcOid),
								 0, 0, 0);
		if (!HeapTupleIsValid(tup))		/* should not happen */
			elog(ERROR, "cache lookup failed for opclass %u", opcOid);

		namespaceOid = ((Form_pg_opclass) GETSTRUCT(tup))->opcnamespace;
	}

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(CLAAMNAMENSP,
							 ObjectIdGetDatum(amOid),
							 CStringGetDatum(newname),
							 ObjectIdGetDatum(namespaceOid),
							 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator class \"%s\" for access method \"%s\" already exists in schema \"%s\"",
						newname, access_method,
						get_namespace_name(namespaceOid))));
	}

	/* must be owner */
	if (!pg_opclass_ownercheck(opcOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPCLASS,
					   NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(((Form_pg_opclass) GETSTRUCT(tup))->opcname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}
