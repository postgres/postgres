/*-------------------------------------------------------------------------
 *
 * bit.c
 *	  Standard bit array code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/lib/Attic/bit.c,v 1.11 2000/08/26 21:53:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/bit.h"


void
BitArraySetBit(BitArray bitArray, BitIndex bitIndex)
{
	bitArray[bitIndex / BITS_PER_BYTE] |=
		(1 << (BITS_PER_BYTE - 1 - (bitIndex % BITS_PER_BYTE)));
}

void
BitArrayClearBit(BitArray bitArray, BitIndex bitIndex)
{
	bitArray[bitIndex / BITS_PER_BYTE] &=
		~(1 << (BITS_PER_BYTE - 1 - (bitIndex % BITS_PER_BYTE)));
}

bool
BitArrayBitIsSet(BitArray bitArray, BitIndex bitIndex)
{
	return ((bitArray[bitIndex / BITS_PER_BYTE] &
			 (1 << (BITS_PER_BYTE - 1 - (bitIndex % BITS_PER_BYTE)))
		) != 0);
}
