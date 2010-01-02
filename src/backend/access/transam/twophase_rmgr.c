/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.c
 *	  Two-phase-commit resource managers tables
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/transam/twophase_rmgr.c,v 1.12 2010/01/02 16:57:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/twophase_rmgr.h"
#include "commands/async.h"
#include "pgstat.h"
#include "storage/lock.h"


const TwoPhaseCallback twophase_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_recover,		/* Lock */
	NULL,						/* notify/listen */
	NULL,						/* pgstat */
	multixact_twophase_recover	/* MultiXact */
};

const TwoPhaseCallback twophase_postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postcommit,	/* Lock */
	notify_twophase_postcommit, /* notify/listen */
	pgstat_twophase_postcommit,	/* pgstat */
	multixact_twophase_postcommit /* MultiXact */
};

const TwoPhaseCallback twophase_postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postabort,	/* Lock */
	NULL,						/* notify/listen */
	pgstat_twophase_postabort,	/* pgstat */
	multixact_twophase_postabort /* MultiXact */
};

const TwoPhaseCallback twophase_standby_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_standby_recover,		/* Lock */
	NULL,						/* notify/listen */
	NULL,						/* pgstat */
	NULL						/* MultiXact */
};
