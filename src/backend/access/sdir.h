/*-------------------------------------------------------------------------
 *
 * sdir.h--
 *    POSTGRES scan direction definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sdir.h,v 1.1.1.1 1996/07/09 06:21:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	SDIR_H
#define SDIR_H

#include "c.h"

/*
 * ScanDirection was an int8 for no apparent reason. I kept the original
 * values because I'm not sure if I'll break anything otherwise.  -ay 2/95
 */
typedef enum ScanDirection {
    BackwardScanDirection = -1,
    NoMovementScanDirection = 0,
    ForwardScanDirection = 1
} ScanDirection;

/*
 * ScanDirectionIsValid --
 *	True iff scan direciton is valid.
 */
#define ScanDirectionIsValid(direction) \
    ((bool) (BackwardScanDirection <= direction && \
	     direction <= ForwardScanDirection))

/*
 * ScanDirectionIsBackward --
 *	True iff scan direciton is backward.
 */
#define ScanDirectionIsBackward(direction) \
    ((bool) (direction == BackwardScanDirection))

/*
 * ScanDirectionIsNoMovement --
 *	True iff scan direciton indicates no movement.
 */
#define ScanDirectionIsNoMovement(direction) \
    ((bool) (direction == NoMovementScanDirection))

/*
 * ScanDirectionIsForward --
 *	True iff scan direciton is forward.
 */
#define ScanDirectionIsForward(direction) \
    ((bool) (direction == ForwardScanDirection))

#endif	/* SDIR_H */
