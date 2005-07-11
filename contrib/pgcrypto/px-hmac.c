/*
 * px-hmac.c
 *		HMAC implementation.
 *
 * Copyright (c) 2001 Marko Kreen
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
 * $PostgreSQL: pgsql/contrib/pgcrypto/px-hmac.c,v 1.7 2005/07/11 15:07:59 tgl Exp $
 */

#include "postgres.h"

#include "px.h"

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C

static unsigned
hmac_result_size(PX_HMAC * h)
{
	return px_md_result_size(h->md);
}

static unsigned
hmac_block_size(PX_HMAC * h)
{
	return px_md_block_size(h->md);
}

static void
hmac_init(PX_HMAC * h, const uint8 *key, unsigned klen)
{
	unsigned	bs,
				hlen,
				i;
	uint8	   *keybuf;
	PX_MD	   *md = h->md;

	bs = px_md_block_size(md);
	hlen = px_md_result_size(md);
	keybuf = px_alloc(bs);
	memset(keybuf, 0, bs);

	if (klen > bs)
	{
		px_md_update(md, key, klen);
		px_md_finish(md, keybuf);
		px_md_reset(md);
	}
	else
		memcpy(keybuf, key, klen);

	for (i = 0; i < bs; i++)
	{
		h->p.ipad[i] = keybuf[i] ^ HMAC_IPAD;
		h->p.opad[i] = keybuf[i] ^ HMAC_OPAD;
	}

	memset(keybuf, 0, bs);
	px_free(keybuf);

	px_md_update(md, h->p.ipad, bs);
}

static void
hmac_reset(PX_HMAC * h)
{
	PX_MD	   *md = h->md;
	unsigned	bs = px_md_block_size(md);

	px_md_reset(md);
	px_md_update(md, h->p.ipad, bs);
}

static void
hmac_update(PX_HMAC * h, const uint8 *data, unsigned dlen)
{
	px_md_update(h->md, data, dlen);
}

static void
hmac_finish(PX_HMAC * h, uint8 *dst)
{
	PX_MD	   *md = h->md;
	unsigned	bs,
				hlen;
	uint8	   *buf;

	bs = px_md_block_size(md);
	hlen = px_md_result_size(md);

	buf = px_alloc(hlen);

	px_md_finish(md, buf);

	px_md_reset(md);
	px_md_update(md, h->p.opad, bs);
	px_md_update(md, buf, hlen);
	px_md_finish(md, dst);

	memset(buf, 0, hlen);
	px_free(buf);
}

static void
hmac_free(PX_HMAC * h)
{
	unsigned	bs;

	bs = px_md_block_size(h->md);
	px_md_free(h->md);

	memset(h->p.ipad, 0, bs);
	memset(h->p.opad, 0, bs);
	px_free(h->p.ipad);
	px_free(h->p.opad);
	px_free(h);
}


/* PUBLIC FUNCTIONS */

int
px_find_hmac(const char *name, PX_HMAC ** res)
{
	int			err;
	PX_MD	   *md;
	PX_HMAC    *h;
	unsigned	bs;

	err = px_find_digest(name, &md);
	if (err)
		return err;

	bs = px_md_block_size(md);
	if (bs < 2)
	{
		px_md_free(md);
		return PXE_HASH_UNUSABLE_FOR_HMAC;
	}

	h = px_alloc(sizeof(*h));
	h->p.ipad = px_alloc(bs);
	h->p.opad = px_alloc(bs);
	h->md = md;

	h->result_size = hmac_result_size;
	h->block_size = hmac_block_size;
	h->reset = hmac_reset;
	h->update = hmac_update;
	h->finish = hmac_finish;
	h->free = hmac_free;
	h->init = hmac_init;

	*res = h;

	return 0;
}
