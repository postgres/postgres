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
#include "catalog/pg_class.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/rls.h"
#include "utils/syscache.h"


extern int	check_enable_rls(Oid relid, Oid checkAsUser, bool noError);

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
 * Handle checking as another role via checkAsUser (for views, etc). Note that
 * if *not* checking as another role, the caller should pass InvalidOid rather
 * than GetUserId(). Otherwise the check for row_security = OFF is skipped, and
 * so we may falsely report that RLS is active when the user has bypassed it.
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
	HeapTuple	tuple;
	Form_pg_class classform;
	bool		relrowsecurity;
	Oid			user_id = checkAsUser ? checkAsUser : GetUserId();

	/* Nothing to do for built-in relations */
	if (relid < FirstNormalObjectId)
		return RLS_NONE;

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
	 * Table owners always bypass RLS.  Note that superuser is always
	 * considered an owner.  Return RLS_NONE_ENV to indicate that this
	 * decision depends on the environment (in this case, the user_id).
	 */
	if (pg_class_ownercheck(relid, user_id))
		return RLS_NONE_ENV;

	/*
	 * If the row_security GUC is 'off', check if the user has permission to
	 * bypass RLS.  row_security is always considered 'on' when querying
	 * through a view or other cases where checkAsUser is valid.
	 */
	if (!row_security && !checkAsUser)
	{
		if (has_bypassrls_privilege(user_id))
			/* OK to bypass */
			return RLS_NONE_ENV;
		else if (noError)
			return RLS_ENABLED;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				  errmsg("insufficient privilege to bypass row security.")));
	}

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
