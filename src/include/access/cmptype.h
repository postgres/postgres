/*-------------------------------------------------------------------------
 *
 * cmptype.h
 *	  POSTGRES compare type definitions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/cmptype.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CMPTYPE_H
#define CMPTYPE_H

/*
 * CompareType - fundamental semantics of certain operators
 *
 * These enum symbols represent the fundamental semantics of certain operators
 * that the system needs to have some hardcoded knowledge about.  (For
 * example, RowCompareExpr needs to know which operators can be determined to
 * act like =, <>, <, etc.)  Index access methods map (some of) strategy
 * numbers to these values so that the system can know about the meaning of
 * (some of) the operators without needing hardcoded knowledge of index AM's
 * strategy numbering.
 *
 * XXX Currently, this mapping is not fully developed and most values are
 * chosen to match btree strategy numbers, which is not going to work very
 * well for other access methods.
 */
typedef enum CompareType
{
	COMPARE_INVALID = 0,
	COMPARE_LT = 1,				/* BTLessStrategyNumber */
	COMPARE_LE = 2,				/* BTLessEqualStrategyNumber */
	COMPARE_EQ = 3,				/* BTEqualStrategyNumber */
	COMPARE_GE = 4,				/* BTGreaterEqualStrategyNumber */
	COMPARE_GT = 5,				/* BTGreaterStrategyNumber */
	COMPARE_NE = 6,				/* no such btree strategy */
	COMPARE_OVERLAP,
	COMPARE_CONTAINED_BY,
} CompareType;

#endif							/* CMPTYPE_H */
