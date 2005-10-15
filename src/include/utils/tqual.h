/*-------------------------------------------------------------------------
 *
 * tqual.h
 *	  POSTGRES "time qualification" definitions, ie, tuple visibility rules.
 *
 *	  Should be moved/renamed...	- vadim 07/28/98
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/tqual.h,v 1.59 2005/10/15 02:49:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include "access/htup.h"
#include "access/xact.h"
#include "storage/buf.h"


/*
 * "Regular" snapshots are pointers to a SnapshotData structure.
 *
 * We also have some "special" snapshot values that have fixed meanings
 * and don't need any backing SnapshotData.  These are encoded by small
 * integer values, which of course is a gross violation of ANSI C, but
 * it works fine on all known platforms.
 *
 * SnapshotDirty is an even more special case: its semantics are fixed,
 * but there is a backing SnapshotData struct for it.  That struct is
 * actually used as *output* data from tqual.c, not input into it.
 * (But hey, SnapshotDirty ought to have a dirty implementation, no? ;-))
 */

typedef struct SnapshotData
{
	TransactionId xmin;			/* XID < xmin are visible to me */
	TransactionId xmax;			/* XID >= xmax are invisible to me */
	uint32		xcnt;			/* # of xact ids in xip[] */
	TransactionId *xip;			/* array of xact IDs in progress */
	/* note: all ids in xip[] satisfy xmin <= xip[i] < xmax */
	CommandId	curcid;			/* in my xact, CID < curcid are visible */
} SnapshotData;

typedef SnapshotData *Snapshot;

/* Special snapshot values: */
#define InvalidSnapshot				((Snapshot) 0x0)	/* same as NULL */
#define SnapshotNow					((Snapshot) 0x1)
#define SnapshotSelf				((Snapshot) 0x2)
#define SnapshotAny					((Snapshot) 0x3)
#define SnapshotToast				((Snapshot) 0x4)

extern DLLIMPORT Snapshot SnapshotDirty;

extern DLLIMPORT Snapshot SerializableSnapshot;
extern DLLIMPORT Snapshot LatestSnapshot;
extern DLLIMPORT Snapshot ActiveSnapshot;

extern TransactionId TransactionXmin;
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
#define HeapTupleSatisfiesVisibility(tuple, snapshot, buffer) \
((snapshot) == SnapshotNow ? \
	HeapTupleSatisfiesNow((tuple)->t_data, buffer) \
: \
	((snapshot) == SnapshotSelf ? \
		HeapTupleSatisfiesItself((tuple)->t_data, buffer) \
	: \
		((snapshot) == SnapshotAny ? \
			true \
		: \
			((snapshot) == SnapshotToast ? \
				HeapTupleSatisfiesToast((tuple)->t_data, buffer) \
			: \
				((snapshot) == SnapshotDirty ? \
					HeapTupleSatisfiesDirty((tuple)->t_data, buffer) \
				: \
					HeapTupleSatisfiesSnapshot((tuple)->t_data, snapshot, buffer) \
				) \
			) \
		) \
	) \
)

/* Result codes for HeapTupleSatisfiesUpdate */
typedef enum
{
	HeapTupleMayBeUpdated,
	HeapTupleInvisible,
	HeapTupleSelfUpdated,
	HeapTupleUpdated,
	HeapTupleBeingUpdated
} HTSU_Result;

/* Result codes for HeapTupleSatisfiesVacuum */
typedef enum
{
	HEAPTUPLE_DEAD,				/* tuple is dead and deletable */
	HEAPTUPLE_LIVE,				/* tuple is live (committed, no deleter) */
	HEAPTUPLE_RECENTLY_DEAD,	/* tuple is dead, but not deletable yet */
	HEAPTUPLE_INSERT_IN_PROGRESS,		/* inserting xact is still in progress */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* deleting xact is still in progress */
} HTSV_Result;

extern bool HeapTupleSatisfiesItself(HeapTupleHeader tuple, Buffer buffer);
extern bool HeapTupleSatisfiesNow(HeapTupleHeader tuple, Buffer buffer);
extern bool HeapTupleSatisfiesDirty(HeapTupleHeader tuple, Buffer buffer);
extern bool HeapTupleSatisfiesToast(HeapTupleHeader tuple, Buffer buffer);
extern bool HeapTupleSatisfiesSnapshot(HeapTupleHeader tuple,
						   Snapshot snapshot, Buffer buffer);
extern HTSU_Result HeapTupleSatisfiesUpdate(HeapTupleHeader tuple,
						 CommandId curcid, Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTupleHeader tuple,
						 TransactionId OldestXmin, Buffer buffer);

extern Snapshot GetTransactionSnapshot(void);
extern Snapshot GetLatestSnapshot(void);
extern Snapshot CopySnapshot(Snapshot snapshot);
extern void FreeSnapshot(Snapshot snapshot);
extern void FreeXactSnapshot(void);

/* in procarray.c; declared here to avoid including tqual.h in procarray.h: */
extern Snapshot GetSnapshotData(Snapshot snapshot, bool serializable);

#endif   /* TQUAL_H */
