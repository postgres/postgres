/*-------------------------------------------------------------------------
 *
 * policy.c
 *	  Commands for manipulating policies.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/commands/policy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_type.h"
#include "commands/policy.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "parser/parse_clause.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rowsecurity.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void RangeVarCallbackForPolicy(const RangeVar *rv,
				Oid relid, Oid oldrelid, void *arg);
static char parse_policy_command(const char *cmd_name);
static ArrayType* policy_role_list_to_array(List *roles);

/*
 * Callback to RangeVarGetRelidExtended().
 *
 * Checks the following:
 *  - the relation specified is a table.
 *  - current user owns the table.
 *  - the table is not a system table.
 *
 * If any of these checks fails then an error is raised.
 */
static void
RangeVarCallbackForPolicy(const RangeVar *rv, Oid relid, Oid oldrelid,
								void *arg)
{
	HeapTuple		tuple;
	Form_pg_class	classform;
	char			relkind;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;

	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/* Must own relation. */
	if (!pg_class_ownercheck(relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, rv->relname);

	/* No system table modifications unless explicitly allowed. */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/* Relation type MUST be a table. */
	if (relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 * parse_policy_command -
 *   helper function to convert full command strings to their char
 *   representation.
 *
 * cmd_name - full string command name. Valid values are 'all', 'select',
 *			  'insert', 'update' and 'delete'.
 *
 */
static char
parse_policy_command(const char *cmd_name)
{
	char cmd;

	if (!cmd_name)
		elog(ERROR, "unrecognized policy command");

	if (strcmp(cmd_name, "all") == 0)
		cmd = '*';
	else if (strcmp(cmd_name, "select") == 0)
		cmd = ACL_SELECT_CHR;
	else if (strcmp(cmd_name, "insert") == 0)
		cmd = ACL_INSERT_CHR;
	else if (strcmp(cmd_name, "update") == 0)
		cmd = ACL_UPDATE_CHR;
	else if (strcmp(cmd_name, "delete") == 0)
		cmd = ACL_DELETE_CHR;
	else
		elog(ERROR, "unrecognized policy command");

	return cmd;
}

/*
 * policy_role_list_to_array
 *   helper function to convert a list of role names in to an array of
 *   role ids.
 *
 * Note: If PUBLIC is provided as a role name, then ACL_ID_PUBLIC is
 *       used as the role id.
 *
 * roles - the list of role names to convert.
 */
static ArrayType *
policy_role_list_to_array(List *roles)
{
	ArrayType  *role_ids;
	Datum	   *temp_array;
	ListCell   *cell;
	int			num_roles;
	int			i = 0;

	/* Handle no roles being passed in as being for public */
	if (roles == NIL)
	{
		temp_array = (Datum *) palloc(sizeof(Datum));
		temp_array[0] = ObjectIdGetDatum(ACL_ID_PUBLIC);

		role_ids = construct_array(temp_array, 1, OIDOID, sizeof(Oid), true,
								   'i');
		return role_ids;
	}

	num_roles = list_length(roles);
	temp_array = (Datum *) palloc(num_roles * sizeof(Datum));

	foreach(cell, roles)
	{
		Oid		roleid = get_role_oid_or_public(strVal(lfirst(cell)));

		/*
		 * PUBLIC covers all roles, so it only makes sense alone.
		 */
		if (roleid == ACL_ID_PUBLIC)
		{
			if (num_roles != 1)
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("ignoring roles specified other than public"),
						 errhint("All roles are members of the public role.")));

			temp_array[0] = ObjectIdGetDatum(roleid);
			num_roles = 1;
			break;
		}
		else
			temp_array[i++] = ObjectIdGetDatum(roleid);
	}

	role_ids = construct_array(temp_array, num_roles, OIDOID, sizeof(Oid), true,
							   'i');

	return role_ids;
}

/*
 * Load row security policy from the catalog, and store it in
 * the relation's relcache entry.
 *
 * We will always set up some kind of policy here.  If no explicit policies
 * are found then an implicit default-deny policy is created.
 */
void
RelationBuildRowSecurity(Relation relation)
{
	MemoryContext		rscxt;
	MemoryContext		oldcxt = CurrentMemoryContext;
	RowSecurityDesc	   * volatile rsdesc = NULL;

	/*
	 * Create a memory context to hold everything associated with this
	 * relation's row security policy.  This makes it easy to clean up
	 * during a relcache flush.
	 */
	rscxt = AllocSetContextCreate(CacheMemoryContext,
								  "row security descriptor",
								  ALLOCSET_SMALL_MINSIZE,
								  ALLOCSET_SMALL_INITSIZE,
								  ALLOCSET_SMALL_MAXSIZE);

	/*
	 * Since rscxt lives under CacheMemoryContext, it is long-lived.  Use
	 * a PG_TRY block to ensure it'll get freed if we fail partway through.
	 */
	PG_TRY();
	{
		Relation			catalog;
		ScanKeyData			skey;
		SysScanDesc			sscan;
		HeapTuple			tuple;

		rsdesc = MemoryContextAllocZero(rscxt, sizeof(RowSecurityDesc));
		rsdesc->rscxt = rscxt;

		catalog = heap_open(PolicyRelationId, AccessShareLock);

		ScanKeyInit(&skey,
					Anum_pg_policy_polrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(relation)));

		sscan = systable_beginscan(catalog, PolicyPolrelidPolnameIndexId, true,
								   NULL, 1, &skey);

		/*
		 * Loop through the row level security policies for this relation, if
		 * any.
		 */
		while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
		{
			Datum				value_datum;
			char				cmd_value;
			Datum				roles_datum;
			char			   *qual_value;
			Expr			   *qual_expr;
			char			   *with_check_value;
			Expr			   *with_check_qual;
			char			   *policy_name_value;
			Oid					policy_id;
			bool				isnull;
			RowSecurityPolicy  *policy;

			/*
			 * Note: all the pass-by-reference data we collect here is either
			 * still stored in the tuple, or constructed in the caller's
			 * short-lived memory context.  We must copy it into rscxt
			 * explicitly below.
			 */

			/* Get policy command */
			value_datum = heap_getattr(tuple, Anum_pg_policy_polcmd,
								 RelationGetDescr(catalog), &isnull);
			Assert(!isnull);
			cmd_value = DatumGetChar(value_datum);

			/* Get policy name */
			value_datum = heap_getattr(tuple, Anum_pg_policy_polname,
										RelationGetDescr(catalog), &isnull);
			Assert(!isnull);
			policy_name_value = NameStr(*(DatumGetName(value_datum)));

			/* Get policy roles */
			roles_datum = heap_getattr(tuple, Anum_pg_policy_polroles,
										RelationGetDescr(catalog), &isnull);
			/* shouldn't be null, but initdb doesn't mark it so, so check */
			if (isnull)
				elog(ERROR, "unexpected null value in pg_policy.polroles");

			/* Get policy qual */
			value_datum = heap_getattr(tuple, Anum_pg_policy_polqual,
								 RelationGetDescr(catalog), &isnull);
			if (!isnull)
			{
				qual_value = TextDatumGetCString(value_datum);
				qual_expr = (Expr *) stringToNode(qual_value);
			}
			else
				qual_expr = NULL;

			/* Get WITH CHECK qual */
			value_datum = heap_getattr(tuple, Anum_pg_policy_polwithcheck,
										RelationGetDescr(catalog), &isnull);
			if (!isnull)
			{
				with_check_value = TextDatumGetCString(value_datum);
				with_check_qual = (Expr *) stringToNode(with_check_value);
			}
			else
				with_check_qual = NULL;

			policy_id = HeapTupleGetOid(tuple);

			/* Now copy everything into the cache context */
			MemoryContextSwitchTo(rscxt);

			policy = palloc0(sizeof(RowSecurityPolicy));
			policy->policy_name = pstrdup(policy_name_value);
			policy->policy_id = policy_id;
			policy->polcmd = cmd_value;
			policy->roles = DatumGetArrayTypePCopy(roles_datum);
			policy->qual = copyObject(qual_expr);
			policy->with_check_qual = copyObject(with_check_qual);
			policy->hassublinks = checkExprHasSubLink((Node *) qual_expr) ||
								  checkExprHasSubLink((Node *) with_check_qual);

			rsdesc->policies = lcons(policy, rsdesc->policies);

			MemoryContextSwitchTo(oldcxt);

			/* clean up some (not all) of the junk ... */
			if (qual_expr != NULL)
				pfree(qual_expr);
			if (with_check_qual != NULL)
				pfree(with_check_qual);
		}

		systable_endscan(sscan);
		heap_close(catalog, AccessShareLock);

		/*
		 * Check if no policies were added
		 *
		 * If no policies exist in pg_policy for this relation, then we
		 * need to create a single default-deny policy.  We use InvalidOid for
		 * the Oid to indicate that this is the default-deny policy (we may
		 * decide to ignore the default policy if an extension adds policies).
		 */
		if (rsdesc->policies == NIL)
		{
			RowSecurityPolicy  *policy;
			Datum				role;

			MemoryContextSwitchTo(rscxt);

			role = ObjectIdGetDatum(ACL_ID_PUBLIC);

			policy = palloc0(sizeof(RowSecurityPolicy));
			policy->policy_name = pstrdup("default-deny policy");
			policy->policy_id = InvalidOid;
			policy->polcmd = '*';
			policy->roles = construct_array(&role, 1, OIDOID, sizeof(Oid), true,
											'i');
			policy->qual = (Expr *) makeConst(BOOLOID, -1, InvalidOid,
											  sizeof(bool), BoolGetDatum(false),
											  false, true);
			policy->with_check_qual = copyObject(policy->qual);
			policy->hassublinks = false;

			rsdesc->policies = lcons(policy, rsdesc->policies);

			MemoryContextSwitchTo(oldcxt);
		}
	}
	PG_CATCH();
	{
		/* Delete rscxt, first making sure it isn't active */
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(rscxt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Success --- attach the policy descriptor to the relcache entry */
	relation->rd_rsdesc = rsdesc;
}

/*
 * RemovePolicyById -
 *   remove a policy by its OID.  If a policy does not exist with the provided
 *   oid, then an error is raised.
 *
 * policy_id - the oid of the policy.
 */
void
RemovePolicyById(Oid policy_id)
{
	Relation 	pg_policy_rel;
	SysScanDesc sscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;
	Oid			relid;
	Relation	rel;

	pg_policy_rel = heap_open(PolicyRelationId, RowExclusiveLock);

	/*
	 * Find the policy to delete.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
							   NULL, 1, skey);

	tuple = systable_getnext(sscan);

	/* If the policy exists, then remove it, otherwise raise an error. */
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for policy %u", policy_id);

	/*
	 * Open and exclusive-lock the relation the policy belong to.
	 */
	relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;

	rel = heap_open(relid, AccessExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	simple_heap_delete(pg_policy_rel, &tuple->t_self);

	systable_endscan(sscan);
	heap_close(rel, AccessExclusiveLock);

	/*
	 * Note that, unlike some of the other flags in pg_class, relrowsecurity
	 * is not just an indication of if policies exist.  When relrowsecurity
	 * is set by a user, then all access to the relation must be through a
	 * policy.  If no policy is defined for the relation then a default-deny
	 * policy is created and all records are filtered (except for queries from
	 * the owner).
	 */

	CacheInvalidateRelcache(rel);

	/* Clean up */
	heap_close(pg_policy_rel, RowExclusiveLock);
}

/*
 * CreatePolicy -
 *   handles the execution of the CREATE POLICY command.
 *
 * stmt - the CreatePolicyStmt that describes the policy to create.
 */
ObjectAddress
CreatePolicy(CreatePolicyStmt *stmt)
{
	Relation		pg_policy_rel;
	Oid				policy_id;
	Relation		target_table;
	Oid				table_id;
	char			polcmd;
	ArrayType	   *role_ids;
	ParseState	   *qual_pstate;
	ParseState	   *with_check_pstate;
	RangeTblEntry  *rte;
	Node		   *qual;
	Node		   *with_check_qual;
	ScanKeyData		skey[2];
	SysScanDesc		sscan;
	HeapTuple		policy_tuple;
	Datum			values[Natts_pg_policy];
	bool			isnull[Natts_pg_policy];
	ObjectAddress	target;
	ObjectAddress	myself;

	/* Parse command */
	polcmd = parse_policy_command(stmt->cmd);

	/*
	 * If the command is SELECT or DELETE then WITH CHECK should be NULL.
	 */
	if ((polcmd == ACL_SELECT_CHR || polcmd == ACL_DELETE_CHR)
		&& stmt->with_check != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("WITH CHECK cannot be applied to SELECT or DELETE")));

	/*
	 * If the command is INSERT then WITH CHECK should be the only expression
	 * provided.
	 */
	if (polcmd == ACL_INSERT_CHR && stmt->qual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only WITH CHECK expression allowed for INSERT")));


	/* Collect role ids */
	role_ids = policy_role_list_to_array(stmt->roles);

	/* Parse the supplied clause */
	qual_pstate = make_parsestate(NULL);
	with_check_pstate = make_parsestate(NULL);

	/* zero-clear */
	memset(values,   0, sizeof(values));
	memset(isnull,   0, sizeof(isnull));

	/* Get id of table.  Also handles permissions checks. */
	table_id = RangeVarGetRelidExtended(stmt->table, AccessExclusiveLock,
										false, false,
										RangeVarCallbackForPolicy,
										(void *) stmt);

	/* Open target_table to build quals. No lock is necessary.*/
	target_table = relation_open(table_id, NoLock);

	/* Add for the regular security quals */
	rte = addRangeTableEntryForRelation(qual_pstate, target_table,
										NULL, false, false);
	addRTEtoQuery(qual_pstate, rte, false, true, true);

	/* Add for the with-check quals */
	rte = addRangeTableEntryForRelation(with_check_pstate, target_table,
										NULL, false, false);
	addRTEtoQuery(with_check_pstate, rte, false, true, true);

	qual = transformWhereClause(qual_pstate,
								copyObject(stmt->qual),
								EXPR_KIND_WHERE,
								"POLICY");

	with_check_qual = transformWhereClause(with_check_pstate,
								copyObject(stmt->with_check),
								EXPR_KIND_WHERE,
								"POLICY");

	/* Open pg_policy catalog */
	pg_policy_rel = heap_open(PolicyRelationId, RowExclusiveLock);

	/* Set key - policy's relation id. */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Set key - policy's name. */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* Complain if the policy name already exists for the table */
	if (HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" for relation \"%s\" already exists",
				 stmt->policy_name, RelationGetRelationName(target_table))));

	values[Anum_pg_policy_polrelid - 1] = ObjectIdGetDatum(table_id);
	values[Anum_pg_policy_polname - 1] = DirectFunctionCall1(namein,
															 CStringGetDatum(stmt->policy_name));
	values[Anum_pg_policy_polcmd - 1] = CharGetDatum(polcmd);
	values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);

	/* Add qual if present. */
	if (qual)
		values[Anum_pg_policy_polqual - 1] = CStringGetTextDatum(nodeToString(qual));
	else
		isnull[Anum_pg_policy_polqual - 1] = true;

	/* Add WITH CHECK qual if present */
	if (with_check_qual)
		values[Anum_pg_policy_polwithcheck - 1] = CStringGetTextDatum(nodeToString(with_check_qual));
	else
		isnull[Anum_pg_policy_polwithcheck - 1] = true;

	policy_tuple = heap_form_tuple(RelationGetDescr(pg_policy_rel), values,
								   isnull);

	policy_id = simple_heap_insert(pg_policy_rel, policy_tuple);

	/* Update Indexes */
	CatalogUpdateIndexes(pg_policy_rel, policy_tuple);

	/* Record Dependencies */
	target.classId = RelationRelationId;
	target.objectId = table_id;
	target.objectSubId = 0;

	myself.classId = PolicyRelationId;
	myself.objectId = policy_id;
	myself.objectSubId = 0;

	recordDependencyOn(&myself, &target, DEPENDENCY_AUTO);

	recordDependencyOnExpr(&myself, qual, qual_pstate->p_rtable,
						   DEPENDENCY_NORMAL);

	recordDependencyOnExpr(&myself, with_check_qual,
						   with_check_pstate->p_rtable, DEPENDENCY_NORMAL);

	/* Invalidate Relation Cache */
	CacheInvalidateRelcache(target_table);

	/* Clean up. */
	heap_freetuple(policy_tuple);
	free_parsestate(qual_pstate);
	free_parsestate(with_check_pstate);
	systable_endscan(sscan);
	relation_close(target_table, NoLock);
	heap_close(pg_policy_rel, RowExclusiveLock);

	return myself;
}

/*
 * AlterPolicy -
 *   handles the execution of the ALTER POLICY command.
 *
 * stmt - the AlterPolicyStmt that describes the policy and how to alter it.
 */
ObjectAddress
AlterPolicy(AlterPolicyStmt *stmt)
{
	Relation		pg_policy_rel;
	Oid				policy_id;
	Relation		target_table;
	Oid				table_id;
	ArrayType	   *role_ids = NULL;
	List		   *qual_parse_rtable = NIL;
	List		   *with_check_parse_rtable = NIL;
	Node		   *qual = NULL;
	Node		   *with_check_qual = NULL;
	ScanKeyData		skey[2];
	SysScanDesc		sscan;
	HeapTuple		policy_tuple;
	HeapTuple		new_tuple;
	Datum			values[Natts_pg_policy];
	bool			isnull[Natts_pg_policy];
	bool			replaces[Natts_pg_policy];
	ObjectAddress	target;
	ObjectAddress	myself;
	Datum			cmd_datum;
	char			polcmd;
	bool			polcmd_isnull;

	/* Parse role_ids */
	if (stmt->roles != NULL)
		role_ids = policy_role_list_to_array(stmt->roles);

	/* Get id of table.  Also handles permissions checks. */
	table_id = RangeVarGetRelidExtended(stmt->table, AccessExclusiveLock,
										false, false,
										RangeVarCallbackForPolicy,
										(void *) stmt);

	target_table = relation_open(table_id, NoLock);

	/* Parse the using policy clause */
	if (stmt->qual)
	{
		RangeTblEntry  *rte;
		ParseState	   *qual_pstate = make_parsestate(NULL);

		rte = addRangeTableEntryForRelation(qual_pstate, target_table,
											NULL, false, false);

		addRTEtoQuery(qual_pstate, rte, false, true, true);

		qual = transformWhereClause(qual_pstate, copyObject(stmt->qual),
									EXPR_KIND_WHERE,
									"POLICY");

		qual_parse_rtable = qual_pstate->p_rtable;
		free_parsestate(qual_pstate);
	}

	/* Parse the with-check policy clause */
	if (stmt->with_check)
	{
		RangeTblEntry  *rte;
		ParseState	   *with_check_pstate = make_parsestate(NULL);

		rte = addRangeTableEntryForRelation(with_check_pstate, target_table,
											NULL, false, false);

		addRTEtoQuery(with_check_pstate, rte, false, true, true);

		with_check_qual = transformWhereClause(with_check_pstate,
											   copyObject(stmt->with_check),
											   EXPR_KIND_WHERE,
											   "POLICY");

		with_check_parse_rtable = with_check_pstate->p_rtable;
		free_parsestate(with_check_pstate);
	}

	/* zero-clear */
	memset(values,   0, sizeof(values));
	memset(replaces, 0, sizeof(replaces));
	memset(isnull,   0, sizeof(isnull));

	/* Find policy to update. */
	pg_policy_rel = heap_open(PolicyRelationId, RowExclusiveLock);

	/* Set key - policy's relation id. */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Set key - policy's name. */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* Check that the policy is found, raise an error if not. */
	if (!HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" on table \"%s\" does not exist",
						stmt->policy_name,
						RelationGetRelationName(target_table))));

	/* Get policy command */
	cmd_datum = heap_getattr(policy_tuple, Anum_pg_policy_polcmd,
							 RelationGetDescr(pg_policy_rel),
							 &polcmd_isnull);
	Assert(!polcmd_isnull);
	polcmd = DatumGetChar(cmd_datum);

	/*
	 * If the command is SELECT or DELETE then WITH CHECK should be NULL.
	 */
	if ((polcmd == ACL_SELECT_CHR || polcmd == ACL_DELETE_CHR)
		&& stmt->with_check != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only USING expression allowed for SELECT, DELETE")));

	/*
	 * If the command is INSERT then WITH CHECK should be the only
	 * expression provided.
	 */
	if ((polcmd == ACL_INSERT_CHR)
		&& stmt->qual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only WITH CHECK expression allowed for INSERT")));

	policy_id = HeapTupleGetOid(policy_tuple);

	if (role_ids != NULL)
	{
		replaces[Anum_pg_policy_polroles - 1] = true;
		values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);
	}

	if (qual != NULL)
	{
		replaces[Anum_pg_policy_polqual - 1] = true;
		values[Anum_pg_policy_polqual - 1]
			= CStringGetTextDatum(nodeToString(qual));
	}

	if (with_check_qual != NULL)
	{
		replaces[Anum_pg_policy_polwithcheck - 1] = true;
		values[Anum_pg_policy_polwithcheck - 1]
			= CStringGetTextDatum(nodeToString(with_check_qual));
	}

	new_tuple = heap_modify_tuple(policy_tuple,
								  RelationGetDescr(pg_policy_rel),
								  values, isnull, replaces);
	simple_heap_update(pg_policy_rel, &new_tuple->t_self, new_tuple);

	/* Update Catalog Indexes */
	CatalogUpdateIndexes(pg_policy_rel, new_tuple);

	/* Update Dependencies. */
	deleteDependencyRecordsFor(PolicyRelationId, policy_id, false);

	/* Record Dependencies */
	target.classId = RelationRelationId;
	target.objectId = table_id;
	target.objectSubId = 0;

	myself.classId = PolicyRelationId;
	myself.objectId = policy_id;
	myself.objectSubId = 0;

	recordDependencyOn(&myself, &target, DEPENDENCY_AUTO);

	recordDependencyOnExpr(&myself, qual, qual_parse_rtable, DEPENDENCY_NORMAL);

	recordDependencyOnExpr(&myself, with_check_qual, with_check_parse_rtable,
						   DEPENDENCY_NORMAL);

	heap_freetuple(new_tuple);

	/* Invalidate Relation Cache */
	CacheInvalidateRelcache(target_table);

	/* Clean up. */
	systable_endscan(sscan);
	relation_close(target_table, NoLock);
	heap_close(pg_policy_rel, RowExclusiveLock);

	return myself;
}

/*
 * rename_policy -
 *   change the name of a policy on a relation
 */
ObjectAddress
rename_policy(RenameStmt *stmt)
{
	Relation		pg_policy_rel;
	Relation		target_table;
	Oid				table_id;
	Oid				opoloid;
	ScanKeyData		skey[2];
	SysScanDesc		sscan;
	HeapTuple		policy_tuple;
	ObjectAddress	address;

	/* Get id of table.  Also handles permissions checks. */
	table_id = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
										false, false,
										RangeVarCallbackForPolicy,
										(void *) stmt);

	target_table = relation_open(table_id, NoLock);

	pg_policy_rel = heap_open(PolicyRelationId, RowExclusiveLock);

	/* First pass -- check for conflict */

	/* Add key - policy's relation id. */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Add key - policy's name. */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->newname));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	if (HeapTupleIsValid(systable_getnext(sscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" already exists",
						stmt->newname, RelationGetRelationName(target_table))));

	systable_endscan(sscan);

	/* Second pass -- find existing policy and update */
	/* Add key - policy's relation id. */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Add key - policy's name. */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->subname));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* Complain if we did not find the policy */
	if (!HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(target_table))));

	opoloid = HeapTupleGetOid(policy_tuple);

	policy_tuple = heap_copytuple(policy_tuple);

	namestrcpy(&((Form_pg_policy) GETSTRUCT(policy_tuple))->polname,
			   stmt->newname);

	simple_heap_update(pg_policy_rel, &policy_tuple->t_self, policy_tuple);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(pg_policy_rel, policy_tuple);

	InvokeObjectPostAlterHook(PolicyRelationId,
							  HeapTupleGetOid(policy_tuple), 0);

	ObjectAddressSet(address, PolicyRelationId, opoloid);

	/*
	 * Invalidate relation's relcache entry so that other backends (and
	 * this one too!) are sent SI message to make them rebuild relcache
	 * entries.  (Ideally this should happen automatically...)
	 */
	CacheInvalidateRelcache(target_table);

	/* Clean up. */
	systable_endscan(sscan);
	heap_close(pg_policy_rel, RowExclusiveLock);
	relation_close(target_table, NoLock);

	return address;
}

/*
 * get_relation_policy_oid - Look up a policy by name to find its OID
 *
 * If missing_ok is false, throw an error if policy not found.  If
 * true, just return InvalidOid.
 */
Oid
get_relation_policy_oid(Oid relid, const char *policy_name, bool missing_ok)
{
	Relation		pg_policy_rel;
	ScanKeyData		skey[2];
	SysScanDesc		sscan;
	HeapTuple		policy_tuple;
	Oid				policy_oid;

	pg_policy_rel = heap_open(PolicyRelationId, AccessShareLock);

	/* Add key - policy's relation id. */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	/* Add key - policy's name. */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	if (!HeapTupleIsValid(policy_tuple))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("policy \"%s\" for table  \"%s\" does not exist",
							policy_name, get_rel_name(relid))));

		policy_oid = InvalidOid;
	}
	else
		policy_oid = HeapTupleGetOid(policy_tuple);

	/* Clean up. */
	systable_endscan(sscan);
	heap_close(pg_policy_rel, AccessShareLock);

	return policy_oid;
}
