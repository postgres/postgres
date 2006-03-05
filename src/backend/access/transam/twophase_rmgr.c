/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.c
 *	  Two-phase-commit resource managers tables
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/transam/twophase_rmgr.c,v 1.3 2006/03/05 15:58:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase_rmgr.h"
#include "commands/async.h"
#include "storage/lock.h"
#include "utils/flatfiles.h"
#include "utils/inval.h"


const TwoPhaseCallback twophase_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_recover,		/* Lock */
	NULL,						/* Inval */
	NULL,						/* flat file update */
	NULL						/* notify/listen */
};

const TwoPhaseCallback twophase_postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postcommit,	/* Lock */
	inval_twophase_postcommit,	/* Inval */
	flatfile_twophase_postcommit,		/* flat file update */
	notify_twophase_postcommit	/* notify/listen */
};

const TwoPhaseCallback twophase_postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postabort,	/* Lock */
	NULL,						/* Inval */
	NULL,						/* flat file update */
	NULL						/* notify/listen */
};
