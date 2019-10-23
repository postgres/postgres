/*
 * pgp-encrypt.c
 *	  OpenPGP encrypt.
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
 * contrib/pgcrypto/pgp-encrypt.c
 */

#include "postgres.h"

#include <time.h>

#include "mbuf.h"
#include "pgp.h"
#include "px.h"

#define MDC_DIGEST_LEN 20
#define STREAM_ID 0xE0
#define STREAM_BLOCK_SHIFT	14

static uint8 *
render_newlen(uint8 *h, int len)
{
	if (len <= 191)
	{
		*h++ = len & 255;
	}
	else if (len > 191 && len <= 8383)
	{
		*h++ = ((len - 192) >> 8) + 192;
		*h++ = (len - 192) & 255;
	}
	else
	{
		*h++ = 255;
		*h++ = (len >> 24) & 255;
		*h++ = (len >> 16) & 255;
		*h++ = (len >> 8) & 255;
		*h++ = len & 255;
	}
	return h;
}

static int
write_tag_only(PushFilter *dst, int tag)
{
	uint8		hdr = 0xC0 | tag;

	return pushf_write(dst, &hdr, 1);
}

static int
write_normal_header(PushFilter *dst, int tag, int len)
{
	uint8		hdr[8];
	uint8	   *h = hdr;

	*h++ = 0xC0 | tag;
	h = render_newlen(h, len);
	return pushf_write(dst, hdr, h - hdr);
}


/*
 * MAC writer
 */

static int
mdc_init(PushFilter *dst, void *init_arg, void **priv_p)
{
	int			res;
	PX_MD	   *md;

	res = pgp_load_digest(PGP_DIGEST_SHA1, &md);
	if (res < 0)
		return res;

	*priv_p = md;
	return 0;
}

static int
mdc_write(PushFilter *dst, void *priv, const uint8 *data, int len)
{
	PX_MD	   *md = priv;

	px_md_update(md, data, len);
	return pushf_write(dst, data, len);
}

static int
mdc_flush(PushFilter *dst, void *priv)
{
	int			res;
	uint8		pkt[2 + MDC_DIGEST_LEN];
	PX_MD	   *md = priv;

	/*
	 * create mdc pkt
	 */
	pkt[0] = 0xD3;
	pkt[1] = 0x14;				/* MDC_DIGEST_LEN */
	px_md_update(md, pkt, 2);
	px_md_finish(md, pkt + 2);

	res = pushf_write(dst, pkt, 2 + MDC_DIGEST_LEN);
	px_memset(pkt, 0, 2 + MDC_DIGEST_LEN);
	return res;
}

static void
mdc_free(void *priv)
{
	PX_MD	   *md = priv;

	px_md_free(md);
}

static const PushFilterOps mdc_filter = {
	mdc_init, mdc_write, mdc_flush, mdc_free
};


/*
 * Encrypted pkt writer
 */
#define ENCBUF 8192
struct EncStat
{
	PGP_CFB    *ciph;
	uint8		buf[ENCBUF];
};

static int
encrypt_init(PushFilter *next, void *init_arg, void **priv_p)
{
	struct EncStat *st;
	PGP_Context *ctx = init_arg;
	PGP_CFB    *ciph;
	int			resync = 1;
	int			res;

	/* should we use newer packet format? */
	if (ctx->disable_mdc == 0)
	{
		uint8		ver = 1;

		resync = 0;
		res = pushf_write(next, &ver, 1);
		if (res < 0)
			return res;
	}
	res = pgp_cfb_create(&ciph, ctx->cipher_algo,
						 ctx->sess_key, ctx->sess_key_len, resync, NULL);
	if (res < 0)
		return res;

	st = px_alloc(sizeof(*st));
	memset(st, 0, sizeof(*st));
	st->ciph = ciph;

	*priv_p = st;
	return ENCBUF;
}

static int
encrypt_process(PushFilter *next, void *priv, const uint8 *data, int len)
{
	int			res;
	struct EncStat *st = priv;
	int			avail = len;

	while (avail > 0)
	{
		int			tmplen = avail > ENCBUF ? ENCBUF : avail;

		res = pgp_cfb_encrypt(st->ciph, data, tmplen, st->buf);
		if (res < 0)
			return res;

		res = pushf_write(next, st->buf, tmplen);
		if (res < 0)
			return res;

		data += tmplen;
		avail -= tmplen;
	}
	return 0;
}

static void
encrypt_free(void *priv)
{
	struct EncStat *st = priv;

	if (st->ciph)
		pgp_cfb_free(st->ciph);
	px_memset(st, 0, sizeof(*st));
	px_free(st);
}

static const PushFilterOps encrypt_filter = {
	encrypt_init, encrypt_process, NULL, encrypt_free
};

/*
 * Write Streamable pkts
 */

struct PktStreamStat
{
	int			final_done;
	int			pkt_block;
};

static int
pkt_stream_init(PushFilter *next, void *init_arg, void **priv_p)
{
	struct PktStreamStat *st;

	st = px_alloc(sizeof(*st));
	st->final_done = 0;
	st->pkt_block = 1 << STREAM_BLOCK_SHIFT;
	*priv_p = st;

	return st->pkt_block;
}

static int
pkt_stream_process(PushFilter *next, void *priv, const uint8 *data, int len)
{
	int			res;
	uint8		hdr[8];
	uint8	   *h = hdr;
	struct PktStreamStat *st = priv;

	if (st->final_done)
		return PXE_BUG;

	if (len == st->pkt_block)
		*h++ = STREAM_ID | STREAM_BLOCK_SHIFT;
	else
	{
		h = render_newlen(h, len);
		st->final_done = 1;
	}

	res = pushf_write(next, hdr, h - hdr);
	if (res < 0)
		return res;

	return pushf_write(next, data, len);
}

static int
pkt_stream_flush(PushFilter *next, void *priv)
{
	int			res;
	uint8		hdr[8];
	uint8	   *h = hdr;
	struct PktStreamStat *st = priv;

	/* stream MUST end with normal packet. */
	if (!st->final_done)
	{
		h = render_newlen(h, 0);
		res = pushf_write(next, hdr, h - hdr);
		if (res < 0)
			return res;
		st->final_done = 1;
	}
	return 0;
}

static void
pkt_stream_free(void *priv)
{
	struct PktStreamStat *st = priv;

	px_memset(st, 0, sizeof(*st));
	px_free(st);
}

static const PushFilterOps pkt_stream_filter = {
	pkt_stream_init, pkt_stream_process, pkt_stream_flush, pkt_stream_free
};

int
pgp_create_pkt_writer(PushFilter *dst, int tag, PushFilter **res_p)
{
	int			res;

	res = write_tag_only(dst, tag);
	if (res < 0)
		return res;

	return pushf_create(res_p, &pkt_stream_filter, NULL, dst);
}

/*
 * Text conversion filter
 */

static int
crlf_process(PushFilter *dst, void *priv, const uint8 *data, int len)
{
	const uint8 *data_end = data + len;
	const uint8 *p2,
			   *p1 = data;
	int			line_len;
	static const uint8 crlf[] = {'\r', '\n'};
	int			res = 0;

	while (p1 < data_end)
	{
		p2 = memchr(p1, '\n', data_end - p1);
		if (p2 == NULL)
			p2 = data_end;

		line_len = p2 - p1;

		/* write data */
		res = 0;
		if (line_len > 0)
		{
			res = pushf_write(dst, p1, line_len);
			if (res < 0)
				break;
			p1 += line_len;
		}

		/* write crlf */
		while (p1 < data_end && *p1 == '\n')
		{
			res = pushf_write(dst, crlf, 2);
			if (res < 0)
				break;
			p1++;
		}
	}
	return res;
}

static const PushFilterOps crlf_filter = {
	NULL, crlf_process, NULL, NULL
};

/*
 * Initialize literal data packet
 */
static int
init_litdata_packet(PushFilter **pf_res, PGP_Context *ctx, PushFilter *dst)
{
	int			res;
	int			hdrlen;
	uint8		hdr[6];
	uint32		t;
	PushFilter *pkt;
	int			type;

	/*
	 * Create header
	 */

	if (ctx->text_mode)
		type = ctx->unicode_mode ? 'u' : 't';
	else
		type = 'b';

	/*
	 * Store the creation time into packet. The goal is to have as few known
	 * bytes as possible.
	 */
	t = (uint32) time(NULL);

	hdr[0] = type;
	hdr[1] = 0;
	hdr[2] = (t >> 24) & 255;
	hdr[3] = (t >> 16) & 255;
	hdr[4] = (t >> 8) & 255;
	hdr[5] = t & 255;
	hdrlen = 6;

	res = write_tag_only(dst, PGP_PKT_LITERAL_DATA);
	if (res < 0)
		return res;

	res = pushf_create(&pkt, &pkt_stream_filter, ctx, dst);
	if (res < 0)
		return res;

	res = pushf_write(pkt, hdr, hdrlen);
	if (res < 0)
	{
		pushf_free(pkt);
		return res;
	}

	*pf_res = pkt;
	return 0;
}

/*
 * Initialize compression filter
 */
static int
init_compress(PushFilter **pf_res, PGP_Context *ctx, PushFilter *dst)
{
	int			res;
	uint8		type = ctx->compress_algo;
	PushFilter *pkt;

	res = write_tag_only(dst, PGP_PKT_COMPRESSED_DATA);
	if (res < 0)
		return res;

	res = pushf_create(&pkt, &pkt_stream_filter, ctx, dst);
	if (res < 0)
		return res;

	res = pushf_write(pkt, &type, 1);
	if (res >= 0)
		res = pgp_compress_filter(pf_res, ctx, pkt);

	if (res < 0)
		pushf_free(pkt);

	return res;
}

/*
 * Initialize encdata packet
 */
static int
init_encdata_packet(PushFilter **pf_res, PGP_Context *ctx, PushFilter *dst)
{
	int			res;
	int			tag;

	if (ctx->disable_mdc)
		tag = PGP_PKT_SYMENCRYPTED_DATA;
	else
		tag = PGP_PKT_SYMENCRYPTED_DATA_MDC;

	res = write_tag_only(dst, tag);
	if (res < 0)
		return res;

	return pushf_create(pf_res, &pkt_stream_filter, ctx, dst);
}

/*
 * write prefix
 */
static int
write_prefix(PGP_Context *ctx, PushFilter *dst)
{
	uint8		prefix[PGP_MAX_BLOCK + 2];
	int			res,
				bs;

	bs = pgp_get_cipher_block_size(ctx->cipher_algo);
	if (!pg_strong_random(prefix, bs))
		return PXE_NO_RANDOM;

	prefix[bs + 0] = prefix[bs - 2];
	prefix[bs + 1] = prefix[bs - 1];

	res = pushf_write(dst, prefix, bs + 2);
	px_memset(prefix, 0, bs + 2);
	return res < 0 ? res : 0;
}

/*
 * write symmetrically encrypted session key packet
 */

static int
symencrypt_sesskey(PGP_Context *ctx, uint8 *dst)
{
	int			res;
	PGP_CFB    *cfb;
	uint8		algo = ctx->cipher_algo;

	res = pgp_cfb_create(&cfb, ctx->s2k_cipher_algo,
						 ctx->s2k.key, ctx->s2k.key_len, 0, NULL);
	if (res < 0)
		return res;

	pgp_cfb_encrypt(cfb, &algo, 1, dst);
	pgp_cfb_encrypt(cfb, ctx->sess_key, ctx->sess_key_len, dst + 1);

	pgp_cfb_free(cfb);
	return ctx->sess_key_len + 1;
}

/* 5.3: Symmetric-Key Encrypted Session-Key */
static int
write_symenc_sesskey(PGP_Context *ctx, PushFilter *dst)
{
	uint8		pkt[256];
	int			pktlen;
	int			res;
	uint8	   *p = pkt;

	*p++ = 4;					/* 5.3 - version number  */
	*p++ = ctx->s2k_cipher_algo;

	*p++ = ctx->s2k.mode;
	*p++ = ctx->s2k.digest_algo;
	if (ctx->s2k.mode > 0)
	{
		memcpy(p, ctx->s2k.salt, 8);
		p += 8;
	}
	if (ctx->s2k.mode == 3)
		*p++ = ctx->s2k.iter;

	if (ctx->use_sess_key)
	{
		res = symencrypt_sesskey(ctx, p);
		if (res < 0)
			return res;
		p += res;
	}

	pktlen = p - pkt;
	res = write_normal_header(dst, PGP_PKT_SYMENCRYPTED_SESSKEY, pktlen);
	if (res >= 0)
		res = pushf_write(dst, pkt, pktlen);

	px_memset(pkt, 0, pktlen);
	return res;
}

/*
 * key setup
 */
static int
init_s2k_key(PGP_Context *ctx)
{
	int			res;

	if (ctx->s2k_cipher_algo < 0)
		ctx->s2k_cipher_algo = ctx->cipher_algo;

	res = pgp_s2k_fill(&ctx->s2k, ctx->s2k_mode, ctx->s2k_digest_algo, ctx->s2k_count);
	if (res < 0)
		return res;

	return pgp_s2k_process(&ctx->s2k, ctx->s2k_cipher_algo,
						   ctx->sym_key, ctx->sym_key_len);
}

static int
init_sess_key(PGP_Context *ctx)
{
	if (ctx->use_sess_key || ctx->pub_key)
	{
		ctx->sess_key_len = pgp_get_cipher_key_size(ctx->cipher_algo);
		if (!pg_strong_random(ctx->sess_key, ctx->sess_key_len))
			return PXE_NO_RANDOM;
	}
	else
	{
		ctx->sess_key_len = ctx->s2k.key_len;
		memcpy(ctx->sess_key, ctx->s2k.key, ctx->s2k.key_len);
	}

	return 0;
}

/*
 * combine
 */
int
pgp_encrypt(PGP_Context *ctx, MBuf *src, MBuf *dst)
{
	int			res;
	int			len;
	uint8	   *buf;
	PushFilter *pf,
			   *pf_tmp;

	/*
	 * do we have any key
	 */
	if (!ctx->sym_key && !ctx->pub_key)
		return PXE_ARGUMENT_ERROR;

	/* MBuf writer */
	res = pushf_create_mbuf_writer(&pf, dst);
	if (res < 0)
		goto out;

	/*
	 * initialize sym_key
	 */
	if (ctx->sym_key)
	{
		res = init_s2k_key(ctx);
		if (res < 0)
			goto out;
	}

	res = init_sess_key(ctx);
	if (res < 0)
		goto out;

	/*
	 * write keypkt
	 */
	if (ctx->pub_key)
		res = pgp_write_pubenc_sesskey(ctx, pf);
	else
		res = write_symenc_sesskey(ctx, pf);
	if (res < 0)
		goto out;

	/* encrypted data pkt */
	res = init_encdata_packet(&pf_tmp, ctx, pf);
	if (res < 0)
		goto out;
	pf = pf_tmp;

	/* encrypter */
	res = pushf_create(&pf_tmp, &encrypt_filter, ctx, pf);
	if (res < 0)
		goto out;
	pf = pf_tmp;

	/* hasher */
	if (ctx->disable_mdc == 0)
	{
		res = pushf_create(&pf_tmp, &mdc_filter, ctx, pf);
		if (res < 0)
			goto out;
		pf = pf_tmp;
	}

	/* prefix */
	res = write_prefix(ctx, pf);
	if (res < 0)
		goto out;

	/* compressor */
	if (ctx->compress_algo > 0 && ctx->compress_level > 0)
	{
		res = init_compress(&pf_tmp, ctx, pf);
		if (res < 0)
			goto out;
		pf = pf_tmp;
	}

	/* data streamer */
	res = init_litdata_packet(&pf_tmp, ctx, pf);
	if (res < 0)
		goto out;
	pf = pf_tmp;


	/* text conversion? */
	if (ctx->text_mode && ctx->convert_crlf)
	{
		res = pushf_create(&pf_tmp, &crlf_filter, ctx, pf);
		if (res < 0)
			goto out;
		pf = pf_tmp;
	}

	/*
	 * chain complete
	 */

	len = mbuf_grab(src, mbuf_avail(src), &buf);
	res = pushf_write(pf, buf, len);
	if (res >= 0)
		res = pushf_flush(pf);
out:
	pushf_free_all(pf);
	return res;
}
