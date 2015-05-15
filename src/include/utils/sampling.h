/*-------------------------------------------------------------------------
 *
 * sampling.h
 *	  definitions for sampling functions
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sampling.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SAMPLING_H
#define SAMPLING_H

#include "storage/bufmgr.h"

extern double sampler_random_fract(void);

/* Block sampling methods */
/* Data structure for Algorithm S from Knuth 3.4.2 */
typedef struct
{
	BlockNumber N;				/* number of blocks, known in advance */
	int			n;				/* desired sample size */
	BlockNumber t;				/* current block number */
	int			m;				/* blocks selected so far */
} BlockSamplerData;

typedef BlockSamplerData *BlockSampler;

extern void BlockSampler_Init(BlockSampler bs, BlockNumber nblocks,
				  int samplesize, long randseed);
extern bool BlockSampler_HasMore(BlockSampler bs);
extern BlockNumber BlockSampler_Next(BlockSampler bs);

/* Reservoid sampling methods */
typedef double ReservoirStateData;
typedef ReservoirStateData *ReservoirState;

extern void reservoir_init_selection_state(ReservoirState rs, int n);
extern double reservoir_get_next_S(ReservoirState rs, double t, int n);

#endif /* SAMPLING_H */
