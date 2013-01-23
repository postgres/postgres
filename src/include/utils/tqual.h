/*-------------------------------------------------------------------------
 *
 * tqual.h
 *	  POSTGRES "time qualification" definitions, ie, tuple visibility rules.
 *
 *	  Should be moved/renamed...	- vadim 07/28/98
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
extern PGDLLIMPORT SnapshotData SnapshotNowData;
extern PGDLLIMPORT SnapshotData SnapshotSelfData;
extern PGDLLIMPORT SnapshotData SnapshotAnyData;
extern PGDLLIMPORT SnapshotData SnapshotToastData;

#define SnapshotNow			(&SnapshotNowData)
#define SnapshotSelf		(&SnapshotSelfData)
#define SnapshotAny			(&SnapshotAnyData)
#define SnapshotToast		(&SnapshotToastData)

/*
 * We don't provide a static SnapshotDirty variable because it would be
 * non-reentrant.  Instead, users of that snapshot type should declare a
 * local variable of type SnapshotData, and initialize it with this macro.
 */
#define InitDirtySnapshot(snapshotdata)  \
	((snapshotdata).satisfies = HeapTupleSatisfiesDirty)

/* This macro encodes the knowledge of which snapshots are MVCC-safe */
#define IsMVCCSnapshot(snapshot)  \
	((snapshot)->satisfies == HeapTupleSatisfiesMVCC)

/*
 * HeapTupleSatisfiesVisibility
 *		True iff heap tuple satisfies a time qual.
 *
 * Notes:
 *	Assumes heap tuple is valid.
 *	Beware of multiple evaluations of snapshot argument.
 *	Hint bits in the HeapTuple's t_infomask may be updated as a side effect;
 *	if so, the indicated buffer is marked dirty.
 */
#define HeapTupleSatisfiesVisibility(tuple, snapshot, buffer) \
	((*(snapshot)->satisfies) ((tuple)->t_data, snapshot, buffer))

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,		/* inserting xact is still in progress */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* deleting xact is still in progress */
} HTSV_Result;

/* These are the "satisfies" test routines for the various snapshot types */
extern bool HeapTupleSatisfiesMVCC(HeapTupleHeader tuple,
					   Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesNow(HeapTupleHeader tuple,
					  Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesSelf(HeapTupleHeader tuple,
					   Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesAny(HeapTupleHeader tuple,
					  Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesToast(HeapTupleHeader tuple,
						Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesDirty(HeapTupleHeader tuple,
						Snapshot snapshot, Buffer buffer);

/* Special "satisfies" routines with different APIs */
extern HTSU_Result HeapTupleSatisfiesUpdate(HeapTupleHeader tuple,
						 CommandId curcid, Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTupleHeader tuple,
						 TransactionId OldestXmin, Buffer buffer);
extern bool HeapTupleIsSurelyDead(HeapTupleHeader tuple,
					  TransactionId OldestXmin);

extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
					 uint16 infomask, TransactionId xid);
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);

#endif   /* TQUAL_H */
