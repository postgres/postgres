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
#include "catalog/heap.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_database.h"
#include "catalog/pg_description.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "miscadmin.h"
#include "rewrite/rewriteRemove.h"
#include "utils/acl.h"
#include "utils/syscache.h"

#include "../backend/parser/parse.h"

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
static void CommentAggregate(char *aggregate, char *aggtype, char *comment);
static void CommentProc(char *function, List *arguments, char *comment);
static void CommentOperator(char *opname, List *arguments, char *comment);
static void CommentTrigger(char *trigger, char *relation, char *comments);

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

void CommentObject(int objtype, char *objname, char *objproperty,
		   List *objlist, char *comment) {

  switch (objtype) {
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
    CommentAggregate(objname, objproperty, comment);
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

void CreateComments(Oid oid, char *comment) {

  Relation description;
  TupleDesc tupDesc;
  HeapScanDesc scan;
  ScanKeyData entry;
  HeapTuple desctuple = NULL, searchtuple;
  Datum values[Natts_pg_description];
  char nulls[Natts_pg_description];
  char replaces[Natts_pg_description];
  bool modified = false;
  int i;

  /*** Open pg_description, form a new tuple, if necessary ***/

  description = heap_openr(DescriptionRelationName, RowExclusiveLock);
  tupDesc = description->rd_att;
  if ((comment != NULL) && (strlen(comment) > 0)) {
    for (i = 0; i < Natts_pg_description; i++) {
      nulls[i] = ' ';
      replaces[i] = 'r';
      values[i] = (Datum) NULL;
    }
    i = 0;
    values[i++] = ObjectIdGetDatum(oid);
    values[i++] = (Datum) fmgr(F_TEXTIN, comment);
  }

  /*** Now, open pg_description and attempt to find the old tuple ***/

  ScanKeyEntryInitialize(&entry, 0x0, Anum_pg_description_objoid, F_OIDEQ,
			 ObjectIdGetDatum(oid));
  scan = heap_beginscan(description, false, SnapshotNow, 1, &entry);
  searchtuple = heap_getnext(scan, 0);

  /*** If a previous tuple exists, either delete or prep replacement ***/

  if (HeapTupleIsValid(searchtuple)) {

    /*** If the comment is blank, call heap_delete, else heap_update ***/

    if ((comment == NULL) || (strlen(comment) == 0)) {
      heap_delete(description, &searchtuple->t_self, NULL);
    } else {
      desctuple = heap_modifytuple(searchtuple, description, values,
				   nulls, replaces);
      setheapoverride(true);
      heap_update(description, &searchtuple->t_self, desctuple, NULL);
      setheapoverride(false);
      modified = TRUE;
    }

  } else {
    desctuple = heap_formtuple(tupDesc, values, nulls);
    heap_insert(description, desctuple);
    modified = TRUE;
  }

  /*** Complete the scan, update indices, if necessary ***/

  heap_endscan(scan);

  if (modified) {
    if (RelationGetForm(description)->relhasindex) {
      Relation idescs[Num_pg_description_indices];

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

void DeleteComments(Oid oid) {

  Relation description;
  TupleDesc tupDesc;
  ScanKeyData entry;
  HeapScanDesc scan;
  HeapTuple searchtuple;

  description = heap_openr(DescriptionRelationName, RowExclusiveLock);
  tupDesc = description->rd_att;

  /*** Now, open pg_description and attempt to find the old tuple ***/

  ScanKeyEntryInitialize(&entry, 0x0, Anum_pg_description_objoid, F_OIDEQ,
			 ObjectIdGetDatum(oid));
  scan = heap_beginscan(description, false, SnapshotNow, 1, &entry);
  searchtuple = heap_getnext(scan, 0);

  /*** If a previous tuple exists, delete it ***/

  if (HeapTupleIsValid(searchtuple)) {
    heap_delete(description, &searchtuple->t_self, NULL);
  }

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

void CommentRelation(int reltype, char *relname, char *comment) {

  HeapTuple reltuple;
  Oid oid;
  char relkind;

  /*** First, check object security ***/

  #ifndef NO_SECURITY
  if (!pg_ownercheck(GetPgUserName(), relname, RELNAME)) {
    elog(ERROR, "you are not permitted to comment on class '%s'", relname);
  }
  #endif

  /*** Now, attempt to find the oid in the cached version of pg_class ***/

  reltuple = SearchSysCacheTuple(RELNAME, PointerGetDatum(relname),
				 0, 0, 0);
  if (!HeapTupleIsValid(reltuple)) {
    elog(ERROR, "relation '%s' does not exist", relname);
  }

  oid = reltuple->t_data->t_oid;

  /*** Next, verify that the relation type matches the intent ***/

  relkind = ((Form_pg_class) GETSTRUCT(reltuple))->relkind;

  switch (reltype) {
  case (INDEX):
    if (relkind != 'i') {
      elog(ERROR, "relation '%s' is not an index", relname);
    }
    break;
  case (TABLE):
    if (relkind != 'r') {
      elog(ERROR, "relation '%s' is not a table", relname);
    }
    break;
  case (VIEW):
    if (relkind != 'r') {
      elog(ERROR, "relation '%s' is not a view", relname);
    }
    break;
  case (SEQUENCE):
    if (relkind != 'S') {
      elog(ERROR, "relation '%s' is not a sequence", relname);
    }
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
 * elog() exception is thrown.  The parameters are the relation
 * and attribute names, and the comments
 *------------------------------------------------------------------
*/

void CommentAttribute(char *relname, char *attrname, char *comment) {

  Relation relation;
  HeapTuple attrtuple;
  Oid oid;

  /*** First, check object security ***/

  #ifndef NO_SECURITY
  if (!pg_ownercheck(GetPgUserName(), relname, RELNAME)) {
    elog(ERROR, "you are not permitted to comment on class '%s\'", relname);
  }
  #endif

  /*** Now, fetch the attribute oid from the system cache ***/

  relation = heap_openr(relname, AccessShareLock);
  attrtuple = SearchSysCacheTuple(ATTNAME, ObjectIdGetDatum(relation->rd_id),
				  PointerGetDatum(attrname), 0, 0);
  if (!HeapTupleIsValid(attrtuple)) {
    elog(ERROR, "'%s' is not an attribute of class '%s'",
	 attrname, relname);
  }
  oid = attrtuple->t_data->t_oid;

  /*** Call CreateComments() to create/drop the comments ***/

  CreateComments(oid, comment);

  /*** Now, close the heap relation and return ***/

  heap_close(relation, AccessShareLock);

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

void CommentDatabase(char *database, char *comment) {

  Relation pg_database;
  HeapTuple dbtuple, usertuple;
  ScanKeyData entry;
  HeapScanDesc scan;
  Oid oid;
  bool superuser;
  int4 dba, userid;
  char *username;

  /*** First find the tuple in pg_database for the database ***/

  pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
  ScanKeyEntryInitialize(&entry, 0, Anum_pg_database_datname,
			 F_NAMEEQ, NameGetDatum(database));
  scan = heap_beginscan(pg_database, 0, SnapshotNow, 1, &entry);
  dbtuple = heap_getnext(scan, 0);

  /*** Validate database exists, and fetch the dba id and oid ***/

  if (!HeapTupleIsValid(dbtuple)) {
    elog(ERROR, "database '%s' does not exist", database);
  }
  dba = ((Form_pg_database) GETSTRUCT(dbtuple))->datdba;
  oid = dbtuple->t_data->t_oid;

  /*** Now, fetch user information ***/

  username = GetPgUserName();
  usertuple = SearchSysCacheTuple(SHADOWNAME, PointerGetDatum(username),
				  0, 0, 0);
  if (!HeapTupleIsValid(usertuple)) {
    elog(ERROR, "current user '%s' does not exist", username);
  }
  userid = ((Form_pg_shadow) GETSTRUCT(usertuple))->usesysid;
  superuser = ((Form_pg_shadow) GETSTRUCT(usertuple))->usesuper;

  /*** Allow if the userid matches the database dba or is a superuser ***/

  #ifndef NO_SECURITY
  if (!(superuser || (userid == dba))) {
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

void CommentRewrite(char *rule, char *comment) {

  HeapTuple rewritetuple;
  Oid oid;
  char *user, *relation;
  int aclcheck;

  /*** First, validate user ***/

  #ifndef NO_SECURITY
  user = GetPgUserName();
  relation = RewriteGetRuleEventRel(rule);
  aclcheck = pg_aclcheck(relation, user, ACL_RU);
  if (aclcheck != ACLCHECK_OK) {
    elog(ERROR, "you are not permitted to comment on rule '%s'",
	 rule);
  }
  #endif

  /*** Next, find the rule's oid ***/

  rewritetuple = SearchSysCacheTuple(RULENAME, PointerGetDatum(rule),
				     0, 0, 0);
  if (!HeapTupleIsValid(rewritetuple)) {
    elog(ERROR, "rule '%s' does not exist", rule);
  }

  oid = rewritetuple->t_data->t_oid;

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

void CommentType(char *type, char *comment) {

  HeapTuple typetuple;
  Oid oid;
  char *user;

  /*** First, validate user ***/

  #ifndef NO_SECURITY
  user = GetPgUserName();
  if (!pg_ownercheck(user, type, TYPENAME)) {
    elog(ERROR, "you are not permitted to comment on type '%s'",
	 type);
  }
  #endif

  /*** Next, find the type's oid ***/

  typetuple = SearchSysCacheTuple(TYPENAME, PointerGetDatum(type),
				  0, 0, 0);
  if (!HeapTupleIsValid(typetuple)) {
    elog(ERROR, "type '%s' does not exist", type);
  }

  oid = typetuple->t_data->t_oid;

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

void CommentAggregate(char *aggregate, char *argument, char *comment) {

  HeapTuple aggtuple;
  Oid baseoid, oid;
  bool defined;
  char *user;

  /*** First, attempt to determine the base aggregate oid ***/

  if (argument) {
    baseoid = TypeGet(argument, &defined);
    if (!OidIsValid(baseoid)) {
      elog(ERROR, "aggregate type '%s' does not exist", argument);
    }
  } else {
    baseoid = 0;
  }

  /*** Next, validate the user's attempt to comment ***/

  #ifndef NO_SECURITY
  user = GetPgUserName();
  if (!pg_aggr_ownercheck(user, aggregate, baseoid)) {
    if (argument) {
      elog(ERROR, "you are not permitted to comment on aggregate '%s' %s '%s'",
	   aggregate, "with type", argument);
    } else {
      elog(ERROR, "you are not permitted to comment on aggregate '%s'",
	   aggregate);
    }
  }
  #endif

  /*** Now, attempt to find the actual tuple in pg_aggregate ***/

  aggtuple = SearchSysCacheTuple(AGGNAME, PointerGetDatum(aggregate),
				 ObjectIdGetDatum(baseoid), 0, 0);
  if (!HeapTupleIsValid(aggtuple)) {
    if (argument) {
      elog(ERROR, "aggregate type '%s' does not exist for aggregate '%s'",
	   argument, aggregate);
    } else {
      elog(ERROR, "aggregate '%s' does not exist", aggregate);
    }
  }

  oid = aggtuple->t_data->t_oid;

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

void CommentProc(char *function, List *arguments, char *comment)
{
	HeapTuple argtuple, functuple;
	Oid oid, argoids[FUNC_MAX_ARGS];
	char *user, *argument;
	int i, argcount;

	/*** First, initialize function's argument list with their type oids ***/

	MemSet(argoids, 0, FUNC_MAX_ARGS * sizeof(Oid));
	argcount = length(arguments);
	if (argcount > FUNC_MAX_ARGS)
		elog(ERROR, "functions cannot have more than %d arguments",
			 FUNC_MAX_ARGS);
	for (i = 0; i < argcount; i++) {
		argument = strVal(lfirst(arguments));
		arguments = lnext(arguments);
		if (strcmp(argument, "opaque") == 0)
		{
			argoids[i] = 0;
		}
		else
		{
			argtuple = SearchSysCacheTuple(TYPENAME,
										   PointerGetDatum(argument),
										   0, 0, 0);
			if (!HeapTupleIsValid(argtuple))
				elog(ERROR, "function argument type '%s' does not exist",
					 argument);
			argoids[i] = argtuple->t_data->t_oid;
		}
    }

	/*** Now, validate the user's ability to comment on this function ***/

#ifndef NO_SECURITY
	user = GetPgUserName();
	if (!pg_func_ownercheck(user, function, argcount, argoids))
		elog(ERROR, "you are not permitted to comment on function '%s'",
			 function);
#endif

	/*** Now, find the corresponding oid for this procedure ***/

	functuple = SearchSysCacheTuple(PROCNAME, PointerGetDatum(function),
									Int32GetDatum(argcount),
									PointerGetDatum(argoids), 0);

	if (!HeapTupleIsValid(functuple))
		elog(ERROR, "function '%s' with the supplied %s does not exist",
			 function, "argument list");
	oid = functuple->t_data->t_oid;

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

void CommentOperator(char *opername, List *arguments, char *comment) {

  HeapTuple optuple;
  Oid oid, leftoid = InvalidOid, rightoid = InvalidOid;
  bool defined;
  char oprtype = 0, *user, *lefttype = NULL, *righttype = NULL;

  /*** Initialize our left and right argument types ***/

  if (lfirst(arguments) != NULL) {
    lefttype = strVal(lfirst(arguments));
  }
  if (lsecond(arguments) != NULL) {
    righttype = strVal(lsecond(arguments));
  }

  /*** Attempt to fetch the left oid, if specified ***/

  if (lefttype != NULL) {
    leftoid = TypeGet(lefttype, &defined);
    if (!OidIsValid(leftoid)) {
      elog(ERROR, "left type '%s' does not exist", lefttype);
    }
  }

  /*** Attempt to fetch the right oid, if specified ***/

  if (righttype != NULL) {
    rightoid = TypeGet(righttype, &defined);
    if (!OidIsValid(rightoid)) {
      elog(ERROR, "right type '%s' does not exist", righttype);
    }
  }

  /*** Determine operator type ***/

  if (OidIsValid(leftoid) && (OidIsValid(rightoid))) oprtype = 'b';
  else if (OidIsValid(leftoid)) oprtype = 'l';
  else if (OidIsValid(rightoid)) oprtype = 'r';
  else elog(ERROR, "operator '%s' is of an illegal type'", opername);

  /*** Attempt to fetch the operator oid ***/

  optuple = SearchSysCacheTupleCopy(OPERNAME, PointerGetDatum(opername),
				    ObjectIdGetDatum(leftoid),
				    ObjectIdGetDatum(rightoid),
				    CharGetDatum(oprtype));
  if (!HeapTupleIsValid(optuple)) {
    elog(ERROR, "operator '%s' does not exist", opername);
  }

  oid = optuple->t_data->t_oid;

  /*** Valid user's ability to comment on this operator ***/

  #ifndef NO_SECURITY
  user = GetPgUserName();
  if (!pg_ownercheck(user, (char *) ObjectIdGetDatum(oid), OPEROID)) {
    elog(ERROR, "you are not permitted to comment on operator '%s'",
	 opername);
  }
  #endif

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

void CommentTrigger(char *trigger, char *relname, char *comment) {

  Form_pg_trigger data;
  Relation pg_trigger, relation;
  HeapTuple triggertuple;
  HeapScanDesc scan;
  ScanKeyData entry;
  Oid oid = InvalidOid;
  char *user;

  /*** First, validate the user's action ***/

  #ifndef NO_SECURITY
  user = GetPgUserName();
  if (!pg_ownercheck(user, relname, RELNAME)) {
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
  while (HeapTupleIsValid(triggertuple)) {
    data = (Form_pg_trigger) GETSTRUCT(triggertuple);
    if (namestrcmp(&(data->tgname), trigger) == 0) {
      oid = triggertuple->t_data->t_oid;
      break;
    }
    triggertuple = heap_getnext(scan, 0);
  }

  /*** If no trigger exists for the relation specified, notify user ***/

  if (oid == InvalidOid) {
    elog(ERROR, "trigger '%s' defined for relation '%s' does not exist",
	 trigger, relname);
  }

  /*** Create the comments with the pg_trigger oid ***/

  CreateComments(oid, comment);

  /*** Complete the scan and close any opened relations ***/

  heap_endscan(scan);
  heap_close(pg_trigger, AccessShareLock);
  heap_close(relation, AccessShareLock);

}
