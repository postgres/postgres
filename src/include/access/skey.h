/*-------------------------------------------------------------------------
 *
 * skey.h--
 *	  POSTGRES scan key definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: skey.h,v 1.7 1998/01/15 19:46:18 pgsql Exp $
 *
 *
 * Note:
 *		Needs more accessor/assignment routines.
 *-------------------------------------------------------------------------
 */
#ifndef SKEY_H
#define SKEY_H

#include <access/attnum.h>
#include <fmgr.h>

typedef struct ScanKeyData
{
	bits16		sk_flags;		/* flags */
	AttrNumber	sk_attno;		/* domain number */
	RegProcedure sk_procedure;	/* procedure OID */
	FmgrInfo	sk_func;
	int32		sk_nargs;
	Datum		sk_argument;	/* data to compare */
} ScanKeyData;

typedef ScanKeyData *ScanKey;


#define SK_ISNULL		0x1
#define SK_UNARY		0x2
#define SK_NEGATE		0x4
#define SK_COMMUTE		0x8

#define ScanUnmarked			0x01
#define ScanUncheckedPrevious	0x02
#define ScanUncheckedNext		0x04


/*
 * prototypes for functions in access/common/scankey.c
 */
extern void ScanKeyEntrySetIllegal(ScanKey entry);
extern void
ScanKeyEntryInitialize(ScanKey entry, bits16 flags,
	 AttrNumber attributeNumber, RegProcedure procedure, Datum argument);

#endif							/* SKEY_H */
