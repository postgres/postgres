/*-------------------------------------------------------------------------
 *
 * snapmgr.h
 *	  POSTGRES snapshot manager
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/snapmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPMGR_H
#define SNAPMGR_H

#include "fmgr.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"


/*
 * The structure used to map times to TransactionId values for the "snapshot
 * too old" feature must have a few entries at the tail to hold old values;
 * otherwise the lookup will often fail and the expected early pruning or
 * vacuum will not usually occur.  It is best if this padding is for a number
 * of minutes greater than a thread would normally be stalled, but it's OK if
 * early vacuum opportunities are occasionally missed, so there's no need to
 * use an extreme value or get too fancy.  10 minutes seems plenty.
 */
#define OLD_SNAPSHOT_PADDING_ENTRIES 10
#define OLD_SNAPSHOT_TIME_MAP_ENTRIES (old_snapshot_threshold + OLD_SNAPSHOT_PADDING_ENTRIES)

/*
 * Common definition of relation properties that allow early pruning/vacuuming
 * when old_snapshot_threshold >= 0.
 */
#define RelationAllowsEarlyPruning(rel) \
( \
	 RelationNeedsWAL(rel) \
  && !IsCatalogRelation(rel) \
  && !RelationIsAccessibleInLogicalDecoding(rel) \
  && !RelationHasUnloggedIndex(rel) \
)

#define EarlyPruningEnabled(rel) (old_snapshot_threshold >= 0 && RelationAllowsEarlyPruning(rel))

/* GUC variables */
extern PGDLLIMPORT int old_snapshot_threshold;


extern Size SnapMgrShmemSize(void);
extern void SnapMgrInit(void);
extern int64 GetSnapshotCurrentTimestamp(void);
extern int64 GetOldSnapshotThresholdTimestamp(void);

extern bool FirstSnapshotSet;

extern TransactionId TransactionXmin;
extern TransactionId RecentXmin;
extern TransactionId RecentGlobalXmin;
extern TransactionId RecentGlobalDataXmin;

extern Snapshot GetTransactionSnapshot(void);
extern Snapshot GetLatestSnapshot(void);
extern void SnapshotSetCommandId(CommandId curcid);
extern Snapshot GetOldestSnapshot(void);

extern Snapshot GetCatalogSnapshot(Oid relid);
extern Snapshot GetNonHistoricCatalogSnapshot(Oid relid);
extern void InvalidateCatalogSnapshot(void);

extern void PushActiveSnapshot(Snapshot snapshot);
extern void PushCopiedSnapshot(Snapshot snapshot);
extern void UpdateActiveSnapshotCommandId(void);
extern void PopActiveSnapshot(void);
extern Snapshot GetActiveSnapshot(void);
extern bool ActiveSnapshotSet(void);

extern Snapshot RegisterSnapshot(Snapshot snapshot);
extern void UnregisterSnapshot(Snapshot snapshot);
extern Snapshot RegisterSnapshotOnOwner(Snapshot snapshot, ResourceOwner owner);
extern void UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner);

extern void AtSubCommit_Snapshot(int level);
extern void AtSubAbort_Snapshot(int level);
extern void AtEOXact_Snapshot(bool isCommit);

extern Datum pg_export_snapshot(PG_FUNCTION_ARGS);
extern void ImportSnapshot(const char *idstr);
extern bool XactHasExportedSnapshots(void);
extern void DeleteAllExportedSnapshotFiles(void);
extern bool ThereAreNoPriorRegisteredSnapshots(void);
extern TransactionId TransactionIdLimitedForOldSnapshots(TransactionId recentXmin,
									Relation relation);
extern void MaintainOldSnapshotTimeMapping(int64 whenTaken, TransactionId xmin);

extern char *ExportSnapshot(Snapshot snapshot);

/* Support for catalog timetravel for logical decoding */
struct HTAB;
extern struct HTAB *HistoricSnapshotGetTupleCids(void);
extern void SetupHistoricSnapshot(Snapshot snapshot_now, struct HTAB *tuplecids);
extern void TeardownHistoricSnapshot(bool is_error);
extern bool HistoricSnapshotActive(void);

extern Size EstimateSnapshotSpace(Snapshot snapshot);
extern void SerializeSnapshot(Snapshot snapshot, char *start_address);
extern Snapshot RestoreSnapshot(char *start_address);
extern void RestoreTransactionSnapshot(Snapshot snapshot, void *master_pgproc);

#endif   /* SNAPMGR_H */
