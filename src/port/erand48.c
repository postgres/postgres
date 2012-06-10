/*-------------------------------------------------------------------------
 *
 * erand48.c
 *
 * This file supplies pg_erand48(), pg_lrand48(), and pg_srand48(), which
 * are just like erand48(), lrand48(), and srand48() except that we use
 * our own implementation rather than the one provided by the operating
 * system.	We used to test for an operating system version rather than
 * unconditionally using our own, but (1) some versions of Cygwin have a
 * buggy erand48() that always returns zero and (2) as of 2011, glibc's
 * erand48() is strangely coded to be almost-but-not-quite thread-safe,
 * which doesn't matter for the backend but is important for pgbench.
 *
 *
 * Copyright (c) 1993 Martin Birgmeier
 * All rights reserved.
 *
 * You may redistribute unmodified or modified versions of this source
 * code provided that the above copyright notice and this and the
 * following conditions are retained.
 *
 * This software is provided ``as is'', and comes with no warranties
 * of any kind. I shall in no event be liable for anything that happens
 * to anyone/anything when using this software.
 *
 * IDENTIFICATION
 *	  src/port/erand48.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <math.h>

#define RAND48_SEED_0	(0x330e)
#define RAND48_SEED_1	(0xabcd)
#define RAND48_SEED_2	(0x1234)
#define RAND48_MULT_0	(0xe66d)
#define RAND48_MULT_1	(0xdeec)
#define RAND48_MULT_2	(0x0005)
#define RAND48_ADD		(0x000b)

static unsigned short _rand48_seed[3] = {
	RAND48_SEED_0,
	RAND48_SEED_1,
	RAND48_SEED_2
};
static unsigned short _rand48_mult[3] = {
	RAND48_MULT_0,
	RAND48_MULT_1,
	RAND48_MULT_2
};
static unsigned short _rand48_add = RAND48_ADD;


static void
_dorand48(unsigned short xseed[3])
{
	unsigned long accu;
	unsigned short temp[2];

	accu = (unsigned long) _rand48_mult[0] * (unsigned long) xseed[0] +
		(unsigned long) _rand48_add;
	temp[0] = (unsigned short) accu;	/* lower 16 bits */
	accu >>= sizeof(unsigned short) * 8;
	accu += (unsigned long) _rand48_mult[0] * (unsigned long) xseed[1] +
		(unsigned long) _rand48_mult[1] * (unsigned long) xseed[0];
	temp[1] = (unsigned short) accu;	/* middle 16 bits */
	accu >>= sizeof(unsigned short) * 8;
	accu += _rand48_mult[0] * xseed[2] + _rand48_mult[1] * xseed[1] + _rand48_mult[2] * xseed[0];
	xseed[0] = temp[0];
	xseed[1] = temp[1];
	xseed[2] = (unsigned short) accu;
}


double
pg_erand48(unsigned short xseed[3])
{
	_dorand48(xseed);
	return ldexp((double) xseed[0], -48) +
		ldexp((double) xseed[1], -32) +
		ldexp((double) xseed[2], -16);
}

long
pg_lrand48(void)
{
	_dorand48(_rand48_seed);
	return ((long) _rand48_seed[2] << 15) + ((long) _rand48_seed[1] >> 1);
}

void
pg_srand48(long seed)
{
	_rand48_seed[0] = RAND48_SEED_0;
	_rand48_seed[1] = (unsigned short) seed;
	_rand48_seed[2] = (unsigned short) (seed >> 16);
	_rand48_mult[0] = RAND48_MULT_0;
	_rand48_mult[1] = RAND48_MULT_1;
	_rand48_mult[2] = RAND48_MULT_2;
	_rand48_add = RAND48_ADD;
}
