/*
 * pgp-s2k.c
 *	  OpenPGP string2key functions.
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/pgp-s2k.c
 */

#include "postgres.h"

#include "px.h"
#include "pgp.h"

static int
calc_s2k_simple(PGP_S2K *s2k, PX_MD *md, const uint8 *key,
				unsigned key_len)
{
	unsigned	md_rlen;
	uint8		buf[PGP_MAX_DIGEST];
	unsigned	preload;
	unsigned	remain;
	uint8	   *dst = s2k->key;

	md_rlen = px_md_result_size(md);

	remain = s2k->key_len;
	preload = 0;
	while (remain > 0)
	{
		px_md_reset(md);

		if (preload)
		{
			memset(buf, 0, preload);
			px_md_update(md, buf, preload);
		}
		preload++;

		px_md_update(md, key, key_len);
		px_md_finish(md, buf);

		if (remain > md_rlen)
		{
			memcpy(dst, buf, md_rlen);
			dst += md_rlen;
			remain -= md_rlen;
		}
		else
		{
			memcpy(dst, buf, remain);
			remain = 0;
		}
	}
	px_memset(buf, 0, sizeof(buf));
	return 0;
}

static int
calc_s2k_salted(PGP_S2K *s2k, PX_MD *md, const uint8 *key, unsigned key_len)
{
	unsigned	md_rlen;
	uint8		buf[PGP_MAX_DIGEST];
	unsigned	preload = 0;
	uint8	   *dst;
	unsigned	remain;

	md_rlen = px_md_result_size(md);

	dst = s2k->key;
	remain = s2k->key_len;
	while (remain > 0)
	{
		px_md_reset(md);

		if (preload > 0)
		{
			memset(buf, 0, preload);
			px_md_update(md, buf, preload);
		}
		preload++;

		px_md_update(md, s2k->salt, PGP_S2K_SALT);
		px_md_update(md, key, key_len);
		px_md_finish(md, buf);

		if (remain > md_rlen)
		{
			memcpy(dst, buf, md_rlen);
			remain -= md_rlen;
			dst += md_rlen;
		}
		else
		{
			memcpy(dst, buf, remain);
			remain = 0;
		}
	}
	px_memset(buf, 0, sizeof(buf));
	return 0;
}

static int
calc_s2k_iter_salted(PGP_S2K *s2k, PX_MD *md, const uint8 *key,
					 unsigned key_len)
{
	unsigned	md_rlen;
	uint8		buf[PGP_MAX_DIGEST];
	uint8	   *dst;
	unsigned	preload = 0;
	unsigned	remain,
				c,
				curcnt,
				count;

	count = s2k_decode_count(s2k->iter);

	md_rlen = px_md_result_size(md);

	remain = s2k->key_len;
	dst = s2k->key;
	while (remain > 0)
	{
		px_md_reset(md);

		if (preload)
		{
			memset(buf, 0, preload);
			px_md_update(md, buf, preload);
		}
		preload++;

		px_md_update(md, s2k->salt, PGP_S2K_SALT);
		px_md_update(md, key, key_len);
		curcnt = PGP_S2K_SALT + key_len;

		while (curcnt < count)
		{
			if (curcnt + PGP_S2K_SALT < count)
				c = PGP_S2K_SALT;
			else
				c = count - curcnt;
			px_md_update(md, s2k->salt, c);
			curcnt += c;

			if (curcnt + key_len < count)
				c = key_len;
			else if (curcnt < count)
				c = count - curcnt;
			else
				break;
			px_md_update(md, key, c);
			curcnt += c;
		}
		px_md_finish(md, buf);

		if (remain > md_rlen)
		{
			memcpy(dst, buf, md_rlen);
			remain -= md_rlen;
			dst += md_rlen;
		}
		else
		{
			memcpy(dst, buf, remain);
			remain = 0;
		}
	}
	px_memset(buf, 0, sizeof(buf));
	return 0;
}

/*
 * Decide PGP_S2K_ISALTED iteration count (in OpenPGP one-byte representation)
 *
 * Too small: weak
 * Too big: slow
 * gpg defaults to 96 => 65536 iters
 *
 * For our default (count=-1) we let it float a bit: 96 + 32 => between 65536
 * and 262144 iterations.
 *
 * Otherwise, find the smallest number which provides at least the specified
 * iteration count.
 */
static uint8
decide_s2k_iter(unsigned rand_byte, int count)
{
	int			iter;

	if (count == -1)
		return 96 + (rand_byte & 0x1F);
	/* this is a bit brute-force, but should be quick enough */
	for (iter = 0; iter <= 255; iter++)
		if (s2k_decode_count(iter) >= count)
			return iter;
	return 255;
}

int
pgp_s2k_fill(PGP_S2K *s2k, int mode, int digest_algo, int count)
{
	int			res = 0;
	uint8		tmp;

	s2k->mode = mode;
	s2k->digest_algo = digest_algo;

	switch (s2k->mode)
	{
		case PGP_S2K_SIMPLE:
			break;
		case PGP_S2K_SALTED:
			res = px_get_random_bytes(s2k->salt, PGP_S2K_SALT);
			break;
		case PGP_S2K_ISALTED:
			res = px_get_random_bytes(s2k->salt, PGP_S2K_SALT);
			if (res < 0)
				break;
			res = px_get_random_bytes(&tmp, 1);
			if (res < 0)
				break;
			s2k->iter = decide_s2k_iter(tmp, count);
			break;
		default:
			res = PXE_PGP_BAD_S2K_MODE;
	}
	return res;
}

int
pgp_s2k_read(PullFilter *src, PGP_S2K *s2k)
{
	int			res = 0;

	GETBYTE(src, s2k->mode);
	GETBYTE(src, s2k->digest_algo);
	switch (s2k->mode)
	{
		case 0:
			break;
		case 1:
			res = pullf_read_fixed(src, 8, s2k->salt);
			break;
		case 3:
			res = pullf_read_fixed(src, 8, s2k->salt);
			if (res < 0)
				break;
			GETBYTE(src, s2k->iter);
			break;
		default:
			res = PXE_PGP_BAD_S2K_MODE;
	}
	return res;
}

int
pgp_s2k_process(PGP_S2K *s2k, int cipher, const uint8 *key, int key_len)
{
	int			res;
	PX_MD	   *md;

	s2k->key_len = pgp_get_cipher_key_size(cipher);
	if (s2k->key_len <= 0)
		return PXE_PGP_UNSUPPORTED_CIPHER;

	res = pgp_load_digest(s2k->digest_algo, &md);
	if (res < 0)
		return res;

	switch (s2k->mode)
	{
		case 0:
			res = calc_s2k_simple(s2k, md, key, key_len);
			break;
		case 1:
			res = calc_s2k_salted(s2k, md, key, key_len);
			break;
		case 3:
			res = calc_s2k_iter_salted(s2k, md, key, key_len);
			break;
		default:
			res = PXE_PGP_BAD_S2K_MODE;
	}
	px_md_free(md);
	return res;
}
