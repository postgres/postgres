/*
 * rewrite/rowsecurity.c
 *    Routines to support policies for row level security (aka RLS).
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
 * queries, this is handled by calling prepend_row_security_policies() during
 * rewrite, which looks at each RTE and adds the expressions defined by the
 * policies to the securityQuals list for the RTE.  For queries which modify
 * the relation, any WITH CHECK policies are added to the list of
 * WithCheckOptions for the Query and checked against each row which is being
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
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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

static List *pull_row_security_policies(CmdType cmd, Relation relation,
										Oid user_id);
static void process_policies(Query* root, List *policies, int rt_index,
							 Expr **final_qual,
							 Expr **final_with_check_qual,
							 bool *hassublinks,
							 BoolExprType boolop);
static bool check_role_for_policy(ArrayType *policy_roles, Oid user_id);

/*
 * hooks to allow extensions to add their own security policies
 *
 * row_security_policy_hook_permissive can be used to add policies which
 * are included in the "OR"d set of policies.
 *
 * row_security_policy_hook_restrictive can be used to add policies which
 * are enforced, regardless of other policies (they are "AND"d).
 *
 * See below where the hook is called in prepend_row_security_policies for
 * insight into how to use this hook.
 */
row_security_policy_hook_type	row_security_policy_hook_permissive = NULL;
row_security_policy_hook_type	row_security_policy_hook_restrictive = NULL;

/*
 * Get any row security quals and check quals that should be applied to the
 * specified RTE.
 *
 * In addition, hasRowSecurity is set to true if row level security is enabled
 * (even if this RTE doesn't have any row security quals), and hasSubLinks is
 * set to true if any of the quals returned contain sublinks.
 */
void
get_row_security_policies(Query* root, RangeTblEntry* rte, int rt_index,
						  List **securityQuals, List **withCheckOptions,
						  bool *hasRowSecurity, bool *hasSubLinks)
{
	Expr			   *rowsec_expr = NULL;
	Expr			   *rowsec_with_check_expr = NULL;
	Expr			   *hook_expr_restrictive = NULL;
	Expr			   *hook_with_check_expr_restrictive = NULL;
	Expr			   *hook_expr_permissive = NULL;
	Expr			   *hook_with_check_expr_permissive = NULL;

	List			   *rowsec_policies;
	List			   *hook_policies_restrictive = NIL;
	List			   *hook_policies_permissive = NIL;

	Relation 			rel;
	Oid					user_id;
	int					sec_context;
	int					rls_status;
	bool				defaultDeny = false;

	/* Defaults for the return values */
	*securityQuals = NIL;
	*withCheckOptions = NIL;
	*hasRowSecurity = false;
	*hasSubLinks = false;

	/* This is just to get the security context */
	GetUserIdAndSecContext(&user_id, &sec_context);

	/* Switch to checkAsUser if it's set */
	user_id = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/*
	 * If this is not a normal relation, or we have been told
	 * to explicitly skip RLS (perhaps because this is an FK check)
	 * then just return immediately.
	 */
	if (rte->relid < FirstNormalObjectId
		|| rte->relkind != RELKIND_RELATION
		|| (sec_context & SECURITY_ROW_LEVEL_DISABLED))
		return;

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
		 * Indicate that this query may involve RLS and must therefore
		 * be replanned if the environment changes (GUCs, role), but we
		 * are not adding anything here.
		 */
		*hasRowSecurity = true;

		return;
	}

	/* Grab the built-in policies which should be applied to this relation. */
	rel = heap_open(rte->relid, NoLock);

	rowsec_policies = pull_row_security_policies(root->commandType, rel,
												 user_id);

	/*
	 * Check if this is only the default-deny policy.
	 *
	 * Normally, if the table has row security enabled but there are
	 * no policies, we use a default-deny policy and not allow anything.
	 * However, when an extension uses the hook to add their own
	 * policies, we don't want to include the default deny policy or
	 * there won't be any way for a user to use an extension exclusively
	 * for the policies to be used.
	 */
	if (((RowSecurityPolicy *) linitial(rowsec_policies))->policy_id
			== InvalidOid)
		defaultDeny = true;

	/* Now that we have our policies, build the expressions from them. */
	process_policies(root, rowsec_policies, rt_index, &rowsec_expr,
					 &rowsec_with_check_expr, hasSubLinks, OR_EXPR);

	/*
	 * Also, allow extensions to add their own policies.
	 *
	 * extensions can add either permissive or restrictive policies.
	 *
	 * Note that, as with the internal policies, if multiple policies are
	 * returned then they will be combined into a single expression with
	 * all of them OR'd (for permissive) or AND'd (for restrictive) together.
	 *
	 * If only a USING policy is returned by the extension then it will be
	 * used for WITH CHECK as well, similar to how internal policies are
	 * handled.
	 *
	 * The only caveat to this is that if there are NO internal policies
	 * defined, there ARE policies returned by the extension, and RLS is
	 * enabled on the table, then we will ignore the internally-generated
	 * default-deny policy and use only the policies returned by the
	 * extension.
	 */
	if (row_security_policy_hook_restrictive)
	{
		hook_policies_restrictive = (*row_security_policy_hook_restrictive)(root->commandType, rel);

		/* Build the expression from any policies returned. */
		if (hook_policies_restrictive != NIL)
			process_policies(root, hook_policies_restrictive, rt_index,
							 &hook_expr_restrictive,
							 &hook_with_check_expr_restrictive,
							 hasSubLinks,
							 AND_EXPR);
	}

	if (row_security_policy_hook_permissive)
	{
		hook_policies_permissive = (*row_security_policy_hook_permissive)(root->commandType, rel);

		/* Build the expression from any policies returned. */
		if (hook_policies_permissive != NIL)
			process_policies(root, hook_policies_permissive, rt_index,
							 &hook_expr_permissive,
							 &hook_with_check_expr_permissive, hasSubLinks,
							 OR_EXPR);
	}

	/*
	 * If the only built-in policy is the default-deny one, and hook
	 * policies exist, then use the hook policies only and do not apply
	 * the default-deny policy.  Otherwise, we will apply both sets below.
	 */
	if (defaultDeny &&
		(hook_policies_restrictive != NIL || hook_policies_permissive != NIL))
	{
		rowsec_expr = NULL;
		rowsec_with_check_expr = NULL;
	}

	/*
	 * For INSERT or UPDATE, we need to add the WITH CHECK quals to
	 * Query's withCheckOptions to verify that any new records pass the
	 * WITH CHECK policy (this will be a copy of the USING policy, if no
	 * explicit WITH CHECK policy exists).
	 */
	if (root->commandType == CMD_INSERT || root->commandType == CMD_UPDATE)
	{
		/*
		 * WITH CHECK OPTIONS wants a WCO node which wraps each Expr, so
		 * create them as necessary.
		 */

		/*
		 * Handle any restrictive policies first.
		 *
		 * They can simply be added.
		 */
		if (hook_with_check_expr_restrictive)
		{
			WithCheckOption	   *wco;

			wco = (WithCheckOption *) makeNode(WithCheckOption);
			wco->viewname = pstrdup(RelationGetRelationName(rel));
			wco->qual = (Node *) hook_with_check_expr_restrictive;
			wco->cascaded = false;
			*withCheckOptions = lappend(*withCheckOptions, wco);
		}

		/*
		 * Handle built-in policies, if there are no permissive
		 * policies from the hook.
		 */
		if (rowsec_with_check_expr && !hook_with_check_expr_permissive)
		{
			WithCheckOption	   *wco;

			wco = (WithCheckOption *) makeNode(WithCheckOption);
			wco->viewname = pstrdup(RelationGetRelationName(rel));
			wco->qual = (Node *) rowsec_with_check_expr;
			wco->cascaded = false;
			*withCheckOptions = lappend(*withCheckOptions, wco);
		}
		/* Handle the hook policies, if there are no built-in ones. */
		else if (!rowsec_with_check_expr && hook_with_check_expr_permissive)
		{
			WithCheckOption	   *wco;

			wco = (WithCheckOption *) makeNode(WithCheckOption);
			wco->viewname = pstrdup(RelationGetRelationName(rel));
			wco->qual = (Node *) hook_with_check_expr_permissive;
			wco->cascaded = false;
			*withCheckOptions = lappend(*withCheckOptions, wco);
		}
		/* Handle the case where there are both. */
		else if (rowsec_with_check_expr && hook_with_check_expr_permissive)
		{
			WithCheckOption	   *wco;
			List			   *combined_quals = NIL;
			Expr			   *combined_qual_eval;

			combined_quals = lcons(copyObject(rowsec_with_check_expr), combined_quals);
			combined_quals = lcons(copyObject(hook_with_check_expr_permissive), combined_quals);

			combined_qual_eval = makeBoolExpr(OR_EXPR, combined_quals, -1);

			wco = (WithCheckOption *) makeNode(WithCheckOption);
			wco->viewname = pstrdup(RelationGetRelationName(rel));
			wco->qual = (Node *) combined_qual_eval;
			wco->cascaded = false;
			*withCheckOptions = lappend(*withCheckOptions, wco);
		}
	}

	/* For SELECT, UPDATE, and DELETE, set the security quals */
	if (root->commandType == CMD_SELECT
		|| root->commandType == CMD_UPDATE
		|| root->commandType == CMD_DELETE)
	{
		/* restrictive policies can simply be added to the list first */
		if (hook_expr_restrictive)
			*securityQuals = lappend(*securityQuals, hook_expr_restrictive);

		/* If we only have internal permissive, then just add those */
		if (rowsec_expr && !hook_expr_permissive)
			*securityQuals = lappend(*securityQuals, rowsec_expr);
		/* .. and if we have only permissive policies from the hook */
		else if (!rowsec_expr && hook_expr_permissive)
			*securityQuals = lappend(*securityQuals, hook_expr_permissive);
		/* if we have both, we have to combine them with an OR */
		else if (rowsec_expr && hook_expr_permissive)
		{
			List   *combined_quals = NIL;
			Expr   *combined_qual_eval;

			combined_quals = lcons(copyObject(rowsec_expr), combined_quals);
			combined_quals = lcons(copyObject(hook_expr_permissive), combined_quals);

			combined_qual_eval = makeBoolExpr(OR_EXPR, combined_quals, -1);

			*securityQuals = lappend(*securityQuals, combined_qual_eval);
		}
	}

	heap_close(rel, NoLock);

	/*
	 * Mark this query as having row security, so plancache can invalidate
	 * it when necessary (eg: role changes)
	 */
	*hasRowSecurity = true;

	return;
}

/*
 * pull_row_security_policies
 *
 * Returns the list of policies to be added for this relation, based on the
 * type of command and the roles to which it applies, from the relation cache.
 *
 */
static List *
pull_row_security_policies(CmdType cmd, Relation relation, Oid user_id)
{
	List			   *policies = NIL;
	ListCell		   *item;

	/*
	 * Row security is enabled for the relation and the row security GUC is
	 * either 'on' or 'force' here, so find the policies to apply to the table.
	 * There must always be at least one policy defined (may be the simple
	 * 'default-deny' policy, if none are explicitly defined on the table).
	 */
	foreach(item, relation->rd_rsdesc->policies)
	{
		RowSecurityPolicy  *policy = (RowSecurityPolicy *) lfirst(item);

		/* Always add ALL policies, if they exist. */
		if (policy->polcmd == '*' &&
				check_role_for_policy(policy->roles, user_id))
			policies = lcons(policy, policies);

		/* Add relevant command-specific policies to the list. */
		switch(cmd)
		{
			case CMD_SELECT:
				if (policy->polcmd == ACL_SELECT_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			case CMD_INSERT:
				/* If INSERT then only need to add the WITH CHECK qual */
				if (policy->polcmd == ACL_INSERT_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			case CMD_UPDATE:
				if (policy->polcmd == ACL_UPDATE_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			case CMD_DELETE:
				if (policy->polcmd == ACL_DELETE_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			default:
				elog(ERROR, "unrecognized policy command type %d", (int) cmd);
				break;
		}
	}

	/*
	 * There should always be a policy applied.  If there are none found then
	 * create a simply defauly-deny policy (might be that policies exist but
	 * that none of them apply to the role which is querying the table).
	 */
	if (policies == NIL)
	{
		RowSecurityPolicy  *policy = NULL;
		Datum               role;

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

		policies = list_make1(policy);
	}

	Assert(policies != NIL);

	return policies;
}

/*
 * process_policies
 *
 * This will step through the policies which are passed in (which would come
 * from either the built-in ones created on a table, or from policies provided
 * by an extension through the hook provided), work out how to combine them,
 * rewrite them as necessary, and produce an Expr for the normal security
 * quals and an Expr for the with check quals.
 *
 * qual_eval, with_check_eval, and hassublinks are output variables
 */
static void
process_policies(Query* root, List *policies, int rt_index, Expr **qual_eval,
				 Expr **with_check_eval, bool *hassublinks,
				 BoolExprType boolop)
{
	ListCell		   *item;
	List			   *quals = NIL;
	List			   *with_check_quals = NIL;

	/*
	 * Extract the USING and WITH CHECK quals from each of the policies
	 * and add them to our lists.  We only want WITH CHECK quals if this
	 * RTE is the query's result relation.
	 */
	foreach(item, policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

		if (policy->qual != NULL)
			quals = lcons(copyObject(policy->qual), quals);

		if (policy->with_check_qual != NULL &&
			rt_index == root->resultRelation)
			with_check_quals = lcons(copyObject(policy->with_check_qual),
									 with_check_quals);

		/*
		 * For each policy, if there is only a USING clause then copy/use it for
		 * the WITH CHECK policy also, if this RTE is the query's result
		 * relation.
		 */
		if (policy->qual != NULL && policy->with_check_qual == NULL &&
			rt_index == root->resultRelation)
			with_check_quals = lcons(copyObject(policy->qual),
									 with_check_quals);


		if (policy->hassublinks)
			*hassublinks = true;
	}

	/*
	 * If we end up without any normal quals (perhaps the only policy matched
	 * was for INSERT), then create a single all-false one.
	 */
	if (quals == NIL)
		quals = lcons(makeConst(BOOLOID, -1, InvalidOid, sizeof(bool),
								BoolGetDatum(false), false, true), quals);

	/*
	 * Row security quals always have the target table as varno 1, as no
	 * joins are permitted in row security expressions. We must walk the
	 * expression, updating any references to varno 1 to the varno
	 * the table has in the outer query.
	 *
	 * We rewrite the expression in-place.
	 *
	 * We must have some quals at this point; the default-deny policy, if
	 * nothing else.  Note that we might not have any WITH CHECK quals-
	 * that's fine, as this might not be the resultRelation.
	 */
	Assert(quals != NIL);

	ChangeVarNodes((Node *) quals, 1, rt_index, 0);

	if (with_check_quals != NIL)
		ChangeVarNodes((Node *) with_check_quals, 1, rt_index, 0);

	/*
	 * If more than one security qual is returned, then they need to be
	 * combined together.
	 */
	if (list_length(quals) > 1)
		*qual_eval = makeBoolExpr(boolop, quals, -1);
	else
		*qual_eval = (Expr*) linitial(quals);

	/*
	 * Similairly, if more than one WITH CHECK qual is returned, then
	 * they need to be combined together.
	 *
	 * with_check_quals is allowed to be NIL here since this might not be the
	 * resultRelation (see above).
	 */
	if (list_length(with_check_quals) > 1)
		*with_check_eval = makeBoolExpr(boolop, with_check_quals, -1);
	else if (with_check_quals != NIL)
		*with_check_eval = (Expr*) linitial(with_check_quals);
	else
		*with_check_eval = NULL;

	return;
}

/*
 * check_role_for_policy -
 *   determines if the policy should be applied for the current role
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
