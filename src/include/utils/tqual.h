/*-------------------------------------------------------------------------
 *
 * tqual.h
 *	  POSTGRES "time" qualification definitions, ie, tuple visibility rules.
 *
 *	  Should be moved/renamed...	- vadim 07/28/98
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.48 2003/10/01 21:30:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include "access/htup.h"
#include "access/xact.h"


typedef struct SnapshotData
{
	TransactionId xmin;			/* XID < xmin are visible to me */
	TransactionId xmax;			/* XID >= xmax are invisible to me */
	uint32		xcnt;			/* # of xact ids in xip[] */
	TransactionId *xip;			/* array of xact IDs in progress */
	/* note: all ids in xip[] satisfy xmin <= xip[i] < xmax */
	CommandId	curcid;			/* in my xact, CID < curcid are visible */
	ItemPointerData tid;		/* required for Dirty snapshot -:( */
} SnapshotData;

typedef SnapshotData *Snapshot;

#define SnapshotNow					((Snapshot) 0x0)
#define SnapshotSelf				((Snapshot) 0x1)
#define SnapshotAny					((Snapshot) 0x2)
#define SnapshotToast				((Snapshot) 0x3)

extern DLLIMPORT Snapshot SnapshotDirty;
extern DLLIMPORT Snapshot QuerySnapshot;
extern DLLIMPORT Snapshot SerializableSnapshot;

extern TransactionId RecentXmin;
extern TransactionId RecentGlobalXmin;


/*
 * HeapTupleSatisfiesVisibility
 *		True iff heap tuple satisfies a time qual.
 *
 * Notes:
 *		Assumes heap tuple is valid.
 *		Beware of multiple evaluations of snapshot argument.
 */
#define HeapTupleSatisfiesVisibility(tuple, snapshot) \
((snapshot) == SnapshotNow ? \
	HeapTupleSatisfiesNow((tuple)->t_data) \
: \
	((snapshot) == SnapshotSelf ? \
		HeapTupleSatisfiesItself((tuple)->t_data) \
	: \
		((snapshot) == SnapshotAny ? \
			true \
		: \
			((snapshot) == SnapshotToast ? \
				HeapTupleSatisfiesToast((tuple)->t_data) \
			: \
				((snapshot) == SnapshotDirty ? \
					HeapTupleSatisfiesDirty((tuple)->t_data) \
				: \
					HeapTupleSatisfiesSnapshot((tuple)->t_data, snapshot) \
				) \
			) \
		) \
	) \
)

/* Result codes for HeapTupleSatisfiesUpdate */
#define HeapTupleMayBeUpdated		0
#define HeapTupleInvisible			1
#define HeapTupleSelfUpdated		2
#define HeapTupleUpdated			3
#define HeapTupleBeingUpdated		4

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,		/* inserting xact is still in
										 * progress */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* deleting xact is still in progress */
} HTSV_Result;

extern bool HeapTupleSatisfiesItself(HeapTupleHeader tuple);
extern bool HeapTupleSatisfiesNow(HeapTupleHeader tuple);
extern bool HeapTupleSatisfiesDirty(HeapTupleHeader tuple);
extern bool HeapTupleSatisfiesToast(HeapTupleHeader tuple);
extern bool HeapTupleSatisfiesSnapshot(HeapTupleHeader tuple,
						   Snapshot snapshot);
extern int HeapTupleSatisfiesUpdate(HeapTupleHeader tuple,
						 CommandId curcid);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTupleHeader tuple,
						 TransactionId OldestXmin);

extern Snapshot GetSnapshotData(Snapshot snapshot, bool serializable);
extern void SetQuerySnapshot(void);
extern Snapshot CopyQuerySnapshot(void);
extern Snapshot CopyCurrentSnapshot(void);
extern void FreeXactSnapshot(void);

#endif   /* TQUAL_H */
