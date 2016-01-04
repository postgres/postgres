/*-------------------------------------------------------------------------
 *
 * rls.c
 *		  RLS-related utility functions.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/utils/misc/rls.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/syscache.h"


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
 * Handle checking as another role via checkAsUser (for views, etc).  Pass
 * InvalidOid to check the current user.
 *
 * If noError is set to 'true' then we just return RLS_ENABLED instead of doing
 * an ereport() if the user has attempted to bypass RLS and they are not
 * allowed to.  This allows users to check if RLS is enabled without having to
 * deal with the actual error case (eg: error cases which are trying to decide
 * if the user should get data from the relation back as part of the error).
 */
int
check_enable_rls(Oid relid, Oid checkAsUser, bool noError)
{
	Oid			user_id = checkAsUser ? checkAsUser : GetUserId();
	HeapTuple	tuple;
	Form_pg_class classform;
	bool		relrowsecurity;
	bool		relforcerowsecurity;
	bool		amowner;

	/* Nothing to do for built-in relations */
	if (relid < (Oid) FirstNormalObjectId)
		return RLS_NONE;

	/* Fetch relation's relrowsecurity and relforcerowsecurity flags */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return RLS_NONE;
	classform = (Form_pg_class) GETSTRUCT(tuple);

	relrowsecurity = classform->relrowsecurity;
	relforcerowsecurity = classform->relforcerowsecurity;

	ReleaseSysCache(tuple);

	/* Nothing to do if the relation does not have RLS */
	if (!relrowsecurity)
		return RLS_NONE;

	/*
	 * BYPASSRLS users always bypass RLS.  Note that superusers are always
	 * considered to have BYPASSRLS.
	 *
	 * Return RLS_NONE_ENV to indicate that this decision depends on the
	 * environment (in this case, the user_id).
	 */
	if (has_bypassrls_privilege(user_id))
		return RLS_NONE_ENV;

	/*
	 * Table owners generally bypass RLS, except if the table has been set (by
	 * an owner) to FORCE ROW SECURITY, and this is not a referential
	 * integrity check.
	 *
	 * Return RLS_NONE_ENV to indicate that this decision depends on the
	 * environment (in this case, the user_id).
	 */
	amowner = pg_class_ownercheck(relid, user_id);
	if (amowner)
	{
		/*
		 * If FORCE ROW LEVEL SECURITY has been set on the relation then we
		 * should return RLS_ENABLED to indicate that RLS should be applied.
		 * If not, or if we are in an InNoForceRLSOperation context, we return
		 * RLS_NONE_ENV.
		 *
		 * InNoForceRLSOperation indicates that we should not apply RLS even
		 * if the table has FORCE RLS set - IF the current user is the owner.
		 * This is specifically to ensure that referential integrity checks
		 * are able to still run correctly.
		 *
		 * This is intentionally only done after we have checked that the user
		 * is the table owner, which should always be the case for referential
		 * integrity checks.
		 */
		if (!relforcerowsecurity || InNoForceRLSOperation())
			return RLS_NONE_ENV;
	}

	/*
	 * We should apply RLS.  However, the user may turn off the row_security
	 * GUC to get a forced error instead.
	 */
	if (!row_security && !noError)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("query would be affected by row-level security policy for table \"%s\"",
						get_rel_name(relid)),
				 amowner ? errhint("To disable the policy for the table's owner, use ALTER TABLE NO FORCE ROW LEVEL SECURITY.") : 0));

	/* RLS should be fully enabled for this relation. */
	return RLS_ENABLED;
}

/*
 * row_security_active
 *
 * check_enable_rls wrapped as a SQL callable function except
 * RLS_NONE_ENV and RLS_NONE are the same for this purpose.
 */
Datum
row_security_active(PG_FUNCTION_ARGS)
{
	/* By OID */
	Oid			tableoid = PG_GETARG_OID(0);
	int			rls_status;

	rls_status = check_enable_rls(tableoid, InvalidOid, true);
	PG_RETURN_BOOL(rls_status == RLS_ENABLED);
}

Datum
row_security_active_name(PG_FUNCTION_ARGS)
{
	/* By qualified name */
	text	   *tablename = PG_GETARG_TEXT_P(0);
	RangeVar   *tablerel;
	Oid			tableoid;
	int			rls_status;

	/* Look up table name.  Can't lock it - we might not have privileges. */
	tablerel = makeRangeVarFromNameList(textToQualifiedNameList(tablename));
	tableoid = RangeVarGetRelid(tablerel, NoLock, false);

	rls_status = check_enable_rls(tableoid, InvalidOid, true);
	PG_RETURN_BOOL(rls_status == RLS_ENABLED);
}
