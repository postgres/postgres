/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *	  POSTGRES "time" qualification definitions.
 *
 *	  Should be moved/renamed...	- vadim 07/28/98
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.13 1998/07/27 19:38:40 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include <access/htup.h>

typedef struct SnapshotData
{
	TransactionId		xmin;				/* XID < xmin are visible to me */
	TransactionId		xmax;				/* XID > xmax are invisible to me */
	TransactionId	   *xip;				/* array of xacts in progress */
} SnapshotData;

typedef SnapshotData	*Snapshot;

#define	IsSnapshotNow(snapshot)		((Snapshot) snapshot == (Snapshot) 0x0)
#define	IsSnapshotSelf(snapshot)	((Snapshot) snapshot == (Snapshot) 0x1)
#define	SnapshotNow					((Snapshot) 0x0)
#define	SnapshotSelf				((Snapshot) 0x1)

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
	TransactionIdEquals((tuple)->t_xmax, AmiTransactionId) ? \
		false \
	: \
	( \
		(IsSnapshotSelf(snapshot) || heapisoverride()) ? \
			HeapTupleSatisfiesItself(tuple) \
		: \
			HeapTupleSatisfiesNow(tuple) \
	) \
)

#define	heapisoverride() \
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

extern bool HeapTupleSatisfiesItself(HeapTuple tuple);
extern bool HeapTupleSatisfiesNow(HeapTuple tuple);

extern void setheapoverride(bool on);


#endif							/* TQUAL_H */
