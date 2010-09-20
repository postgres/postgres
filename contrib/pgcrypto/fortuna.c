/*
 * fortuna.c
 *		Fortuna-like PRNG.
 *
 * Copyright (c) 2005 Marko Kreen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/fortuna.c
 */

#include "postgres.h"

#include <sys/time.h>
#include <time.h>

#include "rijndael.h"
#include "sha2.h"
#include "fortuna.h"


/*
 * Why Fortuna-like: There does not seem to be any definitive reference
 * on Fortuna in the net.  Instead this implementation is based on
 * following references:
 *
 * http://en.wikipedia.org/wiki/Fortuna_(PRNG)
 *	 - Wikipedia article
 * http://jlcooke.ca/random/
 *	 - Jean-Luc Cooke Fortuna-based /dev/random driver for Linux.
 */

/*
 * There is some confusion about whether and how to carry forward
 * the state of the pools.	Seems like original Fortuna does not
 * do it, resetting hash after each request.  I guess expecting
 * feeding to happen more often that requesting.   This is absolutely
 * unsuitable for pgcrypto, as nothing asynchronous happens here.
 *
 * J.L. Cooke fixed this by feeding previous hash to new re-initialized
 * hash context.
 *
 * Fortuna predecessor Yarrow requires ability to query intermediate
 * 'final result' from hash, without affecting it.
 *
 * This implementation uses the Yarrow method - asking intermediate
 * results, but continuing with old state.
 */


/*
 * Algorithm parameters
 */

/*
 * How many pools.
 *
 * Original Fortuna uses 32 pools, that means 32'th pool is
 * used not earlier than in 13th year.	This is a waste in
 * pgcrypto, as we have very low-frequancy seeding.  Here
 * is preferable to have all entropy usable in reasonable time.
 *
 * With 23 pools, 23th pool is used after 9 days which seems
 * more sane.
 *
 * In our case the minimal cycle time would be bit longer
 * than the system-randomness feeding frequency.
 */
#define NUM_POOLS		23

/* in microseconds */
#define RESEED_INTERVAL 100000	/* 0.1 sec */

/* for one big request, reseed after this many bytes */
#define RESEED_BYTES	(1024*1024)

/*
 * Skip reseed if pool 0 has less than this many
 * bytes added since last reseed.
 */
#define POOL0_FILL		(256/8)

/*
 * Algorithm constants
 */

/* Both cipher key size and hash result size */
#define BLOCK			32

/* cipher block size */
#define CIPH_BLOCK		16

/* for internal wrappers */
#define MD_CTX			SHA256_CTX
#define CIPH_CTX		rijndael_ctx

struct fortuna_state
{
	uint8		counter[CIPH_BLOCK];
	uint8		result[CIPH_BLOCK];
	uint8		key[BLOCK];
	MD_CTX		pool[NUM_POOLS];
	CIPH_CTX	ciph;
	unsigned	reseed_count;
	struct timeval last_reseed_time;
	unsigned	pool0_bytes;
	unsigned	rnd_pos;
	int			tricks_done;
};
typedef struct fortuna_state FState;


/*
 * Use our own wrappers here.
 * - Need to get intermediate result from digest, without affecting it.
 * - Need re-set key on a cipher context.
 * - Algorithms are guaranteed to exist.
 * - No memory allocations.
 */

static void
ciph_init(CIPH_CTX * ctx, const uint8 *key, int klen)
{
	rijndael_set_key(ctx, (const uint32 *) key, klen, 1);
}

static void
ciph_encrypt(CIPH_CTX * ctx, const uint8 *in, uint8 *out)
{
	rijndael_encrypt(ctx, (const uint32 *) in, (uint32 *) out);
}

static void
md_init(MD_CTX * ctx)
{
	SHA256_Init(ctx);
}

static void
md_update(MD_CTX * ctx, const uint8 *data, int len)
{
	SHA256_Update(ctx, data, len);
}

static void
md_result(MD_CTX * ctx, uint8 *dst)
{
	SHA256_CTX	tmp;

	memcpy(&tmp, ctx, sizeof(*ctx));
	SHA256_Final(dst, &tmp);
	memset(&tmp, 0, sizeof(tmp));
}

/*
 * initialize state
 */
static void
init_state(FState *st)
{
	int			i;

	memset(st, 0, sizeof(*st));
	for (i = 0; i < NUM_POOLS; i++)
		md_init(&st->pool[i]);
}

/*
 * Endianess does not matter.
 * It just needs to change without repeating.
 */
static void
inc_counter(FState *st)
{
	uint32	   *val = (uint32 *) st->counter;

	if (++val[0])
		return;
	if (++val[1])
		return;
	if (++val[2])
		return;
	++val[3];
}

/*
 * This is called 'cipher in counter mode'.
 */
static void
encrypt_counter(FState *st, uint8 *dst)
{
	ciph_encrypt(&st->ciph, st->counter, dst);
	inc_counter(st);
}


/*
 * The time between reseed must be at least RESEED_INTERVAL
 * microseconds.
 */
static int
enough_time_passed(FState *st)
{
	int			ok;
	struct timeval tv;
	struct timeval *last = &st->last_reseed_time;

	gettimeofday(&tv, NULL);

	/* check how much time has passed */
	ok = 0;
	if (tv.tv_sec > last->tv_sec + 1)
		ok = 1;
	else if (tv.tv_sec == last->tv_sec + 1)
	{
		if (1000000 + tv.tv_usec - last->tv_usec >= RESEED_INTERVAL)
			ok = 1;
	}
	else if (tv.tv_usec - last->tv_usec >= RESEED_INTERVAL)
		ok = 1;

	/* reseed will happen, update last_reseed_time */
	if (ok)
		memcpy(last, &tv, sizeof(tv));

	memset(&tv, 0, sizeof(tv));

	return ok;
}

/*
 * generate new key from all the pools
 */
static void
reseed(FState *st)
{
	unsigned	k;
	unsigned	n;
	MD_CTX		key_md;
	uint8		buf[BLOCK];

	/* set pool as empty */
	st->pool0_bytes = 0;

	/*
	 * Both #0 and #1 reseed would use only pool 0. Just skip #0 then.
	 */
	n = ++st->reseed_count;

	/*
	 * The goal: use k-th pool only 1/(2^k) of the time.
	 */
	md_init(&key_md);
	for (k = 0; k < NUM_POOLS; k++)
	{
		md_result(&st->pool[k], buf);
		md_update(&key_md, buf, BLOCK);

		if (n & 1 || !n)
			break;
		n >>= 1;
	}

	/* add old key into mix too */
	md_update(&key_md, st->key, BLOCK);

	/* now we have new key */
	md_result(&key_md, st->key);

	/* use new key */
	ciph_init(&st->ciph, st->key, BLOCK);

	memset(&key_md, 0, sizeof(key_md));
	memset(buf, 0, BLOCK);
}

/*
 * Pick a random pool.	This uses key bytes as random source.
 */
static unsigned
get_rand_pool(FState *st)
{
	unsigned	rnd;

	/*
	 * This slightly prefers lower pools - thats OK.
	 */
	rnd = st->key[st->rnd_pos] % NUM_POOLS;

	st->rnd_pos++;
	if (st->rnd_pos >= BLOCK)
		st->rnd_pos = 0;

	return rnd;
}

/*
 * update pools
 */
static void
add_entropy(FState *st, const uint8 *data, unsigned len)
{
	unsigned	pos;
	uint8		hash[BLOCK];
	MD_CTX		md;

	/* hash given data */
	md_init(&md);
	md_update(&md, data, len);
	md_result(&md, hash);

	/*
	 * Make sure the pool 0 is initialized, then update randomly.
	 */
	if (st->reseed_count == 0)
		pos = 0;
	else
		pos = get_rand_pool(st);
	md_update(&st->pool[pos], hash, BLOCK);

	if (pos == 0)
		st->pool0_bytes += len;

	memset(hash, 0, BLOCK);
	memset(&md, 0, sizeof(md));
}

/*
 * Just take 2 next blocks as new key
 */
static void
rekey(FState *st)
{
	encrypt_counter(st, st->key);
	encrypt_counter(st, st->key + CIPH_BLOCK);
	ciph_init(&st->ciph, st->key, BLOCK);
}

/*
 * Hide public constants. (counter, pools > 0)
 *
 * This can also be viewed as spreading the startup
 * entropy over all of the components.
 */
static void
startup_tricks(FState *st)
{
	int			i;
	uint8		buf[BLOCK];

	/* Use next block as counter. */
	encrypt_counter(st, st->counter);

	/* Now shuffle pools, excluding #0 */
	for (i = 1; i < NUM_POOLS; i++)
	{
		encrypt_counter(st, buf);
		encrypt_counter(st, buf + CIPH_BLOCK);
		md_update(&st->pool[i], buf, BLOCK);
	}
	memset(buf, 0, BLOCK);

	/* Hide the key. */
	rekey(st);

	/* This can be done only once. */
	st->tricks_done = 1;
}

static void
extract_data(FState *st, unsigned count, uint8 *dst)
{
	unsigned	n;
	unsigned	block_nr = 0;

	/* Should we reseed? */
	if (st->pool0_bytes >= POOL0_FILL || st->reseed_count == 0)
		if (enough_time_passed(st))
			reseed(st);

	/* Do some randomization on first call */
	if (!st->tricks_done)
		startup_tricks(st);

	while (count > 0)
	{
		/* produce bytes */
		encrypt_counter(st, st->result);

		/* copy result */
		if (count > CIPH_BLOCK)
			n = CIPH_BLOCK;
		else
			n = count;
		memcpy(dst, st->result, n);
		dst += n;
		count -= n;

		/* must not give out too many bytes with one key */
		block_nr++;
		if (block_nr > (RESEED_BYTES / CIPH_BLOCK))
		{
			rekey(st);
			block_nr = 0;
		}
	}
	/* Set new key for next request. */
	rekey(st);
}

/*
 * public interface
 */

static FState main_state;
static int	init_done = 0;

void
fortuna_add_entropy(const uint8 *data, unsigned len)
{
	if (!init_done)
	{
		init_state(&main_state);
		init_done = 1;
	}
	if (!data || !len)
		return;
	add_entropy(&main_state, data, len);
}

void
fortuna_get_bytes(unsigned len, uint8 *dst)
{
	if (!init_done)
	{
		init_state(&main_state);
		init_done = 1;
	}
	if (!dst || !len)
		return;
	extract_data(&main_state, len, dst);
}
