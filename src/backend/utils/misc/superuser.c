/*-------------------------------------------------------------------------
 *
 * superuser.c
 *	  The superuser() function.  Determines if user has superuser privilege.
 *
 * All code should use either of these two functions to find out
 * whether a given user is a superuser, rather than examining
 * pg_shadow.usesuper directly, so that the escape hatch built in for
 * the single-user case works.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/misc/superuser.c,v 1.31 2005/05/29 20:38:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_shadow.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "miscadmin.h"


/*
 * In common cases the same userid (ie, the session or current ID) will
 * be queried repeatedly.  So we maintain a simple one-entry cache for
 * the status of the last requested userid.  The cache can be flushed
 * at need by watching for cache update events on pg_shadow.
 */
static AclId	last_userid = 0;		/* 0 == cache not valid */
static bool		last_userid_is_super = false;
static bool		userid_callback_registered = false;

static void UseridCallback(Datum arg, Oid relid);


/*
 * The Postgres user running this command has Postgres superuser privileges
 */
bool
superuser(void)
{
	return superuser_arg(GetUserId());
}


/*
 * The specified userid has Postgres superuser privileges
 */
bool
superuser_arg(AclId userid)
{
	bool		result;
	HeapTuple	utup;

	/* Quick out for cache hit */
	if (AclIdIsValid(last_userid) && last_userid == userid)
		return last_userid_is_super;

	/* Special escape path in case you deleted all your users. */
	if (!IsUnderPostmaster && userid == BOOTSTRAP_USESYSID)
		return true;

	/* OK, look up the information in pg_shadow */
	utup = SearchSysCache(SHADOWSYSID,
						  Int32GetDatum(userid),
						  0, 0, 0);
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_shadow) GETSTRUCT(utup))->usesuper;
		ReleaseSysCache(utup);
	}
	else
	{
		/* Report "not superuser" for invalid userids */
		result = false;
	}

	/* If first time through, set up callback for cache flushes */
	if (!userid_callback_registered)
	{
		CacheRegisterSyscacheCallback(SHADOWSYSID,
									  UseridCallback,
									  (Datum) 0);
		userid_callback_registered = true;
	}

	/* Cache the result for next time */
	last_userid = userid;
	last_userid_is_super = result;

	return result;
}

/*
 * UseridCallback
 *		Syscache inval callback function
 */
static void
UseridCallback(Datum arg, Oid relid)
{
	/* Invalidate our local cache in case user's superuserness changed */
	last_userid = 0;
}
