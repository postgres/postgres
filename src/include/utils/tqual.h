/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *	  POSTGRES "time" qualification definitions.
 *
 *	  Should be moved/renamed...	- vadim 07/28/98
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.17 1998/12/16 11:52:11 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include <access/htup.h>

typedef struct SnapshotData
{
	TransactionId	xmin;			/* XID < xmin are visible to me */
	TransactionId	xmax;			/* XID >= xmax are invisible to me */
	uint32			xcnt;			/* # of xact below */
	TransactionId  *xip;			/* array of xacts in progress */
}			SnapshotData;

typedef SnapshotData *Snapshot;

#define SnapshotNow					((Snapshot) 0x0)
#define SnapshotSelf				((Snapshot) 0x1)

extern	Snapshot					SnapshotDirty;
extern	Snapshot					QuerySnapshot;

#define IsSnapshotNow(snapshot)		((Snapshot) snapshot == SnapshotNow)
#define IsSnapshotSelf(snapshot)	((Snapshot) snapshot == SnapshotSelf)
#define IsSnapshotDirty(snapshot)	((Snapshot) snapshot == SnapshotDirty)

extern TransactionId HeapSpecialTransactionId;
extern CommandId HeapSpecialCommandId;

/*
 * HeapTupleSatisfiesVisibility --
 *		True iff heap tuple satsifies a time qual.
 *
 * Note:
 *		Assumes heap tuple is valid.
 */
#define HeapTupleSatisfiesVisibility(tuple, snapshot) \
( \
	TransactionIdEquals((tuple)->t_data->t_xmax, AmiTransactionId) ? \
		false \
	: \
	( \
		(IsSnapshotSelf(snapshot) || heapisoverride()) ? \
			HeapTupleSatisfiesItself((tuple)->t_data) \
		: \
			((IsSnapshotDirty(snapshot)) ? \
				HeapTupleSatisfiesDirty((tuple)->t_data) \
			: \
				((IsSnapshotNow(snapshot)) ? \
					HeapTupleSatisfiesNow((tuple)->t_data) \
				: \
					HeapTupleSatisfiesSnapshot((tuple)->t_data, snapshot) \
				) \
			) \
	) \
)

#define heapisoverride() \
( \
	(!TransactionIdIsValid(HeapSpecialTransactionId)) ? \
		false \
	: \
	( \
		(!TransactionIdEquals(GetCurrentTransactionId(), \
							 HeapSpecialTransactionId) || \
		 GetCurrentCommandId() != HeapSpecialCommandId) ? \
		( \
			HeapSpecialTransactionId = InvalidTransactionId, \
			false \
		) \
		: \
			true \
	) \
)

#define	HeapTupleMayBeUpdated		0
#define	HeapTupleInvisible			1
#define HeapTupleSelfUpdated		2
#define HeapTupleUpdated			3
#define HeapTupleBeingUpdated		4

extern bool		HeapTupleSatisfiesItself(HeapTupleHeader tuple);
extern bool		HeapTupleSatisfiesNow(HeapTupleHeader tuple);
extern bool		HeapTupleSatisfiesDirty(HeapTupleHeader tuple);
extern bool		HeapTupleSatisfiesSnapshot(HeapTupleHeader tuple, Snapshot snapshot);
extern int		HeapTupleSatisfiesUpdate(HeapTuple tuple);

extern void setheapoverride(bool on);

extern Snapshot GetSnapshotData(void);
extern void		SetQuerySnapshot(void);
extern void		FreeXactSnapshot(void);

#endif	 /* TQUAL_H */
