/*-------------------------------------------------------------------------
 *
 * snapmgr.h
 *	  POSTGRES snapshot manager
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/snapmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPMGR_H
#define SNAPMGR_H

#include "access/transam.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"


extern PGDLLIMPORT bool FirstSnapshotSet;

extern PGDLLIMPORT TransactionId TransactionXmin;
extern PGDLLIMPORT TransactionId RecentXmin;

/* Variables representing various special snapshot semantics */
extern PGDLLIMPORT SnapshotData SnapshotSelfData;
extern PGDLLIMPORT SnapshotData SnapshotAnyData;
extern PGDLLIMPORT SnapshotData SnapshotToastData;

#define SnapshotSelf		(&SnapshotSelfData)
#define SnapshotAny			(&SnapshotAnyData)

/* Use get_toast_snapshot() for the TOAST snapshot */

/*
 * We don't provide a static SnapshotDirty variable because it would be
 * non-reentrant.  Instead, users of that snapshot type should declare a
 * local variable of type SnapshotData, and initialize it with this macro.
 */
#define InitDirtySnapshot(snapshotdata)  \
	((snapshotdata).snapshot_type = SNAPSHOT_DIRTY)

/*
 * Similarly, some initialization is required for a NonVacuumable snapshot.
 * The caller must supply the visibility cutoff state to use (c.f.
 * GlobalVisTestFor()).
 */
#define InitNonVacuumableSnapshot(snapshotdata, vistestp)  \
	((snapshotdata).snapshot_type = SNAPSHOT_NON_VACUUMABLE, \
	 (snapshotdata).vistest = (vistestp))

/*
 * Is the snapshot implemented as an MVCC snapshot (i.e. it uses
 * SNAPSHOT_MVCC)? If so, there will be at most one visible tuple in a chain
 * of updated tuples, and each visible tuple will be seen exactly once.
 */
#define IsMVCCSnapshot(snapshot)  \
	((snapshot)->snapshot_type == SNAPSHOT_MVCC)

/*
 * Special kind of MVCC snapshot, used during logical decoding. The visibility
 * is checked from the perspective of an already committed transaction that is
 * being decoded.
 */
#define IsHistoricMVCCSnapshot(snapshot)  \
	((snapshot)->snapshot_type == SNAPSHOT_HISTORIC_MVCC)

/*
 * Is the snapshot either an MVCC snapshot or has equivalent visibility
 * semantics (see IsMVCCSnapshot())?
 */
#define IsMVCCLikeSnapshot(snapshot)  \
	(IsMVCCSnapshot(snapshot) || IsHistoricMVCCSnapshot(snapshot))

extern Snapshot GetTransactionSnapshot(void);
extern Snapshot GetLatestSnapshot(void);
extern void SnapshotSetCommandId(CommandId curcid);

extern Snapshot GetCatalogSnapshot(Oid relid);
extern Snapshot GetNonHistoricCatalogSnapshot(Oid relid);
extern void InvalidateCatalogSnapshot(void);
extern void InvalidateCatalogSnapshotConditionally(void);

extern void PushActiveSnapshot(Snapshot snapshot);
extern void PushActiveSnapshotWithLevel(Snapshot snapshot, int snap_level);
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
extern void AtEOXact_Snapshot(bool isCommit, bool resetXmin);

extern void ImportSnapshot(const char *idstr);
extern bool XactHasExportedSnapshots(void);
extern void DeleteAllExportedSnapshotFiles(void);
extern void WaitForOlderSnapshots(TransactionId limitXmin, bool progress);
extern bool ThereAreNoPriorRegisteredSnapshots(void);
extern bool HaveRegisteredOrActiveSnapshot(void);

extern char *ExportSnapshot(Snapshot snapshot);

/*
 * These live in procarray.c because they're intimately linked to the
 * procarray contents, but thematically they better fit into snapmgr.h.
 */
typedef struct GlobalVisState GlobalVisState;
extern GlobalVisState *GlobalVisTestFor(Relation rel);
extern bool GlobalVisTestIsRemovableXid(GlobalVisState *state, TransactionId xid);
extern bool GlobalVisTestIsRemovableFullXid(GlobalVisState *state, FullTransactionId fxid);
extern bool GlobalVisCheckRemovableXid(Relation rel, TransactionId xid);
extern bool GlobalVisCheckRemovableFullXid(Relation rel, FullTransactionId fxid);

/*
 * Utility functions for implementing visibility routines in table AMs.
 */
extern bool XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);

/* Support for catalog timetravel for logical decoding */
struct HTAB;
extern struct HTAB *HistoricSnapshotGetTupleCids(void);
extern void SetupHistoricSnapshot(Snapshot historic_snapshot, struct HTAB *tuplecids);
extern void TeardownHistoricSnapshot(bool is_error);
extern bool HistoricSnapshotActive(void);

extern Size EstimateSnapshotSpace(Snapshot snapshot);
extern void SerializeSnapshot(Snapshot snapshot, char *start_address);
extern Snapshot RestoreSnapshot(char *start_address);
struct PGPROC;
extern void RestoreTransactionSnapshot(Snapshot snapshot, struct PGPROC *source_pgproc);

#endif							/* SNAPMGR_H */
