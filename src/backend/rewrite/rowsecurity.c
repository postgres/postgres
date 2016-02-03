/*
 * rewrite/rowsecurity.c
 *	  Routines to support policies for row level security (aka RLS).
 *
 * Policies in PostgreSQL provide a mechanism to limit what records are
 * returned to a user and what records a user is permitted to add to a table.
 *
 * Policies can be defined for specific roles, specific commands, or provided
 * by an extension.  Row security can also be enabled for a table without any
 * policies being explicitly defined, in which case a default-deny policy is
 * applied.
 *
 * Any part of the system which is returning records back to the user, or
 * which is accepting records from the user to add to a table, needs to
 * consider the policies associated with the table (if any).  For normal
 * queries, this is handled by calling get_row_security_policies() during
 * rewrite, for each RTE in the query.  This returns the expressions defined
 * by the table's policies as a list that is prepended to the securityQuals
 * list for the RTE.  For queries which modify the table, any WITH CHECK
 * clauses from the table's policies are also returned and prepended to the
 * list of WithCheckOptions for the Query to check each row that is being
 * added to the table.  Other parts of the system (eg: COPY) simply construct
 * a normal query and use that, if RLS is to be applied.
 *
 * The check to see if RLS should be enabled is provided through
 * check_enable_rls(), which returns an enum (defined in rowsecurity.h) to
 * indicate if RLS should be enabled (RLS_ENABLED), or bypassed (RLS_NONE or
 * RLS_NONE_ENV).  RLS_NONE_ENV indicates that RLS should be bypassed
 * in the current environment, but that may change if the row_security GUC or
 * the current role changes.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rowsecurity.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/syscache.h"
#include "tcop/utility.h"

static void get_policies_for_relation(Relation relation,
						  CmdType cmd, Oid user_id,
						  List **permissive_policies,
						  List **restrictive_policies);

static List *sort_policies_by_name(List *policies);

static int row_security_policy_cmp(const void *a, const void *b);

static void add_security_quals(int rt_index,
							   List *permissive_policies,
							   List *restrictive_policies,
							   List **securityQuals,
							   bool *hasSubLinks);

static void add_with_check_options(Relation rel,
								   int rt_index,
								   WCOKind kind,
								   List *permissive_policies,
								   List *restrictive_policies,
								   List **withCheckOptions,
								   bool *hasSubLinks);

static bool check_role_for_policy(ArrayType *policy_roles, Oid user_id);

/*
 * hooks to allow extensions to add their own security policies
 *
 * row_security_policy_hook_permissive can be used to add policies which
 * are included in the "OR"d set of policies.
 *
 * row_security_policy_hook_restrictive can be used to add policies which
 * are enforced, regardless of other policies (they are "AND"d).
 */
row_security_policy_hook_type row_security_policy_hook_permissive = NULL;
row_security_policy_hook_type row_security_policy_hook_restrictive = NULL;

/*
 * Get any row security quals and WithCheckOption checks that should be
 * applied to the specified RTE.
 *
 * In addition, hasRowSecurity is set to true if row level security is enabled
 * (even if this RTE doesn't have any row security quals), and hasSubLinks is
 * set to true if any of the quals returned contain sublinks.
 */
void
get_row_security_policies(Query *root, RangeTblEntry *rte, int rt_index,
						  List **securityQuals, List **withCheckOptions,
						  bool *hasRowSecurity, bool *hasSubLinks)
{
	Oid			user_id;
	int			rls_status;
	Relation	rel;
	CmdType		commandType;
	List	   *permissive_policies;
	List	   *restrictive_policies;

	/* Defaults for the return values */
	*securityQuals = NIL;
	*withCheckOptions = NIL;
	*hasRowSecurity = false;
	*hasSubLinks = false;

	/* If this is not a normal relation, just return immediately */
	if (rte->relkind != RELKIND_RELATION)
		return;

	/* Switch to checkAsUser if it's set */
	user_id = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Determine the state of RLS for this, pass checkAsUser explicitly */
	rls_status = check_enable_rls(rte->relid, rte->checkAsUser, false);

	/* If there is no RLS on this table at all, nothing to do */
	if (rls_status == RLS_NONE)
		return;

	/*
	 * RLS_NONE_ENV means we are not doing any RLS now, but that may change
	 * with changes to the environment, so we mark it as hasRowSecurity to
	 * force a re-plan when the environment changes.
	 */
	if (rls_status == RLS_NONE_ENV)
	{
		/*
		 * Indicate that this query may involve RLS and must therefore be
		 * replanned if the environment changes (GUCs, role), but we are not
		 * adding anything here.
		 */
		*hasRowSecurity = true;

		return;
	}

	/*
	 * RLS is enabled for this relation.
	 *
	 * Get the security policies that should be applied, based on the command
	 * type.  Note that if this isn't the target relation, we actually want
	 * the relation's SELECT policies, regardless of the query command type,
	 * for example in UPDATE t1 ... FROM t2 we need to apply t1's UPDATE
	 * policies and t2's SELECT policies.
	 */
	rel = heap_open(rte->relid, NoLock);

	commandType = rt_index == root->resultRelation ?
				  root->commandType : CMD_SELECT;

	/*
	 * In some cases, we need to apply USING policies (which control the
	 * visibility of records) associated with multiple command types (see
	 * specific cases below).
	 *
	 * When considering the order in which to apply these USING policies,
	 * we prefer to apply higher privileged policies, those which allow the
	 * user to lock records (UPDATE and DELETE), first, followed by policies
	 * which don't (SELECT).
	 *
	 * Note that the optimizer is free to push down and reorder quals which
	 * use leakproof functions.
	 *
	 * In all cases, if there are no policy clauses allowing access to rows in
	 * the table for the specific type of operation, then a single always-false
	 * clause (a default-deny policy) will be added (see add_security_quals).
	 */

	/*
	 * For a SELECT, if UPDATE privileges are required (eg: the user has
	 * specified FOR [KEY] UPDATE/SHARE), then add the UPDATE USING quals first.
	 *
	 * This way, we filter out any records from the SELECT FOR SHARE/UPDATE
	 * which the user does not have access to via the UPDATE USING policies,
	 * similar to how we require normal UPDATE rights for these queries.
	 */
	if (commandType == CMD_SELECT && rte->requiredPerms & ACL_UPDATE)
	{
		List	   *update_permissive_policies;
		List	   *update_restrictive_policies;

		get_policies_for_relation(rel, CMD_UPDATE, user_id,
								  &update_permissive_policies,
								  &update_restrictive_policies);

		add_security_quals(rt_index,
						   update_permissive_policies,
						   update_restrictive_policies,
						   securityQuals,
						   hasSubLinks);
	}

	/*
	 * For SELECT, UPDATE and DELETE, add security quals to enforce the USING
	 * policies.  These security quals control access to existing table rows.
	 * Restrictive policies are "AND"d together, and permissive policies are
	 * "OR"d together.
	 */

	get_policies_for_relation(rel, commandType, user_id, &permissive_policies,
							  &restrictive_policies);

	if (commandType == CMD_SELECT ||
		commandType == CMD_UPDATE ||
		commandType == CMD_DELETE)
		add_security_quals(rt_index,
						   permissive_policies,
						   restrictive_policies,
						   securityQuals,
						   hasSubLinks);

	/*
	 * Similar to above, during an UPDATE or DELETE, if SELECT rights are also
	 * required (eg: when a RETURNING clause exists, or the user has provided
	 * a WHERE clause which involves columns from the relation), we collect up
	 * CMD_SELECT policies and add them via add_security_quals first.
	 *
	 * This way, we filter out any records which are not visible through an ALL
	 * or SELECT USING policy.
	 */
	if ((commandType == CMD_UPDATE || commandType == CMD_DELETE) &&
		rte->requiredPerms & ACL_SELECT)
	{
		List	   *select_permissive_policies;
		List	   *select_restrictive_policies;

		get_policies_for_relation(rel, CMD_SELECT, user_id,
								  &select_permissive_policies,
								  &select_restrictive_policies);

		add_security_quals(rt_index,
						   select_permissive_policies,
						   select_restrictive_policies,
						   securityQuals,
						   hasSubLinks);
	}

	/*
	 * For INSERT and UPDATE, add withCheckOptions to verify that any new
	 * records added are consistent with the security policies.  This will use
	 * each policy's WITH CHECK clause, or its USING clause if no explicit
	 * WITH CHECK clause is defined.
	 */
	if (commandType == CMD_INSERT || commandType == CMD_UPDATE)
	{
		/* This should be the target relation */
		Assert(rt_index == root->resultRelation);

		add_with_check_options(rel, rt_index,
							   commandType == CMD_INSERT ?
							   WCO_RLS_INSERT_CHECK : WCO_RLS_UPDATE_CHECK,
							   permissive_policies,
							   restrictive_policies,
							   withCheckOptions,
							   hasSubLinks);

		/*
		 * Get and add ALL/SELECT policies, if SELECT rights are required
		 * for this relation (eg: when RETURNING is used).  These are added as
		 * WCO policies rather than security quals to ensure that an error is
		 * raised if a policy is violated; otherwise, we might end up silently
		 * dropping rows to be added.
		 */
		if (rte->requiredPerms & ACL_SELECT)
		{
			List	   *select_permissive_policies = NIL;
			List	   *select_restrictive_policies = NIL;

			get_policies_for_relation(rel, CMD_SELECT, user_id,
									  &select_permissive_policies,
									  &select_restrictive_policies);
			add_with_check_options(rel, rt_index,
								   commandType == CMD_INSERT ?
								   WCO_RLS_INSERT_CHECK : WCO_RLS_UPDATE_CHECK,
								   select_permissive_policies,
								   select_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks);
		}

		/*
		 * For INSERT ... ON CONFLICT DO UPDATE we need additional policy
		 * checks for the UPDATE which may be applied to the same RTE.
		 */
		if (commandType == CMD_INSERT &&
			root->onConflict && root->onConflict->action == ONCONFLICT_UPDATE)
		{
			List	   *conflict_permissive_policies;
			List	   *conflict_restrictive_policies;

			/* Get the policies that apply to the auxiliary UPDATE */
			get_policies_for_relation(rel, CMD_UPDATE, user_id,
									  &conflict_permissive_policies,
									  &conflict_restrictive_policies);

			/*
			 * Enforce the USING clauses of the UPDATE policies using WCOs
			 * rather than security quals.  This ensures that an error is
			 * raised if the conflicting row cannot be updated due to RLS,
			 * rather than the change being silently dropped.
			 */
			add_with_check_options(rel, rt_index,
								   WCO_RLS_CONFLICT_CHECK,
								   conflict_permissive_policies,
								   conflict_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks);

			/*
			 * Get and add ALL/SELECT policies, as WCO_RLS_CONFLICT_CHECK
			 * WCOs to ensure they are considered when taking the UPDATE
			 * path of an INSERT .. ON CONFLICT DO UPDATE, if SELECT
			 * rights are required for this relation, also as WCO policies,
			 * again, to avoid silently dropping data.  See above.
			 */
			if (rte->requiredPerms & ACL_SELECT)
			{
				List	   *conflict_select_permissive_policies = NIL;
				List	   *conflict_select_restrictive_policies = NIL;

				get_policies_for_relation(rel, CMD_SELECT, user_id,
									  &conflict_select_permissive_policies,
									  &conflict_select_restrictive_policies);
				add_with_check_options(rel, rt_index,
									   WCO_RLS_CONFLICT_CHECK,
									   conflict_select_permissive_policies,
									   conflict_select_restrictive_policies,
									   withCheckOptions,
									   hasSubLinks);
			}

			/* Enforce the WITH CHECK clauses of the UPDATE policies */
			add_with_check_options(rel, rt_index,
								   WCO_RLS_UPDATE_CHECK,
								   conflict_permissive_policies,
								   conflict_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks);
		}
	}

	heap_close(rel, NoLock);

	/*
	 * Mark this query as having row security, so plancache can invalidate it
	 * when necessary (eg: role changes)
	 */
	*hasRowSecurity = true;

	return;
}

/*
 * get_policies_for_relation
 *
 * Returns lists of permissive and restrictive policies to be applied to the
 * specified relation, based on the command type and role.
 *
 * This includes any policies added by extensions.
 */
static void
get_policies_for_relation(Relation relation, CmdType cmd, Oid user_id,
						  List **permissive_policies,
						  List **restrictive_policies)
{
	ListCell   *item;

	*permissive_policies = NIL;
	*restrictive_policies = NIL;

	/*
	 * First find all internal policies for the relation.  CREATE POLICY does
	 * not currently support defining restrictive policies, so for now all
	 * internal policies are permissive.
	 */
	foreach(item, relation->rd_rsdesc->policies)
	{
		bool				cmd_matches = false;
		RowSecurityPolicy  *policy = (RowSecurityPolicy *) lfirst(item);

		/* Always add ALL policies, if they exist. */
		if (policy->polcmd == '*')
			cmd_matches = true;
		else
		{
			/* Check whether the policy applies to the specified command type */
			switch (cmd)
			{
				case CMD_SELECT:
					if (policy->polcmd == ACL_SELECT_CHR)
						cmd_matches = true;
					break;
				case CMD_INSERT:
					if (policy->polcmd == ACL_INSERT_CHR)
						cmd_matches = true;
					break;
				case CMD_UPDATE:
					if (policy->polcmd == ACL_UPDATE_CHR)
						cmd_matches = true;
					break;
				case CMD_DELETE:
					if (policy->polcmd == ACL_DELETE_CHR)
						cmd_matches = true;
					break;
				default:
					elog(ERROR, "unrecognized policy command type %d",
						 (int) cmd);
					break;
			}
		}

		/*
		 * Add this policy to the list of permissive policies if it
		 * applies to the specified role.
		 */
		if (cmd_matches && check_role_for_policy(policy->roles, user_id))
			*permissive_policies = lappend(*permissive_policies, policy);
	}

	/*
	 * Then add any permissive or restrictive policies defined by extensions.
	 * These are simply appended to the lists of internal policies, if they
	 * apply to the specified role.
	 */
	if (row_security_policy_hook_restrictive)
	{
		List	   *hook_policies =
			(*row_security_policy_hook_restrictive) (cmd, relation);

		/*
		 * We sort restrictive policies by name so that any WCOs they generate
		 * are checked in a well-defined order.
		 */
		hook_policies = sort_policies_by_name(hook_policies);

		foreach(item, hook_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

			if (check_role_for_policy(policy->roles, user_id))
				*restrictive_policies = lappend(*restrictive_policies, policy);
		}
	}

	if (row_security_policy_hook_permissive)
	{
		List	   *hook_policies =
			(*row_security_policy_hook_permissive) (cmd, relation);

		foreach(item, hook_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

			if (check_role_for_policy(policy->roles, user_id))
				*permissive_policies = lappend(*permissive_policies, policy);
		}
	}
}

/*
 * sort_policies_by_name
 *
 * This is only used for restrictive policies, ensuring that any
 * WithCheckOptions they generate are applied in a well-defined order.
 * This is not necessary for permissive policies, since they are all "OR"d
 * together into a single WithCheckOption check.
 */
static List *
sort_policies_by_name(List *policies)
{
	int			npol = list_length(policies);
	RowSecurityPolicy *pols;
	ListCell   *item;
	int			ii = 0;

	if (npol <= 1)
		return policies;

	pols = (RowSecurityPolicy *) palloc(sizeof(RowSecurityPolicy) * npol);

	foreach(item, policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
		pols[ii++] = *policy;
	}

	qsort(pols, npol, sizeof(RowSecurityPolicy), row_security_policy_cmp);

	policies = NIL;
	for (ii = 0; ii < npol; ii++)
		policies = lappend(policies, &pols[ii]);

	return policies;
}

/*
 * qsort comparator to sort RowSecurityPolicy entries by name
 */
static int
row_security_policy_cmp(const void *a, const void *b)
{
	const RowSecurityPolicy *pa = (const RowSecurityPolicy *) a;
	const RowSecurityPolicy *pb = (const RowSecurityPolicy *) b;

	/* Guard against NULL policy names from extensions */
	if (pa->policy_name == NULL)
		return pb->policy_name == NULL ? 0 : 1;
	if (pb->policy_name == NULL)
		return -1;

	return strcmp(pa->policy_name, pb->policy_name);
}

/*
 * add_security_quals
 *
 * Add security quals to enforce the specified RLS policies, restricting
 * access to existing data in a table.  If there are no policies controlling
 * access to the table, then all access is prohibited --- i.e., an implicit
 * default-deny policy is used.
 *
 * New security quals are added to securityQuals, and hasSubLinks is set to
 * true if any of the quals added contain sublink subqueries.
 */
static void
add_security_quals(int rt_index,
				   List *permissive_policies,
				   List *restrictive_policies,
				   List **securityQuals,
				   bool *hasSubLinks)
{
	ListCell   *item;
	List	   *permissive_quals = NIL;
	Expr	   *rowsec_expr;

	/*
	 * First collect up the permissive quals.  If we do not find any permissive
	 * policies then no rows are visible (this is handled below).
	 */
	foreach(item, permissive_policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

		if (policy->qual != NULL)
		{
			permissive_quals = lappend(permissive_quals,
									   copyObject(policy->qual));
			*hasSubLinks |= policy->hassublinks;
		}
	}

	/*
	 * We must have permissive quals, always, or no rows are visible.
	 *
	 * If we do not, then we simply return a single 'false' qual which results
	 * in no rows being visible.
	 */
	if (permissive_quals != NIL)
	{
		/*
		 * We now know that permissive policies exist, so we can now add
		 * security quals based on the USING clauses from the restrictive
		 * policies.  Since these need to be "AND"d together, we can
		 * just add them one at a time.
		 */
		foreach(item, restrictive_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
			Expr	   *qual;

			if (policy->qual != NULL)
			{
				qual = copyObject(policy->qual);
				ChangeVarNodes((Node *) qual, 1, rt_index, 0);

				*securityQuals = list_append_unique(*securityQuals, qual);
				*hasSubLinks |= policy->hassublinks;
			}
		}

		/*
		 * Then add a single security qual "OR"ing together the USING clauses
		 * from all the permissive policies.
		 */
		if (list_length(permissive_quals) == 1)
			rowsec_expr = (Expr *) linitial(permissive_quals);
		else
			rowsec_expr = makeBoolExpr(OR_EXPR, permissive_quals, -1);

		ChangeVarNodes((Node *) rowsec_expr, 1, rt_index, 0);
		*securityQuals = list_append_unique(*securityQuals, rowsec_expr);
	}
	else
		/*
		 * A permissive policy must exist for rows to be visible at all.
		 * Therefore, if there were no permissive policies found, return a
		 * single always-false clause.
		 */
		*securityQuals = lappend(*securityQuals,
								 makeConst(BOOLOID, -1, InvalidOid,
										   sizeof(bool), BoolGetDatum(false),
										   false, true));
}

/*
 * add_with_check_options
 *
 * Add WithCheckOptions of the specified kind to check that new records
 * added by an INSERT or UPDATE are consistent with the specified RLS
 * policies.  Normally new data must satisfy the WITH CHECK clauses from the
 * policies.  If a policy has no explicit WITH CHECK clause, its USING clause
 * is used instead.  In the special case of an UPDATE arising from an
 * INSERT ... ON CONFLICT DO UPDATE, existing records are first checked using
 * a WCO_RLS_CONFLICT_CHECK WithCheckOption, which always uses the USING
 * clauses from RLS policies.
 *
 * New WCOs are added to withCheckOptions, and hasSubLinks is set to true if
 * any of the check clauses added contain sublink subqueries.
 */
static void
add_with_check_options(Relation rel,
					   int rt_index,
					   WCOKind kind,
					   List *permissive_policies,
					   List *restrictive_policies,
					   List **withCheckOptions,
					   bool *hasSubLinks)
{
	ListCell   *item;
	List	   *permissive_quals = NIL;

#define QUAL_FOR_WCO(policy) \
	( kind != WCO_RLS_CONFLICT_CHECK &&	\
	  (policy)->with_check_qual != NULL ? \
	  (policy)->with_check_qual : (policy)->qual )

	/*
	 * First collect up the permissive policy clauses, similar to
	 * add_security_quals.
	 */
	foreach(item, permissive_policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
		Expr	   *qual = QUAL_FOR_WCO(policy);

		if (qual != NULL)
		{
			permissive_quals = lappend(permissive_quals, copyObject(qual));
			*hasSubLinks |= policy->hassublinks;
		}
	}

	/*
	 * There must be at least one permissive qual found or no rows are
	 * allowed to be added.  This is the same as in add_security_quals.
	 *
	 * If there are no permissive_quals then we fall through and return a single
	 * 'false' WCO, preventing all new rows.
	 */
	if (permissive_quals != NIL)
	{
		/*
		 * Add a single WithCheckOption for all the permissive policy clauses
		 * "OR"d together.  This check has no policy name, since if the check
		 * fails it means that no policy granted permission to perform the
		 * update, rather than any particular policy being violated.
		 */
		WithCheckOption *wco;

		wco = (WithCheckOption *) makeNode(WithCheckOption);
		wco->kind = kind;
		wco->relname = pstrdup(RelationGetRelationName(rel));
		wco->polname = NULL;
		wco->cascaded = false;

		if (list_length(permissive_quals) == 1)
			wco->qual = (Node *) linitial(permissive_quals);
		else
			wco->qual = (Node *) makeBoolExpr(OR_EXPR, permissive_quals, -1);

		ChangeVarNodes(wco->qual, 1, rt_index, 0);

		*withCheckOptions = list_append_unique(*withCheckOptions, wco);

		/*
		 * Now add WithCheckOptions for each of the restrictive policy clauses
		 * (which will be "AND"d together).  We use a separate WithCheckOption
		 * for each restrictive policy to allow the policy name to be included
		 * in error reports if the policy is violated.
		 */
		foreach(item, restrictive_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
			Expr	   *qual = QUAL_FOR_WCO(policy);
			WithCheckOption *wco;

			if (qual != NULL)
			{
				qual = copyObject(qual);
				ChangeVarNodes((Node *) qual, 1, rt_index, 0);

				wco = (WithCheckOption *) makeNode(WithCheckOption);
				wco->kind = kind;
				wco->relname = pstrdup(RelationGetRelationName(rel));
				wco->polname = pstrdup(policy->policy_name);
				wco->qual = (Node *) qual;
				wco->cascaded = false;

				*withCheckOptions = list_append_unique(*withCheckOptions, wco);
				*hasSubLinks |= policy->hassublinks;
			}
		}
	}
	else
	{
		/*
		 * If there were no policy clauses to check new data, add a single
		 * always-false WCO (a default-deny policy).
		 */
		WithCheckOption *wco;

		wco = (WithCheckOption *) makeNode(WithCheckOption);
		wco->kind = kind;
		wco->relname = pstrdup(RelationGetRelationName(rel));
		wco->polname = NULL;
		wco->qual = (Node *) makeConst(BOOLOID, -1, InvalidOid,
									   sizeof(bool), BoolGetDatum(false),
									   false, true);
		wco->cascaded = false;

		*withCheckOptions = lappend(*withCheckOptions, wco);
	}
}

/*
 * check_role_for_policy -
 *	 determines if the policy should be applied for the current role
 */
static bool
check_role_for_policy(ArrayType *policy_roles, Oid user_id)
{
	int			i;
	Oid		   *roles = (Oid *) ARR_DATA_PTR(policy_roles);

	/* Quick fall-thru for policies applied to all roles */
	if (roles[0] == ACL_ID_PUBLIC)
		return true;

	for (i = 0; i < ARR_DIMS(policy_roles)[0]; i++)
	{
		if (has_privs_of_role(user_id, roles[i]))
			return true;
	}

	return false;
}
