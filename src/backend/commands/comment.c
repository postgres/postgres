/*-------------------------------------------------------------------------
 *
 * comment.c
 *
 * PostgreSQL object comments utility code.
 *
 * Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/comment.c,v 1.71 2003/09/25 06:57:58 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_description.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * Static Function Prototypes --
 *
 * The following protoypes are declared static so as not to conflict
 * with any other routines outside this module. These routines are
 * called by the public function CommentObject() routine to create
 * the appropriate comment for the specific object type.
 */

static void CommentRelation(int objtype, List *relname, char *comment);
static void CommentAttribute(List *qualname, char *comment);
static void CommentDatabase(List *qualname, char *comment);
static void CommentNamespace(List *qualname, char *comment);
static void CommentRule(List *qualname, char *comment);
static void CommentType(List *typename, char *comment);
static void CommentAggregate(List *aggregate, List *arguments, char *comment);
static void CommentProc(List *function, List *arguments, char *comment);
static void CommentOperator(List *opername, List *arguments, char *comment);
static void CommentTrigger(List *qualname, char *comment);
static void CommentConstraint(List *qualname, char *comment);


/*
 * CommentObject --
 *
 * This routine is used to add the associated comment into
 * pg_description for the object specified by the given SQL command.
 */
void
CommentObject(CommentStmt *stmt)
{
	switch (stmt->objtype)
	{
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
			CommentRelation(stmt->objtype, stmt->objname, stmt->comment);
			break;
		case OBJECT_COLUMN:
			CommentAttribute(stmt->objname, stmt->comment);
			break;
		case OBJECT_DATABASE:
			CommentDatabase(stmt->objname, stmt->comment);
			break;
		case OBJECT_RULE:
			CommentRule(stmt->objname, stmt->comment);
			break;
		case OBJECT_TYPE:
			CommentType(stmt->objname, stmt->comment);
			break;
		case OBJECT_AGGREGATE:
			CommentAggregate(stmt->objname, stmt->objargs, stmt->comment);
			break;
		case OBJECT_FUNCTION:
			CommentProc(stmt->objname, stmt->objargs, stmt->comment);
			break;
		case OBJECT_OPERATOR:
			CommentOperator(stmt->objname, stmt->objargs, stmt->comment);
			break;
		case OBJECT_TRIGGER:
			CommentTrigger(stmt->objname, stmt->comment);
			break;
		case OBJECT_SCHEMA:
			CommentNamespace(stmt->objname, stmt->comment);
			break;
		case OBJECT_CONSTRAINT:
			CommentConstraint(stmt->objname, stmt->comment);
			break;
		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) stmt->objtype);
	}
}

/*
 * CreateComments --
 *
 * Create a comment for the specified object descriptor.  Inserts a new
 * pg_description tuple, or replaces an existing one with the same key.
 *
 * If the comment given is null or an empty string, instead delete any
 * existing comment for the specified key.
 */
void
CreateComments(Oid oid, Oid classoid, int32 subid, char *comment)
{
	Relation	description;
	ScanKeyData skey[3];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	HeapTuple	newtuple = NULL;
	Datum		values[Natts_pg_description];
	char		nulls[Natts_pg_description];
	char		replaces[Natts_pg_description];
	int			i;

	/* Reduce empty-string to NULL case */
	if (comment != NULL && strlen(comment) == 0)
		comment = NULL;

	/* Prepare to form or update a tuple, if necessary */
	if (comment != NULL)
	{
		for (i = 0; i < Natts_pg_description; i++)
		{
			nulls[i] = ' ';
			replaces[i] = 'r';
		}
		i = 0;
		values[i++] = ObjectIdGetDatum(oid);
		values[i++] = ObjectIdGetDatum(classoid);
		values[i++] = Int32GetDatum(subid);
		values[i++] = DirectFunctionCall1(textin, CStringGetDatum(comment));
	}

	/* Use the index to search for a matching old tuple */

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(oid));

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(classoid));

	ScanKeyEntryInitialize(&skey[2],
						   (bits16) 0x0,
						   (AttrNumber) 3,
						   (RegProcedure) F_INT4EQ,
						   Int32GetDatum(subid));

	description = heap_openr(DescriptionRelationName, RowExclusiveLock);

	sd = systable_beginscan(description, DescriptionObjIndex, true,
							SnapshotNow, 3, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
	{
		/* Found the old tuple, so delete or update it */

		if (comment == NULL)
			simple_heap_delete(description, &oldtuple->t_self);
		else
		{
			newtuple = heap_modifytuple(oldtuple, description, values,
										nulls, replaces);
			simple_heap_update(description, &oldtuple->t_self, newtuple);
		}

		break;					/* Assume there can be only one match */
	}

	systable_endscan(sd);

	/* If we didn't find an old tuple, insert a new one */

	if (newtuple == NULL && comment != NULL)
	{
		newtuple = heap_formtuple(RelationGetDescr(description),
								  values, nulls);
		simple_heap_insert(description, newtuple);
	}

	/* Update indexes, if necessary */
	if (newtuple != NULL)
	{
		CatalogUpdateIndexes(description, newtuple);
		heap_freetuple(newtuple);
	}

	/* Done */

	heap_close(description, NoLock);
}

/*
 * DeleteComments -- remove comments for an object
 *
 * If subid is nonzero then only comments matching it will be removed.
 * If subid is zero, all comments matching the oid/classoid will be removed
 * (this corresponds to deleting a whole object).
 */
void
DeleteComments(Oid oid, Oid classoid, int32 subid)
{
	Relation	description;
	ScanKeyData skey[3];
	int			nkeys;
	SysScanDesc sd;
	HeapTuple	oldtuple;

	/* Use the index to search for all matching old tuples */

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   Anum_pg_description_objoid, F_OIDEQ,
						   ObjectIdGetDatum(oid));

	ScanKeyEntryInitialize(&skey[1], 0x0,
						   Anum_pg_description_classoid, F_OIDEQ,
						   ObjectIdGetDatum(classoid));

	if (subid != 0)
	{
		ScanKeyEntryInitialize(&skey[2], 0x0,
							   Anum_pg_description_objsubid, F_INT4EQ,
							   Int32GetDatum(subid));
		nkeys = 3;
	}
	else
		nkeys = 2;

	description = heap_openr(DescriptionRelationName, RowExclusiveLock);

	sd = systable_beginscan(description, DescriptionObjIndex, true,
							SnapshotNow, nkeys, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
		simple_heap_delete(description, &oldtuple->t_self);

	/* Done */

	systable_endscan(sd);
	heap_close(description, RowExclusiveLock);
}

/*
 * CommentRelation --
 *
 * This routine is used to add/drop a comment from a relation, where
 * a relation is a TABLE, SEQUENCE, VIEW or INDEX. The routine simply
 * finds the relation name by searching the system cache, locating
 * the appropriate tuple, and inserting a comment using that
 * tuple's oid. Its parameters are the relation name and comments.
 */
static void
CommentRelation(int objtype, List *relname, char *comment)
{
	Relation	relation;
	RangeVar   *tgtrel;

	tgtrel = makeRangeVarFromNameList(relname);

	/*
	 * Open the relation.  We do this mainly to acquire a lock that
	 * ensures no one else drops the relation before we commit.  (If they
	 * did, they'd fail to remove the entry we are about to make in
	 * pg_description.)
	 */
	relation = relation_openrv(tgtrel, AccessShareLock);

	/* Check object security */
	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(relation));

	/* Next, verify that the relation type matches the intent */

	switch (objtype)
	{
		case OBJECT_INDEX:
			if (relation->rd_rel->relkind != RELKIND_INDEX)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not an index",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_SEQUENCE:
			if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a sequence",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_TABLE:
			if (relation->rd_rel->relkind != RELKIND_RELATION)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_VIEW:
			if (relation->rd_rel->relkind != RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a view",
								RelationGetRelationName(relation))));
			break;
	}

	/* Create the comment using the relation's oid */

	CreateComments(RelationGetRelid(relation), RelOid_pg_class, 0, comment);

	/* Done, but hold lock until commit */
	relation_close(relation, NoLock);
}

/*
 * CommentAttribute --
 *
 * This routine is used to add/drop a comment from an attribute
 * such as a table's column. The routine will check security
 * restrictions and then attempt to look up the specified
 * attribute. If successful, a comment is added/dropped, else an
 * ereport() exception is thrown.	The parameters are the relation
 * and attribute names, and the comment
 */
static void
CommentAttribute(List *qualname, char *comment)
{
	int			nnames;
	List	   *relname;
	char	   *attrname;
	RangeVar   *rel;
	Relation	relation;
	AttrNumber	attnum;

	/* Separate relname and attr name */
	nnames = length(qualname);
	if (nnames < 2)				/* parser messed up */
		elog(ERROR, "must specify relation and attribute");
	relname = ltruncate(nnames - 1, listCopy(qualname));
	attrname = strVal(llast(qualname));

	/* Open the containing relation to ensure it won't go away meanwhile */
	rel = makeRangeVarFromNameList(relname);
	relation = relation_openrv(rel, AccessShareLock);

	/* Check object security */

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(relation));

	/* Now, fetch the attribute number from the system cache */

	attnum = get_attnum(RelationGetRelid(relation), attrname);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
			 errmsg("column \"%s\" of relation \"%s\" does not exist",
					attrname, RelationGetRelationName(relation))));

	/* Create the comment using the relation's oid */

	CreateComments(RelationGetRelid(relation), RelOid_pg_class,
				   (int32) attnum, comment);

	/* Done, but hold lock until commit */

	relation_close(relation, NoLock);
}

/*
 * CommentDatabase --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding the specified database. The routine will check
 * security for owner permissions, and, if successful, will then
 * attempt to find the oid of the database specified. Once found,
 * a comment is added/dropped using the CreateComments() routine.
 */
static void
CommentDatabase(List *qualname, char *comment)
{
	char	   *database;
	Oid			oid;

	if (length(qualname) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("database name may not be qualified")));
	database = strVal(lfirst(qualname));

	/*
	 * We cannot currently support cross-database comments (since other
	 * DBs cannot see pg_description of this database).  So, we reject
	 * attempts to comment on a database other than the current one.
	 * Someday this might be improved, but it would take a redesigned
	 * infrastructure.
	 *
	 * When loading a dump, we may see a COMMENT ON DATABASE for the old name
	 * of the database.  Erroring out would prevent pg_restore from
	 * completing (which is really pg_restore's fault, but for now we will
	 * work around the problem here).  Consensus is that the best fix is
	 * to treat wrong database name as a WARNING not an ERROR.
	 */

	/* First get the database OID */
	oid = get_database_oid(database);
	if (!OidIsValid(oid))
	{
		ereport(WARNING,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", database)));
		return;
	}

	/* Only allow comments on the current database */
	if (oid != MyDatabaseId)
	{
		ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("database comments may only be applied to the current database")));
		return;
	}

	/* Check object security */
	if (!pg_database_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   database);

	/* Create the comment with the pg_database oid */
	CreateComments(oid, RelOid_pg_database, 0, comment);
}

/*
 * CommentNamespace --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding the specified namespace. The routine will check
 * security for owner permissions, and, if successful, will then
 * attempt to find the oid of the namespace specified. Once found,
 * a comment is added/dropped using the CreateComments() routine.
 */
static void
CommentNamespace(List *qualname, char *comment)
{
	Oid			oid;
	Oid			classoid;
	char	   *namespace;

	if (length(qualname) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("schema name may not be qualified")));
	namespace = strVal(lfirst(qualname));

	oid = GetSysCacheOid(NAMESPACENAME,
						 CStringGetDatum(namespace),
						 0, 0, 0);
	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", namespace)));

	/* Check object security */
	if (!pg_namespace_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_NAMESPACE,
					   namespace);

	/* pg_namespace doesn't have a hard-coded OID, so must look it up */
	classoid = get_system_catalog_relid(NamespaceRelationName);

	/* Call CreateComments() to create/drop the comments */
	CreateComments(oid, classoid, 0, comment);
}

/*
 * CommentRule --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding a specified RULE. The rule for commenting is determined by
 * both its name and the relation to which it refers. The arguments to this
 * function are the rule name and relation name (merged into a qualified
 * name), and the comment to add/drop.
 *
 * Before PG 7.3, rules had unique names across the whole database, and so
 * the syntax was just COMMENT ON RULE rulename, with no relation name.
 * For purposes of backwards compatibility, we support that as long as there
 * is only one rule by the specified name in the database.
 */
static void
CommentRule(List *qualname, char *comment)
{
	int			nnames;
	List	   *relname;
	char	   *rulename;
	RangeVar   *rel;
	Relation	relation;
	HeapTuple	tuple;
	Oid			reloid;
	Oid			ruleoid;
	Oid			classoid;
	AclResult	aclcheck;

	/* Separate relname and trig name */
	nnames = length(qualname);
	if (nnames == 1)
	{
		/* Old-style: only a rule name is given */
		Relation	RewriteRelation;
		HeapScanDesc scanDesc;
		ScanKeyData scanKeyData;

		rulename = strVal(lfirst(qualname));

		/* Search pg_rewrite for such a rule */
		ScanKeyEntryInitialize(&scanKeyData,
							   0,
							   Anum_pg_rewrite_rulename,
							   F_NAMEEQ,
							   PointerGetDatum(rulename));

		RewriteRelation = heap_openr(RewriteRelationName, AccessShareLock);
		scanDesc = heap_beginscan(RewriteRelation, SnapshotNow,
								  1, &scanKeyData);

		tuple = heap_getnext(scanDesc, ForwardScanDirection);
		if (HeapTupleIsValid(tuple))
		{
			reloid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
			ruleoid = HeapTupleGetOid(tuple);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("rule \"%s\" does not exist", rulename)));
			reloid = ruleoid = 0;		/* keep compiler quiet */
		}

		if (HeapTupleIsValid(tuple = heap_getnext(scanDesc,
												  ForwardScanDirection)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("there are multiple rules named \"%s\"", rulename),
			errhint("Specify a relation name as well as a rule name.")));

		heap_endscan(scanDesc);
		heap_close(RewriteRelation, AccessShareLock);

		/* Open the owning relation to ensure it won't go away meanwhile */
		relation = heap_open(reloid, AccessShareLock);
	}
	else
	{
		/* New-style: rule and relname both provided */
		Assert(nnames >= 2);
		relname = ltruncate(nnames - 1, listCopy(qualname));
		rulename = strVal(llast(qualname));

		/* Open the owning relation to ensure it won't go away meanwhile */
		rel = makeRangeVarFromNameList(relname);
		relation = heap_openrv(rel, AccessShareLock);
		reloid = RelationGetRelid(relation);

		/* Find the rule's pg_rewrite tuple, get its OID */
		tuple = SearchSysCache(RULERELNAME,
							   ObjectIdGetDatum(reloid),
							   PointerGetDatum(rulename),
							   0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for rule \"%s\"", rulename);
		Assert(reloid == ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class);
		ruleoid = HeapTupleGetOid(tuple);
		ReleaseSysCache(tuple);
	}

	/* Check object security */
	aclcheck = pg_class_aclcheck(reloid, GetUserId(), ACL_RULE);
	if (aclcheck != ACLCHECK_OK)
		aclcheck_error(aclcheck, ACL_KIND_CLASS,
					   get_rel_name(reloid));

	/* pg_rewrite doesn't have a hard-coded OID, so must look it up */
	classoid = get_system_catalog_relid(RewriteRelationName);

	/* Call CreateComments() to create/drop the comments */
	CreateComments(ruleoid, classoid, 0, comment);

	heap_close(relation, NoLock);
}

/*
 * CommentType --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding a TYPE. The type is specified by name
 * and, if found, and the user has appropriate permissions, a
 * comment will be added/dropped using the CreateComments() routine.
 * The type's name and the comments are the parameters to this routine.
 */
static void
CommentType(List *typename, char *comment)
{
	TypeName   *tname;
	Oid			oid;

	/* XXX a bit of a crock; should accept TypeName in COMMENT syntax */
	tname = makeNode(TypeName);
	tname->names = typename;
	tname->typmod = -1;

	/* Find the type's oid */

	oid = typenameTypeId(tname);

	/* Check object security */

	if (!pg_type_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TYPE,
					   TypeNameToString(tname));

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, RelOid_pg_type, 0, comment);
}

/*
 * CommentAggregate --
 *
 * This routine is used to allow a user to provide comments on an
 * aggregate function. The aggregate function is determined by both
 * its name and its argument type, which, with the comments are
 * the three parameters handed to this routine.
 */
static void
CommentAggregate(List *aggregate, List *arguments, char *comment)
{
	TypeName   *aggtype = (TypeName *) lfirst(arguments);
	Oid			baseoid,
				oid;

	/* First, attempt to determine the base aggregate oid */
	if (aggtype)
		baseoid = typenameTypeId(aggtype);
	else
		baseoid = ANYOID;

	/* Now, attempt to find the actual tuple in pg_proc */

	oid = find_aggregate_func(aggregate, baseoid, false);

	/* Next, validate the user's attempt to comment */

	if (!pg_proc_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(aggregate));

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, RelOid_pg_proc, 0, comment);
}

/*
 * CommentProc --
 *
 * This routine is used to allow a user to provide comments on an
 * procedure (function). The procedure is determined by both
 * its name and its argument list. The argument list is expected to
 * be a series of parsed nodes pointed to by a List object. If the
 * comments string is empty, the associated comment is dropped.
 */
static void
CommentProc(List *function, List *arguments, char *comment)
{
	Oid			oid;

	/* Look up the procedure */

	oid = LookupFuncNameTypeNames(function, arguments, false);

	/* Now, validate the user's ability to comment on this function */

	if (!pg_proc_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(function));

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, RelOid_pg_proc, 0, comment);
}

/*
 * CommentOperator --
 *
 * This routine is used to allow a user to provide comments on an
 * operator. The operator for commenting is determined by both
 * its name and its argument list which defines the left and right
 * hand types the operator will operate on. The argument list is
 * expected to be a couple of parse nodes pointed to be a List
 * object.
 */
static void
CommentOperator(List *opername, List *arguments, char *comment)
{
	TypeName   *typenode1 = (TypeName *) lfirst(arguments);
	TypeName   *typenode2 = (TypeName *) lsecond(arguments);
	Oid			oid;
	Oid			classoid;

	/* Look up the operator */
	oid = LookupOperNameTypeNames(opername, typenode1, typenode2, false);

	/* Valid user's ability to comment on this operator */
	if (!pg_oper_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPER,
					   NameListToString(opername));

	/* pg_operator doesn't have a hard-coded OID, so must look it up */
	classoid = get_system_catalog_relid(OperatorRelationName);

	/* Call CreateComments() to create/drop the comments */
	CreateComments(oid, classoid, 0, comment);
}

/*
 * CommentTrigger --
 *
 * This routine is used to allow a user to provide comments on a
 * trigger event. The trigger for commenting is determined by both
 * its name and the relation to which it refers. The arguments to this
 * function are the trigger name and relation name (merged into a qualified
 * name), and the comment to add/drop.
 */
static void
CommentTrigger(List *qualname, char *comment)
{
	int			nnames;
	List	   *relname;
	char	   *trigname;
	RangeVar   *rel;
	Relation	pg_trigger,
				relation;
	HeapTuple	triggertuple;
	SysScanDesc scan;
	ScanKeyData entry[2];
	Oid			oid;

	/* Separate relname and trig name */
	nnames = length(qualname);
	if (nnames < 2)				/* parser messed up */
		elog(ERROR, "must specify relation and trigger");
	relname = ltruncate(nnames - 1, listCopy(qualname));
	trigname = strVal(llast(qualname));

	/* Open the owning relation to ensure it won't go away meanwhile */
	rel = makeRangeVarFromNameList(relname);
	relation = heap_openrv(rel, AccessShareLock);

	/* Check object security */

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(relation));

	/*
	 * Fetch the trigger tuple from pg_trigger.  There can be only one
	 * because of the unique index.
	 */
	pg_trigger = heap_openr(TriggerRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   Anum_pg_trigger_tgrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));
	ScanKeyEntryInitialize(&entry[1], 0x0,
						   Anum_pg_trigger_tgname,
						   F_NAMEEQ,
						   CStringGetDatum(trigname));
	scan = systable_beginscan(pg_trigger, TriggerRelidNameIndex, true,
							  SnapshotNow, 2, entry);
	triggertuple = systable_getnext(scan);

	/* If no trigger exists for the relation specified, notify user */

	if (!HeapTupleIsValid(triggertuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			  errmsg("trigger \"%s\" for table \"%s\" does not exist",
					 trigname, RelationGetRelationName(relation))));

	oid = HeapTupleGetOid(triggertuple);

	systable_endscan(scan);

	/* Create the comment with the pg_trigger oid */

	CreateComments(oid, RelationGetRelid(pg_trigger), 0, comment);

	/* Done, but hold lock on relation */

	heap_close(pg_trigger, AccessShareLock);
	heap_close(relation, NoLock);
}


/*
 * CommentConstraint --
 *
 * Enable commenting on constraints held within the pg_constraint
 * table.  A qualified name is required as constraint names are
 * unique per relation.
 */
static void
CommentConstraint(List *qualname, char *comment)
{
	int			nnames;
	List	   *relName;
	char	   *conName;
	RangeVar   *rel;
	Relation	pg_constraint,
				relation;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];
	Oid			conOid = InvalidOid;

	/* Separate relname and constraint name */
	nnames = length(qualname);
	if (nnames < 2)				/* parser messed up */
		elog(ERROR, "must specify relation and constraint");
	relName = ltruncate(nnames - 1, listCopy(qualname));
	conName = strVal(llast(qualname));

	/* Open the owning relation to ensure it won't go away meanwhile */
	rel = makeRangeVarFromNameList(relName);
	relation = heap_openrv(rel, AccessShareLock);

	/* Check object security */

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(relation));

	/*
	 * Fetch the constraint tuple from pg_constraint.  There may be more
	 * than one match, because constraints are not required to have unique
	 * names; if so, error out.
	 */
	pg_constraint = heap_openr(ConstraintRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   Anum_pg_constraint_conrelid, F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	scan = systable_beginscan(pg_constraint, ConstraintRelidIndex, true,
							  SnapshotNow, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (strcmp(NameStr(con->conname), conName) == 0)
		{
			if (OidIsValid(conOid))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("table \"%s\" has multiple constraints named \"%s\"",
						   RelationGetRelationName(relation), conName)));
			conOid = HeapTupleGetOid(tuple);
		}
	}

	systable_endscan(scan);

	/* If no constraint exists for the relation specified, notify user */
	if (!OidIsValid(conOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
		   errmsg("constraint \"%s\" for table \"%s\" does not exist",
				  conName, RelationGetRelationName(relation))));

	/* Create the comment with the pg_constraint oid */
	CreateComments(conOid, RelationGetRelid(pg_constraint), 0, comment);

	/* Done, but hold lock on relation */
	heap_close(pg_constraint, AccessShareLock);
	heap_close(relation, NoLock);
}
