/*-------------------------------------------------------------------------
 *
 * comment.c
 *
 * PostgreSQL object comments utility code.
 *
 * Copyright (c) 1999-2001, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/comment.c,v 1.46 2002/05/13 17:45:30 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_description.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "commands/comment.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "parser/parse.h"
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
		case INDEX:
		case SEQUENCE:
		case TABLE:
		case VIEW:
			CommentRelation(stmt->objtype, stmt->objname, stmt->comment);
			break;
		case COLUMN:
			CommentAttribute(stmt->objname, stmt->comment);
			break;
		case DATABASE:
			CommentDatabase(stmt->objname, stmt->comment);
			break;
		case RULE:
			CommentRule(stmt->objname, stmt->comment);
			break;
		case TYPE_P:
			CommentType(stmt->objname, stmt->comment);
			break;
		case AGGREGATE:
			CommentAggregate(stmt->objname, stmt->objargs, stmt->comment);
			break;
		case FUNCTION:
			CommentProc(stmt->objname, stmt->objargs, stmt->comment);
			break;
		case OPERATOR:
			CommentOperator(stmt->objname, stmt->objargs, stmt->comment);
			break;
		case TRIGGER:
			CommentTrigger(stmt->objname, stmt->comment);
			break;
		case SCHEMA:
			CommentNamespace(stmt->objname, stmt->comment);
			break;
		default:
			elog(ERROR, "An attempt was made to comment on a unknown type: %d",
				 stmt->objtype);
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
	Relation	descriptionindex;
	ScanKeyData skey[3];
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	HeapTupleData oldtuple;
	Buffer		buffer;
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

	/* Open pg_description and its index */

	description = heap_openr(DescriptionRelationName, RowExclusiveLock);
	descriptionindex = index_openr(DescriptionObjIndex);

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

	sd = index_beginscan(descriptionindex, false, 3, skey);

	oldtuple.t_datamcxt = CurrentMemoryContext;
	oldtuple.t_data = NULL;

	while ((indexRes = index_getnext(sd, ForwardScanDirection)))
	{
		oldtuple.t_self = indexRes->heap_iptr;
		heap_fetch(description, SnapshotNow, &oldtuple, &buffer, sd);
		pfree(indexRes);

		if (oldtuple.t_data == NULL)
			continue;			/* time qual failed */

		/* Found the old tuple, so delete or update it */

		if (comment == NULL)
			simple_heap_delete(description, &oldtuple.t_self);
		else
		{
			newtuple = heap_modifytuple(&oldtuple, description, values,
										nulls, replaces);
			simple_heap_update(description, &oldtuple.t_self, newtuple);
		}

		ReleaseBuffer(buffer);
		break;					/* Assume there can be only one match */
	}

	index_endscan(sd);

	/* If we didn't find an old tuple, insert a new one */

	if (oldtuple.t_data == NULL && comment != NULL)
	{
		newtuple = heap_formtuple(RelationGetDescr(description),
								  values, nulls);
		heap_insert(description, newtuple);
	}

	/* Update indexes, if necessary */

	if (newtuple != NULL)
	{
		if (RelationGetForm(description)->relhasindex)
		{
			Relation	idescs[Num_pg_description_indices];

			CatalogOpenIndices(Num_pg_description_indices,
							   Name_pg_description_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_description_indices, description,
							   newtuple);
			CatalogCloseIndices(Num_pg_description_indices, idescs);
		}
		heap_freetuple(newtuple);
	}

	/* Done */

	index_close(descriptionindex);
	heap_close(description, NoLock);
}

/*
 * DeleteComments --
 *
 * This routine is used to purge all comments associated with an object,
 * regardless of their objsubid.  It is called, for example, when a relation
 * is destroyed.
 */
void
DeleteComments(Oid oid, Oid classoid)
{
	Relation	description;
	Relation	descriptionindex;
	ScanKeyData skey[2];
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	HeapTupleData oldtuple;
	Buffer		buffer;

	/* Open pg_description and its index */

	description = heap_openr(DescriptionRelationName, RowExclusiveLock);
	descriptionindex = index_openr(DescriptionObjIndex);

	/* Use the index to search for all matching old tuples */

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

	sd = index_beginscan(descriptionindex, false, 2, skey);

	while ((indexRes = index_getnext(sd, ForwardScanDirection)))
	{
		oldtuple.t_self = indexRes->heap_iptr;
		heap_fetch(description, SnapshotNow, &oldtuple, &buffer, sd);
		pfree(indexRes);

		if (oldtuple.t_data == NULL)
			continue;			/* time qual failed */

		simple_heap_delete(description, &oldtuple.t_self);

		ReleaseBuffer(buffer);
	}

	/* Done */

	index_endscan(sd);
	index_close(descriptionindex);
	heap_close(description, NoLock);
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
		aclcheck_error(ACLCHECK_NOT_OWNER, RelationGetRelationName(relation));

	/* Next, verify that the relation type matches the intent */

	switch (objtype)
	{
		case INDEX:
			if (relation->rd_rel->relkind != RELKIND_INDEX)
				elog(ERROR, "relation \"%s\" is not an index",
					 RelationGetRelationName(relation));
			break;
		case TABLE:
			if (relation->rd_rel->relkind != RELKIND_RELATION)
				elog(ERROR, "relation \"%s\" is not a table",
					 RelationGetRelationName(relation));
			break;
		case VIEW:
			if (relation->rd_rel->relkind != RELKIND_VIEW)
				elog(ERROR, "relation \"%s\" is not a view",
					 RelationGetRelationName(relation));
			break;
		case SEQUENCE:
			if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
				elog(ERROR, "relation \"%s\" is not a sequence",
					 RelationGetRelationName(relation));
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
 * elog() exception is thrown.	The parameters are the relation
 * and attribute names, and the comments
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
	if (nnames < 2)
		elog(ERROR, "CommentAttribute: must specify relation.attribute");
	relname = ltruncate(nnames-1, listCopy(qualname));
	attrname = strVal(nth(nnames-1, qualname));

	/* Open the containing relation to ensure it won't go away meanwhile */
	rel = makeRangeVarFromNameList(relname);
	relation = heap_openrv(rel, AccessShareLock);

	/* Check object security */

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, RelationGetRelationName(relation));

	/* Now, fetch the attribute number from the system cache */

	attnum = get_attnum(RelationGetRelid(relation), attrname);
	if (attnum == InvalidAttrNumber)
		elog(ERROR, "\"%s\" is not an attribute of class \"%s\"",
			 attrname, RelationGetRelationName(relation));

	/* Create the comment using the relation's oid */

	CreateComments(RelationGetRelid(relation), RelOid_pg_class,
				   (int32) attnum, comment);

	/* Done, but hold lock until commit */

	heap_close(relation, NoLock);
}

/*
 * CommentDatabase --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding the specified database. The routine will check
 * security for owner permissions, and, if succesful, will then
 * attempt to find the oid of the database specified. Once found,
 * a comment is added/dropped using the CreateComments() routine.
 */
static void
CommentDatabase(List *qualname, char *comment)
{
	char	   *database;
	Relation	pg_database;
	ScanKeyData entry;
	HeapScanDesc scan;
	HeapTuple	dbtuple;
	Oid			oid;

	if (length(qualname) != 1)
		elog(ERROR, "CommentDatabase: database name may not be qualified");
	database = strVal(lfirst(qualname));

	/* Only allow comments on the current database */
	if (strcmp(database, DatabaseName) != 0)
		elog(ERROR, "Database comments may only be applied to the current database");

	/* First find the tuple in pg_database for the database */

	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_database_datname,
						   F_NAMEEQ, CStringGetDatum(database));
	scan = heap_beginscan(pg_database, 0, SnapshotNow, 1, &entry);
	dbtuple = heap_getnext(scan, 0);

	/* Validate database exists, and fetch the db oid */

	if (!HeapTupleIsValid(dbtuple))
		elog(ERROR, "database \"%s\" does not exist", database);
	oid = dbtuple->t_data->t_oid;

	/* Allow if the user matches the database dba or is a superuser */

	if (!(superuser() || is_dbadmin(oid)))
		elog(ERROR, "you are not permitted to comment on database \"%s\"",
			 database);

	/* Create the comments with the pg_database oid */

	CreateComments(oid, RelOid_pg_database, 0, comment);

	/* Complete the scan and close any opened relations */

	heap_endscan(scan);
	heap_close(pg_database, AccessShareLock);
}

/*
 * CommentNamespace --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding the specified namespace. The routine will check
 * security for owner permissions, and, if succesful, will then
 * attempt to find the oid of the namespace specified. Once found,
 * a comment is added/dropped using the CreateComments() routine.
 */
static void
CommentNamespace(List *qualname, char *comment)
{
	Oid			oid;
	Oid			classoid;
	HeapTuple	tp;
	char	   *namespace;

	if (length(qualname) != 1)
		elog(ERROR, "CommentSchema: schema name may not be qualified");
	namespace = strVal(lfirst(qualname));

	tp = SearchSysCache(NAMESPACENAME,
						CStringGetDatum(namespace),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "CommentSchema: Schema \"%s\" could not be found",
			 namespace);

	oid = tp->t_data->t_oid;

	/* Check object security */
	if (!pg_namespace_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, namespace);

	/* pg_namespace doesn't have a hard-coded OID, so must look it up */
	classoid = get_relname_relid(NamespaceRelationName, PG_CATALOG_NAMESPACE);
	Assert(OidIsValid(classoid));

	/* Call CreateComments() to create/drop the comments */
	CreateComments(oid, classoid, 0, comment);

	/* Cleanup */
	ReleaseSysCache(tp);
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
		scanDesc = heap_beginscan(RewriteRelation,
								  0, SnapshotNow, 1, &scanKeyData);

		tuple = heap_getnext(scanDesc, 0);
		if (HeapTupleIsValid(tuple))
		{
			reloid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
			ruleoid = tuple->t_data->t_oid;
		}
		else
		{
			elog(ERROR, "rule \"%s\" does not exist", rulename);
			reloid = ruleoid = 0; /* keep compiler quiet */
		}

		if (HeapTupleIsValid(tuple = heap_getnext(scanDesc, 0)))
			elog(ERROR, "There are multiple rules \"%s\""
				 "\n\tPlease specify a relation name as well as a rule name",
				 rulename);

		heap_endscan(scanDesc);
		heap_close(RewriteRelation, AccessShareLock);

		/* Open the owning relation to ensure it won't go away meanwhile */
		relation = heap_open(reloid, AccessShareLock);
	}
	else
	{
		/* New-style: rule and relname both provided */
		Assert(nnames >= 2);
		relname = ltruncate(nnames-1, listCopy(qualname));
		rulename = strVal(nth(nnames-1, qualname));

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
			elog(ERROR, "rule \"%s\" does not exist", rulename);
		Assert(reloid == ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class);
		ruleoid = tuple->t_data->t_oid;
		ReleaseSysCache(tuple);
	}

	/* Check object security */

	aclcheck = pg_class_aclcheck(reloid, GetUserId(), ACL_RULE);
	if (aclcheck != ACLCHECK_OK)
		aclcheck_error(aclcheck, rulename);

	/* pg_rewrite doesn't have a hard-coded OID, so must look it up */
	classoid = get_relname_relid(RewriteRelationName, PG_CATALOG_NAMESPACE);
	Assert(OidIsValid(classoid));

	/* Call CreateComments() to create/drop the comments */

	CreateComments(ruleoid, classoid, 0, comment);
}

/*
 * CommentType --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding a TYPE. The type is specified by name
 * and, if found, and the user has appropriate permissions, a
 * comment will be added/dropped using the CreateComments() routine.
 * The type's name and the comments are the paramters to this routine.
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
		aclcheck_error(ACLCHECK_NOT_OWNER, TypeNameToString(tname));

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
		baseoid = InvalidOid;

	/* Now, attempt to find the actual tuple in pg_proc */

	oid = find_aggregate_func("CommentAggregate", aggregate, baseoid);

	/* Next, validate the user's attempt to comment */

	if (!pg_proc_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, NameListToString(aggregate));

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

	oid = LookupFuncNameTypeNames(function, arguments,
								  true, "CommentProc");

	/* Now, validate the user's ability to comment on this function */

	if (!pg_proc_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, NameListToString(function));

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
	oid = LookupOperNameTypeNames(opername, typenode1, typenode2,
								  "CommentOperator");

	/* Valid user's ability to comment on this operator */
	if (!pg_oper_ownercheck(oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, NameListToString(opername));

	/* pg_operator doesn't have a hard-coded OID, so must look it up */
	classoid = get_relname_relid(OperatorRelationName, PG_CATALOG_NAMESPACE);
	Assert(OidIsValid(classoid));

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
	SysScanDesc	scan;
	ScanKeyData entry[2];
	Oid			oid;

	/* Separate relname and trig name */
	nnames = length(qualname);
	if (nnames < 2)
		elog(ERROR, "CommentTrigger: must specify relation and trigger");
	relname = ltruncate(nnames-1, listCopy(qualname));
	trigname = strVal(nth(nnames-1, qualname));

	/* Open the owning relation to ensure it won't go away meanwhile */
	rel = makeRangeVarFromNameList(relname);
	relation = heap_openrv(rel, AccessShareLock);

	/* Check object security */

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, RelationGetRelationName(relation));

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
		elog(ERROR, "trigger \"%s\" for relation \"%s\" does not exist",
			 trigname, RelationGetRelationName(relation));

	oid = triggertuple->t_data->t_oid;

	systable_endscan(scan);

	/* Create the comments with the pg_trigger oid */

	CreateComments(oid, RelationGetRelid(pg_trigger), 0, comment);

	/* Done, but hold lock on relation */

	heap_close(pg_trigger, AccessShareLock);
	heap_close(relation, NoLock);
}
