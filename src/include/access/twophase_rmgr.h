/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.h
 *	  Two-phase-commit resource managers definition
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/twophase_rmgr.h,v 1.3 2006/03/05 15:58:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_RMGR_H
#define TWOPHASE_RMGR_H

typedef void (*TwoPhaseCallback) (TransactionId xid, uint16 info,
											  void *recdata, uint32 len);
typedef uint8 TwoPhaseRmgrId;

/*
 * Built-in resource managers
 */
#define TWOPHASE_RM_END_ID			0
#define TWOPHASE_RM_LOCK_ID			1
#define TWOPHASE_RM_INVAL_ID		2
#define TWOPHASE_RM_FLATFILES_ID	3
#define TWOPHASE_RM_NOTIFY_ID		4
#define TWOPHASE_RM_MAX_ID			TWOPHASE_RM_NOTIFY_ID

extern const TwoPhaseCallback twophase_recover_callbacks[];
extern const TwoPhaseCallback twophase_postcommit_callbacks[];
extern const TwoPhaseCallback twophase_postabort_callbacks[];


extern void RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, uint16 info,
					   const void *data, uint32 len);

#endif   /* TWOPHASE_RMGR_H */
