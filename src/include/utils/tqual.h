/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *	  POSTGRES "time" qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.12 1998/04/24 14:43:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include <access/htup.h>

extern TransactionId HeapSpecialTransactionId;
extern CommandId HeapSpecialCommandId;

/*
 * HeapTupleSatisfiesVisibility --
 *		True iff heap tuple satsifies a time qual.
 *
 * Note:
 *		Assumes heap tuple is valid.
 */
#define HeapTupleSatisfiesVisibility(tuple, seeself) \
( \
	TransactionIdEquals((tuple)->t_xmax, AmiTransactionId) ? \
		false \
	: \
	( \
		((seeself) == true || heapisoverride()) ? \
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
