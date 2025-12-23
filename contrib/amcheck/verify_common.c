/*-------------------------------------------------------------------------
 *
 * verify_common.c
 *		Utility functions common to all access methods.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_common.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "verify_common.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "utils/guc.h"
#include "utils/syscache.h"

static bool amcheck_index_mainfork_expected(Relation rel);
static bool index_checkable(Relation rel, Oid am_id);


/*
 * Check if index relation should have a file for its main relation fork.
 * Verification uses this to skip unlogged indexes when in hot standby mode,
 * where there is simply nothing to verify.
 *
 * NB: Caller should call index_checkable() before calling here.
 */
static bool
amcheck_index_mainfork_expected(Relation rel)
{
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED ||
		!RecoveryInProgress())
		return true;

	ereport(NOTICE,
			(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
			 errmsg("cannot verify unlogged index \"%s\" during recovery, skipping",
					RelationGetRelationName(rel))));

	return false;
}

/*
* Amcheck main workhorse.
* Given index relation OID, lock relation.
* Next, take a number of standard actions:
* 1) Make sure the index can be checked
* 2) change the context of the user,
* 3) keep track of GUCs modified via index functions
* 4) execute callback function to verify integrity.
*/
void
amcheck_lock_relation_and_check(Oid indrelid,
								Oid am_id,
								IndexDoCheckCallback check,
								LOCKMODE lockmode,
								void *state)
{
	Oid			heapid;
	Relation	indrel;
	Relation	heaprel;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	/*
	 * We must lock table before index to avoid deadlocks.  However, if the
	 * passed indrelid isn't an index then IndexGetRelation() will fail.
	 * Rather than emitting a not-very-helpful error message, postpone
	 * complaining, expecting that the is-it-an-index test below will fail.
	 *
	 * In hot standby mode this will raise an error when parentcheck is true.
	 */
	heapid = IndexGetRelation(indrelid, true);
	if (OidIsValid(heapid))
	{
		heaprel = table_open(heapid, lockmode);

		/*
		 * Switch to the table owner's userid, so that any index functions are
		 * run as that user.  Also lock down security-restricted operations
		 * and arrange to make GUC variable changes local to this command.
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heaprel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
	}
	else
	{
		heaprel = NULL;
		/* Set these just to suppress "uninitialized variable" warnings */
		save_userid = InvalidOid;
		save_sec_context = -1;
		save_nestlevel = -1;
	}

	/*
	 * Open the target index relations separately (like relation_openrv(), but
	 * with heap relation locked first to prevent deadlocking).  In hot
	 * standby mode this will raise an error when parentcheck is true.
	 *
	 * There is no need for the usual indcheckxmin usability horizon test
	 * here, even in the heapallindexed case, because index undergoing
	 * verification only needs to have entries for a new transaction snapshot.
	 * (If this is a parentcheck verification, there is no question about
	 * committed or recently dead heap tuples lacking index entries due to
	 * concurrent activity.)
	 */
	indrel = index_open(indrelid, lockmode);

	/*
	 * Since we did the IndexGetRelation call above without any lock, it's
	 * barely possible that a race against an index drop/recreation could have
	 * netted us the wrong table.
	 */
	if (heaprel == NULL || heapid != IndexGetRelation(indrelid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index \"%s\"",
						RelationGetRelationName(indrel))));

	/* Check that relation suitable for checking */
	if (index_checkable(indrel, am_id))
		check(indrel, heaprel, state, lockmode == ShareLock);

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/*
	 * Release locks early. That's ok here because nothing in the called
	 * routines will trigger shared cache invalidations to be sent, so we can
	 * relax the usual pattern of only releasing locks after commit.
	 */
	index_close(indrel, lockmode);
	if (heaprel)
		table_close(heaprel, lockmode);
}

/*
 * Basic checks about the suitability of a relation for checking as an index.
 *
 *
 * NB: Intentionally not checking permissions, the function is normally not
 * callable by non-superusers. If granted, it's useful to be able to check a
 * whole cluster.
 */
static bool
index_checkable(Relation rel, Oid am_id)
{
	if (rel->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("expected index as targets for verification"),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (rel->rd_rel->relam != am_id)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("expected \"%s\" index as targets for verification", get_am_name(am_id)),
				 errdetail("Relation \"%s\" is a %s index.",
						   RelationGetRelationName(rel), get_am_name(rel->rd_rel->relam))));

	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions"),
				 errdetail("Index \"%s\" is associated with temporary relation.",
						   RelationGetRelationName(rel))));

	if (!rel->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot check index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Index is not valid.")));

	return amcheck_index_mainfork_expected(rel);
}
