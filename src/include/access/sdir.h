/*-------------------------------------------------------------------------
 *
 * sdir.h
 *	  POSTGRES scan direction definitions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sdir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SDIR_H
#define SDIR_H


/*
 * Defines the direction for scanning a table or an index.  Scans are never
 * invoked using NoMovementScanDirectionScans.  For convenience, we use the
 * values -1 and 1 for backward and forward scans.  This allows us to perform
 * a few mathematical tricks such as what is done in ScanDirectionCombine.
 */
typedef enum ScanDirection
{
	BackwardScanDirection = -1,
	NoMovementScanDirection = 0,
	ForwardScanDirection = 1
} ScanDirection;

/*
 * Determine the net effect of two direction specifications.
 * This relies on having ForwardScanDirection = +1, BackwardScanDirection = -1,
 * and will probably not do what you want if applied to any other values.
 */
#define ScanDirectionCombine(a, b)  ((a) * (b))

/*
 * ScanDirectionIsValid
 *		True iff scan direction is valid.
 */
#define ScanDirectionIsValid(direction) \
	((bool) (BackwardScanDirection <= (direction) && \
			 (direction) <= ForwardScanDirection))

/*
 * ScanDirectionIsBackward
 *		True iff scan direction is backward.
 */
#define ScanDirectionIsBackward(direction) \
	((bool) ((direction) == BackwardScanDirection))

/*
 * ScanDirectionIsNoMovement
 *		True iff scan direction indicates no movement.
 */
#define ScanDirectionIsNoMovement(direction) \
	((bool) ((direction) == NoMovementScanDirection))

/*
 * ScanDirectionIsForward
 *		True iff scan direction is forward.
 */
#define ScanDirectionIsForward(direction) \
	((bool) ((direction) == ForwardScanDirection))

#endif							/* SDIR_H */
