/*-------------------------------------------------------------------------
 *
 * bit.h--
 *    Standard bit array definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bit.h,v 1.1.1.1 1996/07/09 06:22:01 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	BIT_H
#define BIT_H

typedef bits8	*BitArray;
typedef uint32	BitIndex;

#define BitsPerByte	8

/*
 * BitArraySetBit --
 *	Sets (to 1) the value of a bit in a bit array.
 */
extern void BitArraySetBit(BitArray bitArray, BitIndex bitIndex);

/*
 * BitArrayClearBit --
 *	Clears (to 0) the value of a bit in a bit array.
 */
extern void BitArrayClearBit(BitArray bitArray, BitIndex bitIndex);

/*
 * BitArrayBitIsSet --
 *	True iff the bit is set (1) in a bit array.
 */
extern bool BitArrayBitIsSet(BitArray bitArray, BitIndex bitIndex);

#endif	/* BIT_H */
