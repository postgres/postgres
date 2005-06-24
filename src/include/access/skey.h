/*-------------------------------------------------------------------------
 *
 * skey.h
 *	  POSTGRES scan key definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/skey.h,v 1.29 2005/06/24 00:18:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SKEY_H
#define SKEY_H

#include "access/attnum.h"
#include "fmgr.h"


/*
 * Strategy numbers identify the semantics that particular operators have
 * with respect to particular operator classes.  In some cases a strategy
 * subtype (an OID) is used as further information.
 */
typedef uint16 StrategyNumber;

#define InvalidStrategy ((StrategyNumber) 0)

/*
 * We define the strategy numbers for B-tree indexes here, to avoid having
 * to import access/nbtree.h into a lot of places that shouldn't need it.
 */
#define BTLessStrategyNumber			1
#define BTLessEqualStrategyNumber		2
#define BTEqualStrategyNumber			3
#define BTGreaterEqualStrategyNumber	4
#define BTGreaterStrategyNumber			5

#define BTMaxStrategyNumber				5


/*
 * A ScanKey represents the application of a comparison operator between
 * a table or index column and a constant.	When it's part of an array of
 * ScanKeys, the comparison conditions are implicitly ANDed.  The index
 * column is the left argument of the operator, if it's a binary operator.
 * (The data structure can support unary indexable operators too; in that
 * case sk_argument would go unused.  This is not currently implemented.)
 *
 * For an index scan, sk_strategy and sk_subtype must be set correctly for
 * the operator.  When using a ScanKey in a heap scan, these fields are not
 * used and may be set to InvalidStrategy/InvalidOid.
 *
 * Note: in some places, ScanKeys are used as a convenient representation
 * for the invocation of an access method support procedure.  In this case
 * sk_strategy/sk_subtype are not meaningful, and sk_func may refer to a
 * function that returns something other than boolean.
 */
typedef struct ScanKeyData
{
	int			sk_flags;		/* flags, see below */
	AttrNumber	sk_attno;		/* table or index column number */
	StrategyNumber sk_strategy; /* operator strategy number */
	Oid			sk_subtype;		/* strategy subtype */
	FmgrInfo	sk_func;		/* lookup info for function to call */
	Datum		sk_argument;	/* data to compare */
} ScanKeyData;

typedef ScanKeyData *ScanKey;

/* ScanKeyData sk_flags */
#define SK_ISNULL		0x0001	/* sk_argument is NULL */
#define SK_UNARY		0x0002	/* unary operator (currently unsupported) */
#define SK_NEGATE		0x0004	/* must negate the function result */


/*
 * prototypes for functions in access/common/scankey.c
 */
extern void ScanKeyInit(ScanKey entry,
			AttrNumber attributeNumber,
			StrategyNumber strategy,
			RegProcedure procedure,
			Datum argument);
extern void ScanKeyEntryInitialize(ScanKey entry,
					   int flags,
					   AttrNumber attributeNumber,
					   StrategyNumber strategy,
					   Oid subtype,
					   RegProcedure procedure,
					   Datum argument);
extern void ScanKeyEntryInitializeWithInfo(ScanKey entry,
							   int flags,
							   AttrNumber attributeNumber,
							   StrategyNumber strategy,
							   Oid subtype,
							   FmgrInfo *finfo,
							   Datum argument);

#endif   /* SKEY_H */
