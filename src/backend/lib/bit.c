/*-------------------------------------------------------------------------
 *
 * bit.c--
 *    Standard bit array code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/lib/Attic/bit.c,v 1.1.1.1 1996/07/09 06:21:28 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * utils/memutils.h contains declarations of the functions in this file
 */
#include "utils/memutils.h"	

void
BitArraySetBit(BitArray bitArray, BitIndex bitIndex)
{	
    bitArray[bitIndex/BitsPerByte]
	|= (1 << (BitsPerByte - (bitIndex % BitsPerByte) - 1));
    return;
}

void
BitArrayClearBit(BitArray bitArray, BitIndex bitIndex)
{
    bitArray[bitIndex/BitsPerByte]
	&= ~(1 << (BitsPerByte - (bitIndex % BitsPerByte) - 1));
    return;
}

bool
BitArrayBitIsSet(BitArray bitArray, BitIndex bitIndex)
{	
    return( (bool) (((bitArray[bitIndex / BitsPerByte] &
		      (1 << (BitsPerByte - (bitIndex % BitsPerByte)
			     - 1)
		       )
		      ) != 0 ) ? 1 : 0) );
}

