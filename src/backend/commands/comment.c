/*-------------------------------------------------------------------------
 *
 * comment.c
 *
 * PostgreSQL object comments utility code.
 *
 * Copyright (c) 1999-2001, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/comment.c,v 1.38 2002/03/29 19:06:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_database.h"
#include "catalog/pg_description.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_agg.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "parser/parse.h"
#include "rewrite/rewriteRemove.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*------------------------------------------------------------------
 * Static Function Prototypes --
 *
 * The following protoypes are declared static so as not to conflict
 * with any other routines outside this module. These routines are
 * called by the public function CommentObject() routine to create
 * the appropriate comment for the specific object type.
 *------------------------------------------------------------------
 */

static void CommentRelation(int objtype, char * schemaname, char *relation,
							char *comment);
static void CommentAttribute(char * schemaname, char *relation,
							 char *attrib, char *comment);
static void CommentDatabase(char *database, char *comment);
static void CommentRewrite(char *rule, char *comment);
static void CommentType(char *type, char *comment);
static void CommentAggregate(char *aggregate, List *arguments, char *comment);
static void CommentProc(char *function, List *arguments, char *comment);
static void CommentOperator(char *opname, List *arguments, char *comment);
static void CommentTrigger(char *trigger, char *schemaname, char *relation,
						   char *comments);


/*------------------------------------------------------------------
 * CommentObject --
 *
 * This routine is used to add the associated comment into
 * pg_description for the object specified by the paramters handed
 * to this routine. If the routine cannot determine an Oid to
 * associated with the parameters handed to this routine, an
 * error is thrown. Otherwise the comment is added to pg_description
 * by calling the CreateComments() routine. If the comment string is
 * empty, CreateComments() will drop any comments associated with
 * the object.
 *------------------------------------------------------------------
*/

void
CommentObject(int objtype, char *schemaname, char *objname, char *objproperty,
			  List *objlist, char *comment)
{
	switch (objtype)
	{
		case INDEX:
		case SEQUENCE:
		case TABLE:
		case VIEW:
			CommentRelation(objtype, schemaname, objname, comment);
			break;
		case COLUMN:
			CommentAttribute(schemaname, objname, objproperty, comment);
			break;
		case DATABASE:
			CommentDatabase(objname, comment);
			break;
		case RULE:
			CommentRewrite(objname, comment);
			break;
		case TYPE_P:
			CommentType(objname, comment);
			break;
		case AGGREGATE:
			CommentAggregate(objname, objlist, comment);
			break;
		case FUNCTION:
			CommentProc(objname, objlist, comment);
			break;
		case OPERATOR:
			CommentOperator(objname, objlist, comment);
			break;
		case TRIGGER:
			CommentTrigger(objname, schemaname, objproperty, comment);
			break;
		default:
			elog(ERROR, "An attempt was made to comment on a unknown type: %d",
				 objtype);
	}
}

/*------------------------------------------------------------------
 * CreateComments --
 *
 * Create a comment for the specified object descriptor.  Inserts a new
 * pg_description tuple, or replaces an existing one with the same key.
 *
 * If the comment given is null or an empty string, instead delete any
 * existing comment for the specified key.
 *------------------------------------------------------------------
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

/*------------------------------------------------------------------
 * DeleteComments --
 *
 * This routine is used to purge all comments associated with an object,
 * regardless of their objsubid.  It is called, for example, when a relation
 * is destroyed.
 *------------------------------------------------------------------
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

/*------------------------------------------------------------------
 * CommentRelation --
 *
 * This routine is used to add/drop a comment from a relation, where
 * a relation is a TABLE, SEQUENCE, VIEW or INDEX. The routine simply
 * finds the relation name by searching the system cache, locating
 * the appropriate tuple, and inserting a comment using that
 * tuple's oid. Its parameters are the relation name and comments.
 *------------------------------------------------------------------
 */

static void
CommentRelation(int reltype, char *schemaname, char *relname, char *comment)
{
	Relation	relation;
	RangeVar   *tgtrel = makeNode(RangeVar);
	
	
	tgtrel->relname = relname;
	tgtrel->schemaname = schemaname;
	/* FIXME SCHEMA: Can we add comments to temp relations? */
	tgtrel->istemp = false;

	/*
	 * Open the relation.  We do this mainly to acquire a lock that
	 * ensures no one else drops the relation before we commit.  (If they
	 * did, they'd fail to remove the entry we are about to make in
	 * pg_description.)
	 */
	relation = relation_openrv(tgtrel, AccessShareLock);

	/* Check object security */
	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		elog(ERROR, "you are not permitted to comment on class '%s'", relname);

	/* Next, verify that the relation type matches the intent */

	switch (reltype)
	{
		case INDEX:
			if (relation->rd_rel->relkind != RELKIND_INDEX)
				elog(ERROR, "relation '%s' is not an index", relname);
			break;
		case TABLE:
			if (relation->rd_rel->relkind != RELKIND_RELATION)
				elog(ERROR, "relation '%s' is not a table", relname);
			break;
		case VIEW:
			if (relation->rd_rel->relkind != RELKIND_VIEW)
				elog(ERROR, "relation '%s' is not a view", relname);
			break;
		case SEQUENCE:
			if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
				elog(ERROR, "relation '%s' is not a sequence", relname);
			break;
	}

	/* Create the comment using the relation's oid */

	CreateComments(RelationGetRelid(relation), RelOid_pg_class, 0, comment);

	/* Done, but hold lock until commit */
	relation_close(relation, NoLock);
}

/*------------------------------------------------------------------
 * CommentAttribute --
 *
 * This routine is used to add/drop a comment from an attribute
 * such as a table's column. The routine will check security
 * restrictions and then attempt to look up the specified
 * attribute. If successful, a comment is added/dropped, else an
 * elog() exception is thrown.	The parameters are the relation
 * and attribute names, and the comments
 *------------------------------------------------------------------
*/

static void
CommentAttribute(char *schemaname, char *relname, char *attrname, char *comment)
{
	RangeVar   *rel = makeNode(RangeVar);
	Relation	relation;
	AttrNumber	attnum;

	/* Open the containing relation to ensure it won't go away meanwhile */

	rel->relname = relname;
	rel->schemaname = schemaname;
	rel->istemp = false;
	relation = heap_openrv(rel, AccessShareLock);

	/* Check object security */

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		elog(ERROR, "you are not permitted to comment on class '%s'", relname);

	/* Now, fetch the attribute number from the system cache */

	attnum = get_attnum(RelationGetRelid(relation), attrname);
	if (attnum == InvalidAttrNumber)
		elog(ERROR, "'%s' is not an attribute of class '%s'",
			 attrname, relname);

	/* Create the comment using the relation's oid */

	CreateComments(RelationGetRelid(relation), RelOid_pg_class,
				   (int32) attnum, comment);

	/* Done, but hold lock until commit */

	heap_close(relation, NoLock);
}

/*------------------------------------------------------------------
 * CommentDatabase --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding the specified database. The routine will check
 * security for owner permissions, and, if succesful, will then
 * attempt to find the oid of the database specified. Once found,
 * a comment is added/dropped using the CreateComments() routine.
 *------------------------------------------------------------------
*/

static void
CommentDatabase(char *database, char *comment)
{
	Relation	pg_database;
	ScanKeyData entry;
	HeapScanDesc scan;
	HeapTuple	dbtuple;
	Oid			oid;

	/* First find the tuple in pg_database for the database */

	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(database));
	scan = heap_beginscan(pg_database, 0, SnapshotNow, 1, &entry);
	dbtuple = heap_getnext(scan, 0);

	/* Validate database exists, and fetch the db oid */

	if (!HeapTupleIsValid(dbtuple))
		elog(ERROR, "database '%s' does not exist", database);
	oid = dbtuple->t_data->t_oid;

	/* Allow if the user matches the database dba or is a superuser */

	if (!(superuser() || is_dbadmin(oid)))
		elog(ERROR, "you are not permitted to comment on database '%s'",
			 database);

	/* Create the comments with the pg_database oid */

	CreateComments(oid, RelOid_pg_database, 0, comment);

	/* Complete the scan and close any opened relations */

	heap_endscan(scan);
	heap_close(pg_database, AccessShareLock);
}

/*------------------------------------------------------------------
 * CommentRewrite --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding a specified RULE. The rule is specified by name
 * and, if found, and the user has appropriate permissions, a
 * comment will be added/dropped using the CreateComments() routine.
 *------------------------------------------------------------------
*/

static void
CommentRewrite(char *rule, char *comment)
{
	HeapTuple	tuple;
	Oid			reloid;
	Oid			ruleoid;
	Oid			classoid;
	int32		aclcheck;

	/* Find the rule's pg_rewrite tuple, get its OID and its table's OID */

	tuple = SearchSysCache(RULENAME,
						   PointerGetDatum(rule),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "rule '%s' does not exist", rule);

	reloid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	ruleoid = tuple->t_data->t_oid;

	ReleaseSysCache(tuple);

	/* Check object security */

	aclcheck = pg_class_aclcheck(reloid, GetUserId(), ACL_RULE);
	if (aclcheck != ACLCHECK_OK)
		elog(ERROR, "you are not permitted to comment on rule '%s'",
			 rule);

	/* pg_rewrite doesn't have a hard-coded OID, so must look it up */

	classoid = get_relname_relid(RewriteRelationName, PG_CATALOG_NAMESPACE);
	Assert(OidIsValid(classoid));

	/* Call CreateComments() to create/drop the comments */

	CreateComments(ruleoid, classoid, 0, comment);
}

/*------------------------------------------------------------------
 * CommentType --
 *
 * This routine is used to add/drop any user-comments a user might
 * have regarding a TYPE. The type is specified by name
 * and, if found, and the user has appropriate permissions, a
 * comment will be added/dropped using the CreateComments() routine.
 * The type's name and the comments are the paramters to this routine.
 *------------------------------------------------------------------
*/

static void
CommentType(char *type, char *comment)
{
	Oid			oid;

	/* Find the type's oid */

	/* XXX WRONG: need to deal with qualified type names */
	oid = typenameTypeId(makeTypeName(type));

	/* Check object security */

	if (!pg_type_ownercheck(oid, GetUserId()))
		elog(ERROR, "you are not permitted to comment on type '%s'",
			 type);

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, RelOid_pg_type, 0, comment);
}

/*------------------------------------------------------------------
 * CommentAggregate --
 *
 * This routine is used to allow a user to provide comments on an
 * aggregate function. The aggregate function is determined by both
 * its name and its argument type, which, with the comments are
 * the three parameters handed to this routine.
 *------------------------------------------------------------------
*/

static void
CommentAggregate(char *aggregate, List *arguments, char *comment)
{
	TypeName   *aggtype = (TypeName *) lfirst(arguments);
	Oid			baseoid,
				oid;
	Oid			classoid;

	/* First, attempt to determine the base aggregate oid */
	if (aggtype)
		baseoid = typenameTypeId(aggtype);
	else
		baseoid = InvalidOid;

	/* Now, attempt to find the actual tuple in pg_aggregate */

	oid = GetSysCacheOid(AGGNAME,
						 PointerGetDatum(aggregate),
						 ObjectIdGetDatum(baseoid),
						 0, 0);
	if (!OidIsValid(oid))
		agg_error("CommentAggregate", aggregate, baseoid);

	/* Next, validate the user's attempt to comment */

	if (!pg_aggr_ownercheck(oid, GetUserId()))
	{
		if (baseoid == InvalidOid)
			elog(ERROR, "you are not permitted to comment on aggregate '%s' for all types",
				 aggregate);
		else
			elog(ERROR, "you are not permitted to comment on aggregate '%s' for type %s",
				 aggregate, format_type_be(baseoid));
	}

	/* pg_aggregate doesn't have a hard-coded OID, so must look it up */

	classoid = get_relname_relid(AggregateRelationName, PG_CATALOG_NAMESPACE);
	Assert(OidIsValid(classoid));

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, classoid, 0, comment);
}

/*------------------------------------------------------------------
 * CommentProc --
 *
 * This routine is used to allow a user to provide comments on an
 * procedure (function). The procedure is determined by both
 * its name and its argument list. The argument list is expected to
 * be a series of parsed nodes pointed to by a List object. If the
 * comments string is empty, the associated comment is dropped.
 *------------------------------------------------------------------
*/

static void
CommentProc(char *function, List *arguments, char *comment)
{
	Oid			oid,
				argoids[FUNC_MAX_ARGS];
	int			i,
				argcount;

	/* First, initialize function's argument list with their type oids */

	MemSet(argoids, 0, FUNC_MAX_ARGS * sizeof(Oid));
	argcount = length(arguments);
	if (argcount > FUNC_MAX_ARGS)
		elog(ERROR, "functions cannot have more than %d arguments",
			 FUNC_MAX_ARGS);
	for (i = 0; i < argcount; i++)
	{
		TypeName   *t = (TypeName *) lfirst(arguments);

		argoids[i] = LookupTypeName(t);
		if (!OidIsValid(argoids[i]))
		{
			char      *typnam = TypeNameToString(t);

			if (strcmp(typnam, "opaque") == 0)
				argoids[i] = InvalidOid;
			else
				elog(ERROR, "Type \"%s\" does not exist", typnam);
		}

		arguments = lnext(arguments);
	}

	/* Now, find the corresponding oid for this procedure */

	oid = GetSysCacheOid(PROCNAME,
						 PointerGetDatum(function),
						 Int32GetDatum(argcount),
						 PointerGetDatum(argoids),
						 0);
	if (!OidIsValid(oid))
		func_error("CommentProc", function, argcount, argoids, NULL);

	/* Now, validate the user's ability to comment on this function */

	if (!pg_proc_ownercheck(oid, GetUserId()))
		elog(ERROR, "you are not permitted to comment on function '%s'",
			 function);

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, RelOid_pg_proc, 0, comment);
}

/*------------------------------------------------------------------
 * CommentOperator --
 *
 * This routine is used to allow a user to provide comments on an
 * operator. The operator for commenting is determined by both
 * its name and its argument list which defines the left and right
 * hand types the operator will operate on. The argument list is
 * expected to be a couple of parse nodes pointed to be a List
 * object. If the comments string is empty, the associated comment
 * is dropped.
 *
 * NOTE: we actually attach the comment to the procedure that underlies
 * the operator.  This is a feature, not a bug: we want the same comment
 * to be visible for both operator and function.
 *------------------------------------------------------------------
*/

static void
CommentOperator(char *opername, List *arguments, char *comment)
{
	TypeName   *typenode1 = (TypeName *) lfirst(arguments);
	TypeName   *typenode2 = (TypeName *) lsecond(arguments);
	char		oprtype = 0;
	Form_pg_operator data;
	HeapTuple	optuple;
	Oid			oid,
				leftoid = InvalidOid,
				rightoid = InvalidOid;

	/* Attempt to fetch the left type oid, if specified */
	if (typenode1 != NULL)
		leftoid = typenameTypeId(typenode1);

	/* Attempt to fetch the right type oid, if specified */
	if (typenode2 != NULL)
		rightoid = typenameTypeId(typenode2);

	/* Determine operator type */

	if (OidIsValid(leftoid) && (OidIsValid(rightoid)))
		oprtype = 'b';
	else if (OidIsValid(leftoid))
		oprtype = 'r';
	else if (OidIsValid(rightoid))
		oprtype = 'l';
	else
		elog(ERROR, "operator '%s' is of an illegal type'", opername);

	/* Attempt to fetch the operator oid */

	optuple = SearchSysCache(OPERNAME,
							 PointerGetDatum(opername),
							 ObjectIdGetDatum(leftoid),
							 ObjectIdGetDatum(rightoid),
							 CharGetDatum(oprtype));
	if (!HeapTupleIsValid(optuple))
		elog(ERROR, "operator '%s' does not exist", opername);

	oid = optuple->t_data->t_oid;

	/* Valid user's ability to comment on this operator */

	if (!pg_oper_ownercheck(oid, GetUserId()))
		elog(ERROR, "you are not permitted to comment on operator '%s'",
			 opername);

	/* Get the procedure associated with the operator */

	data = (Form_pg_operator) GETSTRUCT(optuple);
	oid = data->oprcode;
	if (oid == InvalidOid)
		elog(ERROR, "operator '%s' does not have an underlying function", opername);

	ReleaseSysCache(optuple);

	/* Call CreateComments() to create/drop the comments */

	CreateComments(oid, RelOid_pg_proc, 0, comment);
}

/*------------------------------------------------------------------
 * CommentTrigger --
 *
 * This routine is used to allow a user to provide comments on a
 * trigger event. The trigger for commenting is determined by both
 * its name and the relation to which it refers. The arguments to this
 * function are the trigger name, the relation name, and the comments
 * to add/drop.
 *------------------------------------------------------------------
*/

static void
CommentTrigger(char *trigger, char *schemaname, char *relname, char *comment)
{
	RangeVar   *rel = makeNode(RangeVar);
	Relation	pg_trigger,
				relation;
	HeapTuple	triggertuple;
	HeapScanDesc scan;
	ScanKeyData entry[2];
	Oid			oid;

	/* First, validate the user's action */

	rel->relname = relname;
	rel->schemaname = schemaname;
	rel->istemp = false;
	relation = heap_openrv(rel, AccessShareLock);

	if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
		elog(ERROR, "you are not permitted to comment on trigger '%s' %s '%s'",
			 trigger, "defined for relation", relname);

	/* Fetch the trigger oid from pg_trigger  */

	pg_trigger = heap_openr(TriggerRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry[0], 0x0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));
	ScanKeyEntryInitialize(&entry[1], 0x0, Anum_pg_trigger_tgname,
						   F_NAMEEQ,
						   NameGetDatum(trigger));
	scan = heap_beginscan(pg_trigger, 0, SnapshotNow, 2, entry);
	triggertuple = heap_getnext(scan, 0);

	/* If no trigger exists for the relation specified, notify user */

	if (!HeapTupleIsValid(triggertuple))
		elog(ERROR, "trigger '%s' defined for relation '%s' does not exist",
			 trigger, relname);

	oid = triggertuple->t_data->t_oid;

	heap_endscan(scan);

	/* Create the comments with the pg_trigger oid */

	CreateComments(oid, RelationGetRelid(pg_trigger), 0, comment);

	/* Done, but hold lock on relation */

	heap_close(pg_trigger, AccessShareLock);
	heap_close(relation, NoLock);
}
