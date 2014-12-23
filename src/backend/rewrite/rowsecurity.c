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
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
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
#include "utils/syscache.h"
#include "tcop/utility.h"

static List *pull_row_security_policies(CmdType cmd, Relation relation,
										Oid user_id);
static void process_policies(List *policies, int rt_index,
							 Expr **final_qual,
							 Expr **final_with_check_qual,
							 bool *hassublinks);
static bool check_role_for_policy(ArrayType *policy_roles, Oid user_id);

/*
 * hook to allow extensions to apply their own security policy
 *
 * See below where the hook is called in prepend_row_security_policies for
 * insight into how to use this hook.
 */
row_security_policy_hook_type	row_security_policy_hook = NULL;

/*
 * Check the given RTE to see whether it's already had row security quals
 * expanded and, if not, prepend any row security rules from built-in or
 * plug-in sources to the securityQuals. The security quals are rewritten (for
 * view expansion, etc) before being added to the RTE.
 *
 * Returns true if any quals were added. Note that quals may have been found
 * but not added if user rights make the user exempt from row security.
 */
bool
prepend_row_security_policies(Query* root, RangeTblEntry* rte, int rt_index)
{
	Expr			   *rowsec_expr = NULL;
	Expr			   *rowsec_with_check_expr = NULL;
	Expr			   *hook_expr = NULL;
	Expr			   *hook_with_check_expr = NULL;

	List			   *rowsec_policies;
	List			   *hook_policies = NIL;

	Relation 			rel;
	Oid					user_id;
	int					sec_context;
	int					rls_status;
	bool				defaultDeny = true;
	bool				hassublinks = false;

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
		return false;

	/* Determine the state of RLS for this, pass checkAsUser explicitly */
	rls_status = check_enable_rls(rte->relid, rte->checkAsUser);

	/* If there is no RLS on this table at all, nothing to do */
	if (rls_status == RLS_NONE)
		return false;

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
		root->hasRowSecurity = true;

		return false;
	}

	/*
	 * We may end up getting called multiple times for the same RTE, so check
	 * to make sure we aren't doing double-work.
	 */
	if (rte->securityQuals != NIL)
		return false;

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
	process_policies(rowsec_policies, rt_index, &rowsec_expr,
					 &rowsec_with_check_expr, &hassublinks);

	/*
	 * Also, allow extensions to add their own policies.
	 *
	 * Note that, as with the internal policies, if multiple policies are
	 * returned then they will be combined into a single expression with
	 * all of them OR'd together.  However, to avoid the situation of an
	 * extension granting more access to a table than the internal policies
	 * would allow, the extension's policies are AND'd with the internal
	 * policies.  In other words - extensions can only provide further
	 * filtering of the result set (or further reduce the set of records
	 * allowed to be added).
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
	if (row_security_policy_hook)
	{
		hook_policies = (*row_security_policy_hook)(root->commandType, rel);

		/* Build the expression from any policies returned. */
		process_policies(hook_policies, rt_index, &hook_expr,
						 &hook_with_check_expr, &hassublinks);
	}

	/*
	 * If the only built-in policy is the default-deny one, and hook
	 * policies exist, then use the hook policies only and do not apply
	 * the default-deny policy.  Otherwise, apply both sets (AND'd
	 * together).
	 */
	if (defaultDeny && hook_policies != NIL)
		rowsec_expr = NULL;

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
		if (rowsec_with_check_expr)
		{
			WithCheckOption	   *wco;

			wco = (WithCheckOption *) makeNode(WithCheckOption);
			wco->viewname = RelationGetRelationName(rel);
			wco->qual = (Node *) rowsec_with_check_expr;
			wco->cascaded = false;
			root->withCheckOptions = lcons(wco, root->withCheckOptions);
		}

		/*
		 * Ditto for the expression, if any, returned from the extension.
		 */
		if (hook_with_check_expr)
		{
			WithCheckOption	   *wco;

			wco = (WithCheckOption *) makeNode(WithCheckOption);
			wco->viewname = RelationGetRelationName(rel);
			wco->qual = (Node *) hook_with_check_expr;
			wco->cascaded = false;
			root->withCheckOptions = lcons(wco, root->withCheckOptions);
		}
	}

	/* For SELECT, UPDATE, and DELETE, set the security quals */
	if (root->commandType == CMD_SELECT
		|| root->commandType == CMD_UPDATE
		|| root->commandType == CMD_DELETE)
	{
		if (rowsec_expr)
			rte->securityQuals = lcons(rowsec_expr, rte->securityQuals);

		if (hook_expr)
			rte->securityQuals = lcons(hook_expr,
									   rte->securityQuals);
	}

	heap_close(rel, NoLock);

	/*
	 * Mark this query as having row security, so plancache can invalidate
	 * it when necessary (eg: role changes)
	 */
	root->hasRowSecurity = true;

	/*
	 * If we have sublinks added because of the policies being added to the
	 * query, then set hasSubLinks on the Query to force subLinks to be
	 * properly expanded.
	 */
	if (hassublinks)
		root->hasSubLinks = hassublinks;

	/* If we got this far, we must have added quals */
	return true;
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
	RowSecurityPolicy  *policy;

	/*
	 * Row security is enabled for the relation and the row security GUC is
	 * either 'on' or 'force' here, so find the policies to apply to the table.
	 * There must always be at least one policy defined (may be the simple
	 * 'default-deny' policy, if none are explicitly defined on the table).
	 */
	foreach(item, relation->rd_rsdesc->policies)
	{
		policy = (RowSecurityPolicy *) lfirst(item);

		/* Always add ALL policies, if they exist. */
		if (policy->cmd == '\0' &&
				check_role_for_policy(policy->roles, user_id))
			policies = lcons(policy, policies);

		/* Build the list of policies to return. */
		switch(cmd)
		{
			case CMD_SELECT:
				if (policy->cmd == ACL_SELECT_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			case CMD_INSERT:
				/* If INSERT then only need to add the WITH CHECK qual */
				if (policy->cmd == ACL_INSERT_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			case CMD_UPDATE:
				if (policy->cmd == ACL_UPDATE_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			case CMD_DELETE:
				if (policy->cmd == ACL_DELETE_CHR
					&& check_role_for_policy(policy->roles, user_id))
					policies = lcons(policy, policies);
				break;
			default:
				elog(ERROR, "unrecognized command type.");
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
		policy->cmd = '\0';
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
process_policies(List *policies, int rt_index, Expr **qual_eval,
				 Expr **with_check_eval, bool *hassublinks)
{
	ListCell		   *item;
	List			   *quals = NIL;
	List			   *with_check_quals = NIL;

	/*
	 * Extract the USING and WITH CHECK quals from each of the policies
	 * and add them to our lists.
	 */
	foreach(item, policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

		if (policy->qual != NULL)
			quals = lcons(copyObject(policy->qual), quals);

		if (policy->with_check_qual != NULL)
			with_check_quals = lcons(copyObject(policy->with_check_qual),
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
	 * If we end up with only USING quals, then use those as
	 * WITH CHECK quals also.
	 */
	if (with_check_quals == NIL)
		with_check_quals = copyObject(quals);

	/*
	 * Row security quals always have the target table as varno 1, as no
	 * joins are permitted in row security expressions. We must walk the
	 * expression, updating any references to varno 1 to the varno
	 * the table has in the outer query.
	 *
	 * We rewrite the expression in-place.
	 */
	ChangeVarNodes((Node *) quals, 1, rt_index, 0);
	ChangeVarNodes((Node *) with_check_quals, 1, rt_index, 0);

	/*
	 * If more than one security qual is returned, then they need to be
	 * OR'ed together.
	 */
	if (list_length(quals) > 1)
		*qual_eval = makeBoolExpr(OR_EXPR, quals, -1);
	else
		*qual_eval = (Expr*) linitial(quals);

	/*
	 * If more than one WITH CHECK qual is returned, then they need to
	 * be OR'ed together.
	 */
	if (list_length(with_check_quals) > 1)
		*with_check_eval = makeBoolExpr(OR_EXPR, with_check_quals, -1);
	else
		*with_check_eval = (Expr*) linitial(with_check_quals);

	return;
}

/*
 * check_enable_rls
 *
 * Determine, based on the relation, row_security setting, and current role,
 * if RLS is applicable to this query.  RLS_NONE_ENV indicates that, while
 * RLS is not to be added for this query, a change in the environment may change
 * that.  RLS_NONE means that RLS is not on the relation at all and therefore
 * we don't need to worry about it.  RLS_ENABLED means RLS should be implemented
 * for the table and the plan cache needs to be invalidated if the environment
 * changes.
 *
 * Handle checking as another role via checkAsUser (for views, etc).
 */
int
check_enable_rls(Oid relid, Oid checkAsUser)
{
	HeapTuple		tuple;
	Form_pg_class	classform;
	bool			relrowsecurity;
	Oid				user_id = checkAsUser ? checkAsUser : GetUserId();

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return RLS_NONE;

	classform = (Form_pg_class) GETSTRUCT(tuple);

	relrowsecurity = classform->relrowsecurity;

	ReleaseSysCache(tuple);

	/* Nothing to do if the relation does not have RLS */
	if (!relrowsecurity)
		return RLS_NONE;

	/*
	 * Check permissions
	 *
	 * If the relation has row level security enabled and the row_security GUC
	 * is off, then check if the user has rights to bypass RLS for this
	 * relation.  Table owners can always bypass, as can any role with the
	 * BYPASSRLS capability.
	 *
	 * If the role is the table owner, then we bypass RLS unless row_security
	 * is set to 'force'.  Note that superuser is always considered an owner.
	 *
	 * Return RLS_NONE_ENV to indicate that this decision depends on the
	 * environment (in this case, what the current values of user_id and
	 * row_security are).
	 */
	if (row_security != ROW_SECURITY_FORCE
		&& (pg_class_ownercheck(relid, user_id)))
		return RLS_NONE_ENV;

	/*
	 * If the row_security GUC is 'off' then check if the user has permission
	 * to bypass it.  Note that we have already handled the case where the user
	 * is the table owner above.
	 *
	 * Note that row_security is always considered 'on' when querying
	 * through a view or other cases where checkAsUser is true, so skip this
	 * if checkAsUser is in use.
	 */
	if (!checkAsUser && row_security == ROW_SECURITY_OFF)
	{
		if (has_role_attribute(user_id, ROLE_ATTR_BYPASSRLS))
			/* OK to bypass */
			return RLS_NONE_ENV;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("insufficient privilege to bypass row security.")));
	}

	/* RLS should be fully enabled for this relation. */
	return RLS_ENABLED;
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
