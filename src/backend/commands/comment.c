/*-------------------------------------------------------------------------
 *
 * comment.c
 *
 * PostgreSQL object comments utility code.
 *
 * Copyright (c) 1999, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_database.h"
#include "catalog/pg_description.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/pg_class.h"
#include "commands/comment.h"
#include "miscadmin.h"
#include "parser/parse.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "rewrite/rewriteRemove.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
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

static void CommentRelation(int objtype, char *relation, char *comment);
static void CommentAttribute(char *relation, char *attrib, char *comment);
static void CommentDatabase(char *database, char *comment);
static void CommentRewrite(char *rule, char *comment);
static void CommentType(char *type, char *comment);
static void CommentAggregate(char *aggregate, List *arguments, char *comment);
static void CommentProc(char *function, List *arguments, char *comment);
static void CommentOperator(char *opname, List *arguments, char *comment);
static void CommentTrigger(char *trigger, char *relation, char *comments);
static void	CreateComments(Oid oid, char *comment);

/*------------------------------------------------------------------
 * CommentObject --
 *
 * This routine is used to add the associated comment into
 * pg_description for the object specified by the paramters handed
 * to this routine. If the routine cannot determine an Oid to
 * associated with the parameters handed to this routine, an
 * error is thrown. Otherwise the comment is added to pg_description
 * by calling the CreateComments() routine. If the comments were
 * empty, CreateComments() will drop any comments associated with
 * the object.
 *------------------------------------------------------------------
*/

void
CommentObject(int objtype, char *objname, char *objproperty,
			  List *objlist, char *comment)
{

	switch (objtype)
	{
			case (INDEX):
			case (SEQUENCE):
			case (TABLE):
			case (VIEW):
			CommentRelation(objtype, objname, comment);
			break;
		case (COLUMN):
			CommentAttribute(objname, objproperty, comment);
			break;
		case (DATABASE):
			CommentDatabase(objname, comment);
			break;
		case (RULE):
			CommentRewrite(objname, comment);
			break;
		case (TYPE_P):
			CommentType(objname, comment);
			break;
		case (AGGREGATE):
			CommentAggregate(objname, objlist, comment);
			break;
		case (FUNCTION):
			CommentProc(objname, objlist, comment);
			break;
		case (OPERATOR):
			CommentOperator(objname, objlist, comment);
			break;
		case (TRIGGER):
			CommentTrigger(objname, objproperty, comment);
			break;
		default:
			elog(ERROR, "An attempt was made to comment on a unknown type: %i",
				 objtype);
	}


}

/*------------------------------------------------------------------
 * CreateComments --
 *
 * This routine is handed the oid and the command associated
 * with that id and will insert, update, or delete (if the
 * comment is an empty string or a NULL pointer) the associated
 * comment from the system cataloge, pg_description.
 *
 *------------------------------------------------------------------
 */

static void
CreateComments(Oid oid, char *comment)
{

	Relation	description;
	TupleDesc	tupDesc;
	HeapScanDesc scan;
	ScanKeyData entry;
	HeapTuple	desctuple = NULL,
				searchtuple;
	Datum		values[Natts_pg_description];
	char		nulls[Natts_pg_description];
	char		replaces[Natts_pg_description];
	bool		modified = false;
	int			i;

	/*** Open pg_description, form a new tuple, if necessary ***/

	description = heap_openr(DescriptionRelationName, RowExclusiveLock);
	tupDesc = description->rd_att;
	if ((comment != NULL) && (strlen(comment) > 0))
	{
		for (i = 0; i < Natts_pg_description; i++)
		{
			nulls[i] = ' ';
			replaces[i] = 'r';
			values[i] = (Datum) NULL;
		}
		i = 0;
		values[i++] = ObjectIdGetDatum(oid);
		values[i++] = DirectFunctionCall1(textin, CStringGetDatum(comment));
	}

	/*** Now, open pg_description and attempt to find the old tuple ***/

	ScanKeyEntryInitialize(&entry, 0x0, Anum_pg_description_objoid, F_OIDEQ,
						   ObjectIdGetDatum(oid));
	scan = heap_beginscan(description, false, SnapshotNow, 1, &entry);
	searchtuple = heap_getnext(scan, 0);

	/*** If a previous tuple exists, either delete or prep replacement ***/

	if (HeapTupleIsValid(searchtuple))
	{

		/*** If the comment is blank, call heap_delete, else heap_update ***/

		if ((comment == NULL) || (strlen(comment) == 0))
			heap_delete(description, &searchtuple->t_self, NULL);
		else
		{
			desctuple = heap_modifytuple(searchtuple, description, values,
										 nulls, replaces);
			heap_update(description, &searchtuple->t_self, desctuple, NULL);
			modified = TRUE;
		}

	}
	else
	{

		/*** Only if comment is non-blank do we form a new tuple ***/

		if ((comment != NULL) && (strlen(comment) > 0))
		{
			desctuple = heap_formtuple(tupDesc, values, nulls);
			heap_insert(description, desctuple);
			modified = TRUE;
		}

	}

	/*** Complete the scan, update indices, if necessary ***/

	heap_endscan(scan);

	if (modified)
	{
		if (RelationGetForm(description)->relhasindex)
		{
			Relation	idescs[Num_pg_description_indices];

			CatalogOpenIndices(Num_pg_description_indices,
							   Name_pg_description_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_description_indices, description,
							   desctuple);
			CatalogCloseIndices(Num_pg_description_indices, idescs);
		}
		heap_freetuple(desctuple);

	}

	heap_close(description, RowExclusiveLock);

}

/*------------------------------------------------------------------
 * DeleteComments --
 *
 * This routine is used to purge any comments
 * associated with the Oid handed to this routine,
 * regardless of the actual object type. It is
 * called, for example, when a relation is destroyed.
 *------------------------------------------------------------------
 */

void
DeleteComments(Oid oid)
{

	Relation	description;
	TupleDesc	tupDesc;
	ScanKeyData entry;
	HeapScanDesc scan;
	HeapTuple	searchtuple;

	description = heap_openr(DescriptionRelationName, RowExclusiveLock);
	tupDesc = description->rd_att;

	/*** Now, open pg_description and attempt to find the old tuple ***/

	ScanKeyEntryInitialize(&entry, 0x0, Anum_pg_description_objoid, F_OIDEQ,
						   ObjectIdGetDatum(oid));
	scan = heap_beginscan(description, false, SnapshotNow, 1, &entry);
	searchtuple = heap_getnext(scan, 0);

	/*** If a previous tuple exists, delete it ***/

	if (HeapTupleIsValid(searchtuple))
		heap_delete(description, &searchtuple->t_self, NULL);

	/*** Complete the scan, update indices, if necessary ***/

	heap_endscan(scan);
	heap_close(description, RowExclusiveLock);

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
CommentRelation(int reltype, char *relname, char *comment)
{
	HeapTuple	reltuple;
	Oid			oid;
	char		relkind;

	/*** First, check object security ***/

	if (!pg_ownercheck(GetUserId(), relname, RELNAME))
		elog(ERROR, "you are not permitted to comment on class '%s'", relname);

	/*** Now, attempt to find the oid in the cached version of pg_class ***/

	reltuple = SearchSysCache(RELNAME,
							  PointerGetDatum(relname),
							  0, 0, 0);
	if (!HeapTupleIsValid(reltuple))
		elog(ERROR, "relation '%s' does not exist", relname);

	oid = reltuple->t_data->t_oid;

	relkind = ((Form_pg_class) GETSTRUCT(reltuple))->relkind;

	ReleaseSysCache(reltuple);

	/*** Next, verify that the relation type matches the intent ***/

	switch (reltype)
	{
		case (INDEX):
			if (relkind != RELKIND_INDEX)
				elog(ERROR, "relation '%s' is not an index", relname);
			break;
		case (TABLE):
			if (relkind != RELKIND_RELATION)
				elog(ERROR, "relation '%s' is not a table", relname);
			break;
		case (VIEW):
			if (relkind != RELKIND_VIEW)
				elog(ERROR, "relation '%s' is not a view", relname);
			break;
		case (SEQUENCE):
			if (relkind != RELKIND_SEQUENCE)
				elog(ERROR, "relation '%s' is not a sequence", relname);
			break;
	}

	/*** Create the comments using the tuple's oid ***/

	CreateComments(oid, comment);
}

/*------------------------------------------------------------------
 * CommentAttribute --
 *
 * This routine is used to add/drop a comment from an attribute
 * such as a table's column. The routine will check security
 * restrictions and then attempt to fetch the oid of the associated
 * attribute. If successful, a comment is added/dropped, else an
 * elog() exception is thrown.	The parameters are the relation
 * and attribute names, and the comments
 *------------------------------------------------------------------
*/

static void
CommentAttribute(char *relname, char *attrname, char *comment)
{
	Relation	relation;
	Oid			oid;

	/*** First, check object security ***/

	if (!pg_ownercheck(GetUserId(), relname, RELNAME))
		elog(ERROR, "you are not permitted to comment on class '%s\'", relname);

	/* Open the containing relation to ensure it won't go away meanwhile */

	relation = heap_openr(relname, AccessShareLock);

	/*** Now, fetch the attribute oid from the system cache ***/

	oid = GetSysCacheOid(ATTNAME,
						 ObjectIdGetDatum(relation->rd_id),
						 PointerGetDatum(attrname),
						 0, 0);
	if (!OidIsValid(oid))
		elog(ERROR, "'%s' is not an attribute of class '%s'",
			 attrname, relname);

	/*** Call CreateComments() to create/drop the comments ***/

	CreateComments(oid, comment);

	/*** Now, close the heap relation and return ***/

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
	HeapTuple	dbtuple,
				usertuple;
	ScanKeyData entry;
	HeapScanDesc scan;
	Oid			oid;
	bool		superuser;
	int32		dba;
	Oid		userid;

	/*** First find the tuple in pg_database for the database ***/

	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(database));
	scan = heap_beginscan(pg_database, 0, SnapshotNow, 1, &entry);
	dbtuple = heap_getnext(scan, 0);

	/*** Validate database exists, and fetch the dba id and oid ***/

	if (!HeapTupleIsValid(dbtuple))
		elog(ERROR, "database '%s' does not exist", database);
	dba = ((Form_pg_database) GETSTRUCT(dbtuple))->datdba;
	oid = dbtuple->t_data->t_oid;

	/*** Now, fetch user information ***/

	userid = GetUserId();
	usertuple = SearchSysCache(SHADOWSYSID,
							   ObjectIdGetDatum(userid),
							   0, 0, 0);
	if (!HeapTupleIsValid(usertuple))
		elog(ERROR, "invalid user id %u", (unsigned) userid);
	superuser = ((Form_pg_shadow) GETSTRUCT(usertuple))->usesuper;
	ReleaseSysCache(usertuple);

	/*** Allow if the userid matches the database dba or is a superuser ***/

#ifndef NO_SECURITY
	if (!(superuser || (userid == dba)))
	{
		elog(ERROR, "you are not permitted to comment on database '%s'",
			 database);
	}
#endif

	/*** Create the comments with the pg_database oid ***/

	CreateComments(oid, comment);

	/*** Complete the scan and close any opened relations ***/

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
	Oid			oid;
	char	   *relation;
	int			aclcheck;

	/*** First, validate user ***/

#ifndef NO_SECURITY
	relation = RewriteGetRuleEventRel(rule);
	aclcheck = pg_aclcheck(relation, GetUserId(), ACL_RU);
	if (aclcheck != ACLCHECK_OK)
	{
		elog(ERROR, "you are not permitted to comment on rule '%s'",
			 rule);
	}
#endif

	/*** Next, find the rule's oid ***/

	oid = GetSysCacheOid(RULENAME,
						 PointerGetDatum(rule),
						 0, 0, 0);
	if (!OidIsValid(oid))
		elog(ERROR, "rule '%s' does not exist", rule);

	/*** Call CreateComments() to create/drop the comments ***/

	CreateComments(oid, comment);
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

	/*** First, validate user ***/

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), type, TYPENAME))
	{
		elog(ERROR, "you are not permitted to comment on type '%s'",
			 type);
	}
#endif

	/*** Next, find the type's oid ***/

	oid = GetSysCacheOid(TYPENAME,
						 PointerGetDatum(type),
						 0, 0, 0);
	if (!OidIsValid(oid))
		elog(ERROR, "type '%s' does not exist", type);

	/*** Call CreateComments() to create/drop the comments ***/

	CreateComments(oid, comment);
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
	char	   *aggtypename = NULL;
	Oid			baseoid,
				oid;
	bool		defined;

	/*** First, attempt to determine the base aggregate oid ***/

	if (aggtype)
	{
		aggtypename = TypeNameToInternalName(aggtype);
		baseoid = TypeGet(aggtypename, &defined);
		if (!OidIsValid(baseoid))
			elog(ERROR, "type '%s' does not exist", aggtypename);
	}
	else
		baseoid = 0;

	/*** Next, validate the user's attempt to comment ***/

#ifndef NO_SECURITY
	if (!pg_aggr_ownercheck(GetUserId(), aggregate, baseoid))
	{
		if (aggtypename)
		{
			elog(ERROR, "you are not permitted to comment on aggregate '%s' %s '%s'",
				 aggregate, "with type", aggtypename);
		}
		else
		{
			elog(ERROR, "you are not permitted to comment on aggregate '%s'",
				 aggregate);
		}
	}
#endif

	/*** Now, attempt to find the actual tuple in pg_aggregate ***/

	oid = GetSysCacheOid(AGGNAME,
						 PointerGetDatum(aggregate),
						 ObjectIdGetDatum(baseoid),
						 0, 0);
	if (!OidIsValid(oid))
	{
		if (aggtypename)
		{
			elog(ERROR, "aggregate type '%s' does not exist for aggregate '%s'",
				 aggtypename, aggregate);
		}
		else
			elog(ERROR, "aggregate '%s' does not exist", aggregate);
	}

	/*** Call CreateComments() to create/drop the comments ***/

	CreateComments(oid, comment);
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

	/*** First, initialize function's argument list with their type oids ***/

	MemSet(argoids, 0, FUNC_MAX_ARGS * sizeof(Oid));
	argcount = length(arguments);
	if (argcount > FUNC_MAX_ARGS)
		elog(ERROR, "functions cannot have more than %d arguments",
			 FUNC_MAX_ARGS);
	for (i = 0; i < argcount; i++)
	{
		TypeName   *t = (TypeName *) lfirst(arguments);
		char	   *typnam = TypeNameToInternalName(t);

		arguments = lnext(arguments);

		if (strcmp(typnam, "opaque") == 0)
			argoids[i] = InvalidOid;
		else
		{
			argoids[i] = GetSysCacheOid(TYPENAME,
										PointerGetDatum(typnam),
										0, 0, 0);
			if (!OidIsValid(argoids[i]))
				elog(ERROR, "CommentProc: type '%s' not found", typnam);
		}
	}

	/*** Now, validate the user's ability to comment on this function ***/

#ifndef NO_SECURITY
	if (!pg_func_ownercheck(GetUserId(), function, argcount, argoids))
		elog(ERROR, "you are not permitted to comment on function '%s'",
			 function);
#endif

	/*** Now, find the corresponding oid for this procedure ***/

	oid = GetSysCacheOid(PROCNAME,
						 PointerGetDatum(function),
						 Int32GetDatum(argcount),
						 PointerGetDatum(argoids),
						 0);
	if (!OidIsValid(oid))
		func_error("CommentProc", function, argcount, argoids, NULL);

	/*** Call CreateComments() to create/drop the comments ***/

	CreateComments(oid, comment);
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
 *------------------------------------------------------------------
*/

static void
CommentOperator(char *opername, List *arguments, char *comment)
{
	TypeName   *typenode1 = (TypeName *) lfirst(arguments);
	TypeName   *typenode2 = (TypeName *) lsecond(arguments);
	char		oprtype = 0,
			   *lefttype = NULL,
			   *righttype = NULL;
	Form_pg_operator data;
	HeapTuple	optuple;
	Oid			oid,
				leftoid = InvalidOid,
				rightoid = InvalidOid;
	bool		defined;

	/*** Initialize our left and right argument types ***/

	if (typenode1 != NULL)
		lefttype = TypeNameToInternalName(typenode1);
	if (typenode2 != NULL)
		righttype = TypeNameToInternalName(typenode2);

	/*** Attempt to fetch the left oid, if specified ***/

	if (lefttype != NULL)
	{
		leftoid = TypeGet(lefttype, &defined);
		if (!OidIsValid(leftoid))
			elog(ERROR, "left type '%s' does not exist", lefttype);
	}

	/*** Attempt to fetch the right oid, if specified ***/

	if (righttype != NULL)
	{
		rightoid = TypeGet(righttype, &defined);
		if (!OidIsValid(rightoid))
			elog(ERROR, "right type '%s' does not exist", righttype);
	}

	/*** Determine operator type ***/

	if (OidIsValid(leftoid) && (OidIsValid(rightoid)))
		oprtype = 'b';
	else if (OidIsValid(leftoid))
		oprtype = 'r';
	else if (OidIsValid(rightoid))
		oprtype = 'l';
	else
		elog(ERROR, "operator '%s' is of an illegal type'", opername);

	/*** Attempt to fetch the operator oid ***/

	optuple = SearchSysCache(OPERNAME,
							 PointerGetDatum(opername),
							 ObjectIdGetDatum(leftoid),
							 ObjectIdGetDatum(rightoid),
							 CharGetDatum(oprtype));
	if (!HeapTupleIsValid(optuple))
		elog(ERROR, "operator '%s' does not exist", opername);

	oid = optuple->t_data->t_oid;

	/*** Valid user's ability to comment on this operator ***/

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), (char *) ObjectIdGetDatum(oid), OPEROID))
	{
		elog(ERROR, "you are not permitted to comment on operator '%s'",
			 opername);
	}
#endif

	/*** Get the procedure associated with the operator ***/

	data = (Form_pg_operator) GETSTRUCT(optuple);
	oid = RegprocToOid(data->oprcode);
	if (oid == InvalidOid)
		elog(ERROR, "operator '%s' does not have an underlying function", opername);

	ReleaseSysCache(optuple);

	/*** Call CreateComments() to create/drop the comments ***/

	CreateComments(oid, comment);
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
CommentTrigger(char *trigger, char *relname, char *comment)
{

	Form_pg_trigger data;
	Relation	pg_trigger,
				relation;
	HeapTuple	triggertuple;
	HeapScanDesc scan;
	ScanKeyData entry;
	Oid			oid = InvalidOid;

	/*** First, validate the user's action ***/

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relname, RELNAME))
	{
		elog(ERROR, "you are not permitted to comment on trigger '%s' %s '%s'",
			 trigger, "defined for relation", relname);
	}
#endif

	/*** Now, fetch the trigger oid from pg_trigger  ***/

	relation = heap_openr(relname, AccessShareLock);
	pg_trigger = heap_openr(TriggerRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ, RelationGetRelid(relation));
	scan = heap_beginscan(pg_trigger, 0, SnapshotNow, 1, &entry);
	triggertuple = heap_getnext(scan, 0);
	while (HeapTupleIsValid(triggertuple))
	{
		data = (Form_pg_trigger) GETSTRUCT(triggertuple);
		if (namestrcmp(&(data->tgname), trigger) == 0)
		{
			oid = triggertuple->t_data->t_oid;
			break;
		}
		triggertuple = heap_getnext(scan, 0);
	}

	/*** If no trigger exists for the relation specified, notify user ***/

	if (oid == InvalidOid)
	{
		elog(ERROR, "trigger '%s' defined for relation '%s' does not exist",
			 trigger, relname);
	}

	/*** Create the comments with the pg_trigger oid ***/

	CreateComments(oid, comment);

	/*** Complete the scan and close any opened relations ***/

	heap_endscan(scan);
	heap_close(pg_trigger, AccessShareLock);
	heap_close(relation, NoLock);
}
