/*-------------------------------------------------------------------------
 *
 * snapbuild.h
 *	  Exports from replication/logical/snapbuild.c.
 *
 * Copyright (c) 2012-2026, PostgreSQL Global Development Group
 *
 * src/include/replication/snapbuild.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPBUILD_H
#define SNAPBUILD_H

#include "access/xlogdefs.h"
#include "utils/snapmgr.h"

/*
 * forward declarations in this file
 */
typedef struct ReorderBuffer ReorderBuffer;
typedef struct SnapBuild SnapBuild;
typedef struct xl_heap_new_cid xl_heap_new_cid;
typedef struct xl_running_xacts xl_running_xacts;

/*
 * Please keep get_snapbuild_state_desc() (located in the pg_logicalinspect
 * module) updated if a change needs to be made to SnapBuildState.
 */
typedef enum
{
	/*
	 * Initial state, we can't do much yet.
	 */
	SNAPBUILD_START = -1,

	/*
	 * Collecting committed transactions, to build the initial catalog
	 * snapshot.
	 */
	SNAPBUILD_BUILDING_SNAPSHOT = 0,

	/*
	 * We have collected enough information to decode tuples in transactions
	 * that started after this.
	 *
	 * Once we reached this we start to collect changes. We cannot apply them
	 * yet, because they might be based on transactions that were still
	 * running when FULL_SNAPSHOT was reached.
	 */
	SNAPBUILD_FULL_SNAPSHOT = 1,

	/*
	 * Found a point after SNAPBUILD_FULL_SNAPSHOT where all transactions that
	 * were running at that point finished. Till we reach that we hold off
	 * calling any commit callbacks.
	 */
	SNAPBUILD_CONSISTENT = 2,
} SnapBuildState;



extern void CheckPointSnapBuild(void);

extern SnapBuild *AllocateSnapshotBuilder(ReorderBuffer *reorder,
										  TransactionId xmin_horizon, XLogRecPtr start_lsn,
										  bool need_full_snapshot,
										  bool in_slot_creation,
										  XLogRecPtr two_phase_at);
extern void FreeSnapshotBuilder(SnapBuild *builder);

extern void SnapBuildSnapDecRefcount(Snapshot snap);

extern Snapshot SnapBuildInitialSnapshot(SnapBuild *builder);
extern const char *SnapBuildExportSnapshot(SnapBuild *builder);
extern void SnapBuildClearExportedSnapshot(void);
extern void SnapBuildResetExportedSnapshotState(void);

extern SnapBuildState SnapBuildCurrentState(SnapBuild *builder);
extern Snapshot SnapBuildGetOrBuildSnapshot(SnapBuild *builder);

extern bool SnapBuildXactNeedsSkip(SnapBuild *builder, XLogRecPtr ptr);
extern XLogRecPtr SnapBuildGetTwoPhaseAt(SnapBuild *builder);
extern void SnapBuildSetTwoPhaseAt(SnapBuild *builder, XLogRecPtr ptr);

extern void SnapBuildCommitTxn(SnapBuild *builder, XLogRecPtr lsn,
							   TransactionId xid, int nsubxacts,
							   TransactionId *subxacts, uint32 xinfo);
extern bool SnapBuildProcessChange(SnapBuild *builder, TransactionId xid,
								   XLogRecPtr lsn);
extern void SnapBuildProcessNewCid(SnapBuild *builder, TransactionId xid,
								   XLogRecPtr lsn,
								   xl_heap_new_cid *xlrec);
extern void SnapBuildProcessRunningXacts(SnapBuild *builder, XLogRecPtr lsn,
										 xl_running_xacts *running,
										 bool db_specific);
extern void SnapBuildSerializationPoint(SnapBuild *builder, XLogRecPtr lsn);

extern bool SnapBuildSnapshotExists(XLogRecPtr lsn);

#endif							/* SNAPBUILD_H */
