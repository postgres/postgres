/*-------------------------------------------------------------------------
 *
 * superuser.c
 *	  The superuser() function.  Determines if user has superuser privilege.
 *	  Also, a function to check for the owner (datdba) of a database.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/superuser.c,v 1.22 2002/05/20 23:51:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "utils/syscache.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"


/*
 * The Postgres user running this command has Postgres superuser privileges
 *
 * All code should use either of these two functions to find out
 * whether a given user is a superuser, rather than evaluating
 * pg_shadow.usesuper directly, so that the escape hatch built in for
 * the single-user case works.
 */
bool
superuser(void)
{
	return superuser_arg(GetUserId());
}


bool
superuser_arg(Oid userid)
{
	bool		result = false;
	HeapTuple	utup;

	/* Special escape path in case you deleted all your users. */
	if (!IsUnderPostmaster && userid == BOOTSTRAP_USESYSID)
		return true;

	utup = SearchSysCache(SHADOWSYSID,
						  ObjectIdGetDatum(userid),
						  0, 0, 0);
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_shadow) GETSTRUCT(utup))->usesuper;
		ReleaseSysCache(utup);
	}
	return result;
}


/*
 * The Postgres user running this command is the owner of the specified
 * database.
 */
bool
is_dbadmin(Oid dbid)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	dbtuple;
	int32		dba;

	/* There's no syscache for pg_database, so must look the hard way */
	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(dbid));
	scan = heap_beginscan(pg_database, SnapshotNow, 1, entry);
	dbtuple = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(dbtuple))
		elog(ERROR, "database %u does not exist", dbid);
	dba = ((Form_pg_database) GETSTRUCT(dbtuple))->datdba;
	heap_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	/* XXX some confusion about whether userids are OID or int4 ... */
	return (GetUserId() == (Oid) dba);
}
