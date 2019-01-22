/*-------------------------------------------------------------------------
 *
 * tqual.h
 *	  POSTGRES "time qualification" definitions, ie, tuple visibility rules.
 *
 *	  Should be moved/renamed...    - vadim 07/28/98
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/tqual.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include "utils/snapshot.h"


/* Static variables representing various special snapshot semantics */
extern PGDLLIMPORT SnapshotData SnapshotSelfData;
extern PGDLLIMPORT SnapshotData SnapshotAnyData;
extern PGDLLIMPORT SnapshotData CatalogSnapshotData;

#define SnapshotSelf		(&SnapshotSelfData)
#define SnapshotAny			(&SnapshotAnyData)

/* This macro encodes the knowledge of which snapshots are MVCC-safe */
#define IsMVCCSnapshot(snapshot)  \
	((snapshot)->snapshot_type == SNAPSHOT_MVCC || \
	 (snapshot)->snapshot_type == SNAPSHOT_HISTORIC_MVCC)

extern bool HeapTupleSatisfiesVisibility(HeapTuple stup, Snapshot snapshot,
							 Buffer buffer);

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,	/* inserting xact is still in progress */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* deleting xact is still in progress */
} HTSV_Result;

/* Special "satisfies" routines with different APIs */
extern HTSU_Result HeapTupleSatisfiesUpdate(HeapTuple htup,
						 CommandId curcid, Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTuple htup,
						 TransactionId OldestXmin, Buffer buffer);
extern bool HeapTupleIsSurelyDead(HeapTuple htup,
					  TransactionId OldestXmin);
extern bool XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);

extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
					 uint16 infomask, TransactionId xid);
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);

/*
 * To avoid leaking too much knowledge about reorderbuffer implementation
 * details this is implemented in reorderbuffer.c not tqual.c.
 */
struct HTAB;
extern bool ResolveCminCmaxDuringDecoding(struct HTAB *tuplecid_data,
							  Snapshot snapshot,
							  HeapTuple htup,
							  Buffer buffer,
							  CommandId *cmin, CommandId *cmax);

/*
 * We don't provide a static SnapshotDirty variable because it would be
 * non-reentrant.  Instead, users of that snapshot type should declare a
 * local variable of type SnapshotData, and initialize it with this macro.
 */
#define InitDirtySnapshot(snapshotdata)  \
	((snapshotdata).snapshot_type = SNAPSHOT_DIRTY)

/*
 * Similarly, some initialization is required for a NonVacuumable snapshot.
 * The caller must supply the xmin horizon to use (e.g., RecentGlobalXmin).
 */
#define InitNonVacuumableSnapshot(snapshotdata, xmin_horizon)  \
	((snapshotdata).snapshot_type = SNAPSHOT_NON_VACUUMABLE, \
	 (snapshotdata).xmin = (xmin_horizon))

/*
 * Similarly, some initialization is required for SnapshotToast.  We need
 * to set lsn and whenTaken correctly to support snapshot_too_old.
 */
#define InitToastSnapshot(snapshotdata, l, w)  \
	((snapshotdata).snapshot_type = SNAPSHOT_TOAST, \
	 (snapshotdata).lsn = (l),					\
	 (snapshotdata).whenTaken = (w))

#endif							/* TQUAL_H */
