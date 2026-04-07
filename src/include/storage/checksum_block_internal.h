/*-------------------------------------------------------------------------
 *
 * checksum_block_internal.h
 *	  Core algorithm for page checksums, semi-private to checksum_impl.h
 *	  and checksum.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum_block_internal.h
 *
 *-------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef CHECKSUM_BLOCK_INTERNAL_H here */

uint32		sums[N_SUMS];
uint32		result = 0;
uint32		i,
			j;

/* ensure that the size is compatible with the algorithm */
Assert(sizeof(PGChecksummablePage) == BLCKSZ);

/* initialize partial checksums to their corresponding offsets */
memcpy(sums, checksumBaseOffsets, sizeof(checksumBaseOffsets));

/* main checksum calculation */
for (i = 0; i < (uint32) (BLCKSZ / (sizeof(uint32) * N_SUMS)); i++)
	for (j = 0; j < N_SUMS; j++)
		CHECKSUM_COMP(sums[j], page->data[i][j]);

/* finally add in two rounds of zeroes for additional mixing */
for (i = 0; i < 2; i++)
	for (j = 0; j < N_SUMS; j++)
		CHECKSUM_COMP(sums[j], 0);

/* xor fold partial checksums together */
for (i = 0; i < N_SUMS; i++)
	result ^= sums[i];

return result;
