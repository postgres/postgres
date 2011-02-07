/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.c
 *	  Two-phase-commit resource managers tables
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/twophase_rmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/twophase_rmgr.h"
#include "pgstat.h"
#include "storage/lock.h"
#include "storage/predicate.h"


const TwoPhaseCallback twophase_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_recover,		/* Lock */
	predicatelock_twophase_recover,		/* PredicateLock */
	NULL,						/* pgstat */
	multixact_twophase_recover	/* MultiXact */
};

const TwoPhaseCallback twophase_postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postcommit,	/* Lock */
	NULL,						/* PredicateLock */
	pgstat_twophase_postcommit, /* pgstat */
	multixact_twophase_postcommit		/* MultiXact */
};

const TwoPhaseCallback twophase_postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postabort,	/* Lock */
	NULL,						/* PredicateLock */
	pgstat_twophase_postabort,	/* pgstat */
	multixact_twophase_postabort	/* MultiXact */
};

const TwoPhaseCallback twophase_standby_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_standby_recover,		/* Lock */
	NULL,						/* PredicateLock */
	NULL,						/* pgstat */
	NULL						/* MultiXact */
};
