/*-------------------------------------------------------------------------
 *
 * superuser.c
 *	  The superuser() function.  Determines if user has superuser privilege.
 *
 * All code should use either of these two functions to find out
 * whether a given user is a superuser, rather than examining
 * pg_authid.rolsuper directly, so that the escape hatch built in for
 * the single-user case works.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/misc/superuser.c,v 1.35 2006/03/05 15:58:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "miscadmin.h"


/*
 * In common cases the same roleid (ie, the session or current ID) will
 * be queried repeatedly.  So we maintain a simple one-entry cache for
 * the status of the last requested roleid.  The cache can be flushed
 * at need by watching for cache update events on pg_authid.
 */
static Oid	last_roleid = InvalidOid;	/* InvalidOid == cache not valid */
static bool last_roleid_is_super = false;
static bool roleid_callback_registered = false;

static void RoleidCallback(Datum arg, Oid relid);


/*
 * The Postgres user running this command has Postgres superuser privileges
 */
bool
superuser(void)
{
	return superuser_arg(GetUserId());
}


/*
 * The specified role has Postgres superuser privileges
 */
bool
superuser_arg(Oid roleid)
{
	bool		result;
	HeapTuple	rtup;

	/* Quick out for cache hit */
	if (OidIsValid(last_roleid) && last_roleid == roleid)
		return last_roleid_is_super;

	/* Special escape path in case you deleted all your users. */
	if (!IsUnderPostmaster && roleid == BOOTSTRAP_SUPERUSERID)
		return true;

	/* OK, look up the information in pg_authid */
	rtup = SearchSysCache(AUTHOID,
						  ObjectIdGetDatum(roleid),
						  0, 0, 0);
	if (HeapTupleIsValid(rtup))
	{
		result = ((Form_pg_authid) GETSTRUCT(rtup))->rolsuper;
		ReleaseSysCache(rtup);
	}
	else
	{
		/* Report "not superuser" for invalid roleids */
		result = false;
	}

	/* If first time through, set up callback for cache flushes */
	if (!roleid_callback_registered)
	{
		CacheRegisterSyscacheCallback(AUTHOID,
									  RoleidCallback,
									  (Datum) 0);
		roleid_callback_registered = true;
	}

	/* Cache the result for next time */
	last_roleid = roleid;
	last_roleid_is_super = result;

	return result;
}

/*
 * UseridCallback
 *		Syscache inval callback function
 */
static void
RoleidCallback(Datum arg, Oid relid)
{
	/* Invalidate our local cache in case role's superuserness changed */
	last_roleid = InvalidOid;
}
