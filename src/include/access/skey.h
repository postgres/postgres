/*-------------------------------------------------------------------------
 *
 * skey.h
 *	  POSTGRES scan key definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/skey.h,v 1.33 2006/10/04 00:30:07 momjian Exp $
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

/*
 * About row comparisons:
 *
 * The ScanKey data structure also supports row comparisons, that is ordered
 * tuple comparisons like (x, y) > (c1, c2), having the SQL-spec semantics
 * "x > c1 OR (x = c1 AND y > c2)".  Note that this is currently only
 * implemented for btree index searches, not for heapscans or any other index
 * type.  A row comparison is represented by a "header" ScanKey entry plus
 * a separate array of ScanKeys, one for each column of the row comparison.
 * The header entry has these properties:
 *		sk_flags = SK_ROW_HEADER
 *		sk_attno = index column number for leading column of row comparison
 *		sk_strategy = btree strategy code for semantics of row comparison
 *				(ie, < <= > or >=)
 *		sk_subtype, sk_func: not used
 *		sk_argument: pointer to subsidiary ScanKey array
 * If the header is part of a ScanKey array that's sorted by attno, it
 * must be sorted according to the leading column number.
 *
 * The subsidiary ScanKey array appears in logical column order of the row
 * comparison, which may be different from index column order.	The array
 * elements are like a normal ScanKey array except that:
 *		sk_flags must include SK_ROW_MEMBER, plus SK_ROW_END in the last
 *				element (needed since row header does not include a count)
 *		sk_func points to the btree comparison support function for the
 *				opclass, NOT the operator's implementation function.
 * sk_strategy must be the same in all elements of the subsidiary array,
 * that is, the same as in the header entry.
 */

/*
 * ScanKeyData sk_flags
 *
 * sk_flags bits 0-15 are reserved for system-wide use (symbols for those
 * bits should be defined here).  Bits 16-31 are reserved for use within
 * individual index access methods.
 */
#define SK_ISNULL		0x0001	/* sk_argument is NULL */
#define SK_UNARY		0x0002	/* unary operator (currently unsupported) */
#define SK_ROW_HEADER	0x0004	/* row comparison header (see above) */
#define SK_ROW_MEMBER	0x0008	/* row comparison member (see above) */
#define SK_ROW_END		0x0010	/* last row comparison member (see above) */


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
