/*-------------------------------------------------------------------------
 *
 * tupmacs.h--
 *    Tuple macros used by both index tuples and heap tuples.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tupmacs.h,v 1.1.1.1 1996/07/09 06:21:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPMACS_H
#define TUPMACS_H

/*
 * check to see if the ATT'th bit of an array of 8-bit bytes is set.
 */
#define att_isnull(ATT, BITS) (!((BITS)[(ATT) >> 3] & (1 << ((ATT) & 0x07))))

/*
 * given a AttributeTupleForm and a pointer into a tuple's data
 * area, return the correct value or pointer.
 *
 * note that T must already be properly LONGALIGN/SHORTALIGN'd for
 * this to work correctly.
 *
 * the double-cast is to stop gcc from (correctly) complaining about 
 * casting integer types with size < sizeof(char *) to (char *).
 * sign-extension may get weird if you use an integer type that
 * isn't the same size as (char *) for the first cast.  (on the other
 * hand, it's safe to use another type for the (foo *)(T).)
 */
#define fetchatt(A, T) \
 ((*(A))->attbyval \
  ? ((*(A))->attlen > sizeof(int16) \
     ? (char *) (long) *((int32 *)(T)) \
     : ((*(A))->attlen < sizeof(int16) \
        ? (char *) (long) *((char *)(T)) \
        : (char *) (long) *((int16 *)(T)))) \
  : (char *) (T))
	
#endif
