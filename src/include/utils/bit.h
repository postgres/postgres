/*-------------------------------------------------------------------------
 *
 * bit.h
 *	  Standard bit array definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bit.h,v 1.9 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BIT_H
#define BIT_H

typedef bits8 *BitArray;
typedef uint32 BitIndex;

/*
 * BitArraySetBit
 *		Sets (to 1) the value of a bit in a bit array.
 */
extern void BitArraySetBit(BitArray bitArray, BitIndex bitIndex);

/*
 * BitArrayClearBit
 *		Clears (to 0) the value of a bit in a bit array.
 */
extern void BitArrayClearBit(BitArray bitArray, BitIndex bitIndex);

/*
 * BitArrayBitIsSet
 *		True iff the bit is set (1) in a bit array.
 */
extern bool BitArrayBitIsSet(BitArray bitArray, BitIndex bitIndex);

#endif	 /* BIT_H */
