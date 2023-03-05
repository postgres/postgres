/*-------------------------------------------------------------------------
 *
 * sampling.h
 *	  definitions for sampling functions
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sampling.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SAMPLING_H
#define SAMPLING_H

#include "common/pg_prng.h"
#include "storage/block.h"		/* for typedef BlockNumber */


/* Random generator for sampling code */
extern void sampler_random_init_state(uint32 seed,
									  pg_prng_state *randstate);
extern double sampler_random_fract(pg_prng_state *randstate);

/* Block sampling methods */

/* Data structure for Algorithm S from Knuth 3.4.2 */
typedef struct
{
	BlockNumber N;				/* number of blocks, known in advance */
	int			n;				/* desired sample size */
	BlockNumber t;				/* current block number */
	int			m;				/* blocks selected so far */
	pg_prng_state randstate;	/* random generator state */
} BlockSamplerData;

typedef BlockSamplerData *BlockSampler;

extern BlockNumber BlockSampler_Init(BlockSampler bs, BlockNumber nblocks,
									 int samplesize, uint32 randseed);
extern bool BlockSampler_HasMore(BlockSampler bs);
extern BlockNumber BlockSampler_Next(BlockSampler bs);

/* Reservoir sampling methods */

typedef struct
{
	double		W;
	pg_prng_state randstate;	/* random generator state */
} ReservoirStateData;

typedef ReservoirStateData *ReservoirState;

extern void reservoir_init_selection_state(ReservoirState rs, int n);
extern double reservoir_get_next_S(ReservoirState rs, double t, int n);

/* Old API, still in use by assorted FDWs */
/* For backwards compatibility, these declarations are duplicated in vacuum.h */

extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

#endif							/* SAMPLING_H */
