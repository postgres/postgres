/*-------------------------------------------------------------------------
 *
 * skey.h
 *	  POSTGRES scan key definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: skey.h,v 1.22 2003/08/04 02:40:10 momjian Exp $
 *
 * Note:
 *		Needs more accessor/assignment routines.
 *-------------------------------------------------------------------------
 */
#ifndef SKEY_H
#define SKEY_H

#include "access/attnum.h"
#include "fmgr.h"


typedef struct ScanKeyData
{
	bits16		sk_flags;		/* flags */
	AttrNumber	sk_attno;		/* domain number */
	RegProcedure sk_procedure;	/* procedure OID */
	FmgrInfo	sk_func;		/* fmgr call info for procedure */
	Datum		sk_argument;	/* data to compare */
} ScanKeyData;

typedef ScanKeyData *ScanKey;

/* ScanKeyData flags */
#define SK_ISNULL		0x1		/* sk_argument is NULL */
#define SK_UNARY		0x2		/* unary function (currently unsupported) */
#define SK_NEGATE		0x4		/* negate function result */
#define SK_COMMUTE		0x8		/* commute function (not fully supported) */


/*
 * prototypes for functions in access/common/scankey.c
 */
extern void ScanKeyEntrySetIllegal(ScanKey entry);
extern void ScanKeyEntryInitialize(ScanKey entry, bits16 flags,
	 AttrNumber attributeNumber, RegProcedure procedure, Datum argument);
extern void ScanKeyEntryInitializeWithInfo(ScanKey entry, bits16 flags,
							 AttrNumber attributeNumber, FmgrInfo *finfo,
							   MemoryContext mcxt, Datum argument);

#endif   /* SKEY_H */
