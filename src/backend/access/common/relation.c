/*-------------------------------------------------------------------------
 *
 * relation.c
 *	  Generic relation related routines.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/relation.c
 *
 * NOTES
 *	  This file contains relation_ routines that implement access to relations
 *	  (tables, indexes, etc). Support that's specific to subtypes of relations
 *	  should go into their respective files, not here.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "pgplanner/pgplanner.h"


/* ----------------
 *		relation_open - open any relation by relation OID
 * ----------------
 */
Relation
relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;
	const PgPlannerCallbacks *cb;
	PgPlannerRelationInfo *rinfo;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* Get the lock before trying to open the relcache entry */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/* Epsio -- use pgplanner callbacks instead of relcache */
	cb = pgplanner_get_callbacks();
	rinfo = cb->get_relation_by_oid(relationId);
	if (rinfo == NULL)
		elog(ERROR, "could not open relation with OID %u", relationId);
	r = pgplanner_build_relation(rinfo);

	return r;
}
/* ----------------
 *		try_relation_open - open any relation by relation OID
 *
 *		Same as relation_open, except return NULL instead of failing
 *		if the relation does not exist.
 * ----------------
 */
Relation
try_relation_open(Oid relationId, LOCKMODE lockmode)
{
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relationId)))
		return NULL;
	return relation_open(relationId, NoLock);
}

/* ----------------
 *		relation_openrv - open any relation specified by a RangeVar
 *
 *		Same as relation_open, but the relation is specified by a RangeVar.
 * ----------------
 */
Relation
relation_openrv(const RangeVar *relation, LOCKMODE lockmode)
{
	Oid			relOid;

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  This is needed even if we already hold a lock on the
	 * relation, because GRANT/REVOKE are executed without taking any lock on
	 * the target relation, and we want to be sure we see current ACL
	 * information.  We can skip this if asked for NoLock, on the assumption
	 * that such a call is not the first one in the current command, and so we
	 * should be reasonably up-to-date already.  (XXX this all could stand to
	 * be redesigned, but for the moment we'll keep doing this like it's been
	 * done historically.)
	 */
	// Epsio -- no need for this ;)
	// if (lockmode != NoLock)
	// 	AcceptInvalidationMessages();

	/* Look up and lock the appropriate relation using namespace search */
	relOid = RangeVarGetRelid(relation, lockmode, false);

	/* Let relation_open do the rest */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_openrv_extended - open any relation specified by a RangeVar
 *
 *		Same as relation_openrv, but with an additional missing_ok argument
 *		allowing a NULL return rather than an error if the relation is not
 *		found.
 * ----------------
 */
Relation
relation_openrv_extended(const RangeVar *relation, LOCKMODE lockmode,
						 bool missing_ok)
{
	Oid			relOid;

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  See comments in relation_openrv().
	 */
	// if (lockmode != NoLock)
	// 	AcceptInvalidationMessages();

	/* Look up and lock the appropriate relation using namespace search */
	relOid = RangeVarGetRelid(relation, lockmode, missing_ok);

	/* Return NULL on not-found */
	if (!OidIsValid(relOid))
		return NULL;

	/* Let relation_open do the rest */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_close - close any relation
 *
 *		In pgplanner mode, relations are palloc'd fakes — no-op.
 * ----------------
 */
void
relation_close(Relation relation, LOCKMODE lockmode)
{
	/* no-op: fake relations from pgplanner, no relcache or locks */
}
