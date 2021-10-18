/*-------------------------------------------------------------------------
 *
 * snapbuild.h
 *	  Exports from replication/logical/snapbuild.c.
 *
 * Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 * src/include/replication/snapbuild.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPBUILD_H
#define SNAPBUILD_H

#include "access/xlogdefs.h"
#include "utils/snapmgr.h"

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
	SNAPBUILD_CONSISTENT = 2
} SnapBuildState;

/* forward declare so we don't have to expose the struct to the public */
struct SnapBuild;
typedef struct SnapBuild SnapBuild;

/* forward declare so we don't have to include reorderbuffer.h */
struct ReorderBuffer;

/* forward declare so we don't have to include heapam_xlog.h */
struct xl_heap_new_cid;
struct xl_running_xacts;

extern void CheckPointSnapBuild(void);

extern SnapBuild *AllocateSnapshotBuilder(struct ReorderBuffer *cache,
										  TransactionId xmin_horizon, XLogRecPtr start_lsn,
										  bool need_full_snapshot);
extern void FreeSnapshotBuilder(SnapBuild *cache);

extern void SnapBuildSnapDecRefcount(Snapshot snap);

extern Snapshot SnapBuildInitialSnapshot(SnapBuild *builder);
extern const char *SnapBuildExportSnapshot(SnapBuild *snapstate);
extern void SnapBuildClearExportedSnapshot(void);
extern void SnapBuildResetExportedSnapshotState(void);

extern SnapBuildState SnapBuildCurrentState(SnapBuild *snapstate);
extern Snapshot SnapBuildGetOrBuildSnapshot(SnapBuild *builder,
											TransactionId xid);

extern bool SnapBuildXactNeedsSkip(SnapBuild *snapstate, XLogRecPtr ptr);

extern void SnapBuildCommitTxn(SnapBuild *builder, XLogRecPtr lsn,
							   TransactionId xid, int nsubxacts,
							   TransactionId *subxacts);
extern bool SnapBuildProcessChange(SnapBuild *builder, TransactionId xid,
								   XLogRecPtr lsn);
extern void SnapBuildProcessNewCid(SnapBuild *builder, TransactionId xid,
								   XLogRecPtr lsn, struct xl_heap_new_cid *cid);
extern void SnapBuildProcessRunningXacts(SnapBuild *builder, XLogRecPtr lsn,
										 struct xl_running_xacts *running);
extern void SnapBuildSerializationPoint(SnapBuild *builder, XLogRecPtr lsn);

#endif							/* SNAPBUILD_H */
