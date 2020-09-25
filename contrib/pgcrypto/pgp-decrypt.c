/*
 * pgp-decrypt.c
 *	  OpenPGP decrypt.
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
 * contrib/pgcrypto/pgp-decrypt.c
 */

#include "postgres.h"

#include "mbuf.h"
#include "pgp.h"
#include "px.h"

#define NO_CTX_SIZE		0
#define ALLOW_CTX_SIZE	1
#define NO_COMPR		0
#define ALLOW_COMPR		1
#define NO_MDC			0
#define NEED_MDC		1

#define PKT_NORMAL 1
#define PKT_STREAM 2
#define PKT_CONTEXT 3

#define MAX_CHUNK (16*1024*1024)

static int
parse_new_len(PullFilter *src, int *len_p)
{
	uint8		b;
	int			len;
	int			pkttype = PKT_NORMAL;

	GETBYTE(src, b);
	if (b <= 191)
		len = b;
	else if (b >= 192 && b <= 223)
	{
		len = ((unsigned) (b) - 192) << 8;
		GETBYTE(src, b);
		len += 192 + b;
	}
	else if (b == 255)
	{
		GETBYTE(src, b);
		len = b;
		GETBYTE(src, b);
		len = (len << 8) | b;
		GETBYTE(src, b);
		len = (len << 8) | b;
		GETBYTE(src, b);
		len = (len << 8) | b;
	}
	else
	{
		len = 1 << (b & 0x1F);
		pkttype = PKT_STREAM;
	}

	if (len < 0 || len > MAX_CHUNK)
	{
		px_debug("parse_new_len: weird length");
		return PXE_PGP_CORRUPT_DATA;
	}

	*len_p = len;
	return pkttype;
}

static int
parse_old_len(PullFilter *src, int *len_p, int lentype)
{
	uint8		b;
	int			len;

	GETBYTE(src, b);
	len = b;

	if (lentype == 1)
	{
		GETBYTE(src, b);
		len = (len << 8) | b;
	}
	else if (lentype == 2)
	{
		GETBYTE(src, b);
		len = (len << 8) | b;
		GETBYTE(src, b);
		len = (len << 8) | b;
		GETBYTE(src, b);
		len = (len << 8) | b;
	}

	if (len < 0 || len > MAX_CHUNK)
	{
		px_debug("parse_old_len: weird length");
		return PXE_PGP_CORRUPT_DATA;
	}
	*len_p = len;
	return PKT_NORMAL;
}

/* returns pkttype or 0 on eof */
int
pgp_parse_pkt_hdr(PullFilter *src, uint8 *tag, int *len_p, int allow_ctx)
{
	int			lentype;
	int			res;
	uint8	   *p;

	/* EOF is normal here, thus we don't use GETBYTE */
	res = pullf_read(src, 1, &p);
	if (res < 0)
		return res;
	if (res == 0)
		return 0;

	if ((*p & 0x80) == 0)
	{
		px_debug("pgp_parse_pkt_hdr: not pkt hdr");
		return PXE_PGP_CORRUPT_DATA;
	}

	if (*p & 0x40)
	{
		*tag = *p & 0x3f;
		res = parse_new_len(src, len_p);
	}
	else
	{
		lentype = *p & 3;
		*tag = (*p >> 2) & 0x0F;
		if (lentype == 3)
			res = allow_ctx ? PKT_CONTEXT : PXE_PGP_CORRUPT_DATA;
		else
			res = parse_old_len(src, len_p, lentype);
	}
	return res;
}

/*
 * Packet reader
 */
struct PktData
{
	int			type;
	int			len;
};

static int
pktreader_pull(void *priv, PullFilter *src, int len,
			   uint8 **data_p, uint8 *buf, int buflen)
{
	int			res;
	struct PktData *pkt = priv;

	/* PKT_CONTEXT means: whatever there is */
	if (pkt->type == PKT_CONTEXT)
		return pullf_read(src, len, data_p);

	while (pkt->len == 0)
	{
		/* this was last chunk in stream */
		if (pkt->type == PKT_NORMAL)
			return 0;

		/* next chunk in stream */
		res = parse_new_len(src, &pkt->len);
		if (res < 0)
			return res;
		pkt->type = res;
	}

	if (len > pkt->len)
		len = pkt->len;

	res = pullf_read(src, len, data_p);
	if (res > 0)
		pkt->len -= res;

	return res;
}

static void
pktreader_free(void *priv)
{
	struct PktData *pkt = priv;

	px_memset(pkt, 0, sizeof(*pkt));
	pfree(pkt);
}

static struct PullFilterOps pktreader_filter = {
	NULL, pktreader_pull, pktreader_free
};

/* needs helper function to pass several parameters */
int
pgp_create_pkt_reader(PullFilter **pf_p, PullFilter *src, int len,
					  int pkttype, PGP_Context *ctx)
{
	int			res;
	struct PktData *pkt = palloc(sizeof(*pkt));

	pkt->type = pkttype;
	pkt->len = len;
	res = pullf_create(pf_p, &pktreader_filter, pkt, src);
	if (res < 0)
		pfree(pkt);
	return res;
}

/*
 * Prefix check filter
 * https://tools.ietf.org/html/rfc4880#section-5.7
 * https://tools.ietf.org/html/rfc4880#section-5.13
 */

static int
prefix_init(void **priv_p, void *arg, PullFilter *src)
{
	PGP_Context *ctx = arg;
	int			len;
	int			res;
	uint8	   *buf;
	uint8		tmpbuf[PGP_MAX_BLOCK + 2];

	len = pgp_get_cipher_block_size(ctx->cipher_algo);
	if (len > sizeof(tmpbuf))
		return PXE_BUG;

	res = pullf_read_max(src, len + 2, &buf, tmpbuf);
	if (res < 0)
		return res;
	if (res != len + 2)
	{
		px_debug("prefix_init: short read");
		px_memset(tmpbuf, 0, sizeof(tmpbuf));
		return PXE_PGP_CORRUPT_DATA;
	}

	if (buf[len - 2] != buf[len] || buf[len - 1] != buf[len + 1])
	{
		px_debug("prefix_init: corrupt prefix");
		/* report error in pgp_decrypt() */
		ctx->corrupt_prefix = 1;
	}
	px_memset(tmpbuf, 0, sizeof(tmpbuf));
	return 0;
}

static struct PullFilterOps prefix_filter = {
	prefix_init, NULL, NULL
};


/*
 * Decrypt filter
 */

static int
decrypt_init(void **priv_p, void *arg, PullFilter *src)
{
	PGP_CFB    *cfb = arg;

	*priv_p = cfb;

	/* we need to write somewhere, so ask for a buffer */
	return 4096;
}

static int
decrypt_read(void *priv, PullFilter *src, int len,
			 uint8 **data_p, uint8 *buf, int buflen)
{
	PGP_CFB    *cfb = priv;
	uint8	   *tmp;
	int			res;

	res = pullf_read(src, len, &tmp);
	if (res > 0)
	{
		pgp_cfb_decrypt(cfb, tmp, res, buf);
		*data_p = buf;
	}
	return res;
}

struct PullFilterOps pgp_decrypt_filter = {
	decrypt_init, decrypt_read, NULL
};


/*
 * MDC hasher filter
 */

static int
mdc_init(void **priv_p, void *arg, PullFilter *src)
{
	PGP_Context *ctx = arg;

	*priv_p = ctx;
	return pgp_load_digest(PGP_DIGEST_SHA1, &ctx->mdc_ctx);
}

static void
mdc_free(void *priv)
{
	PGP_Context *ctx = priv;

	if (ctx->use_mdcbuf_filter)
		return;
	px_md_free(ctx->mdc_ctx);
	ctx->mdc_ctx = NULL;
}

static int
mdc_finish(PGP_Context *ctx, PullFilter *src, int len)
{
	int			res;
	uint8		hash[20];
	uint8		tmpbuf[20];
	uint8	   *data;

	/* should not happen */
	if (ctx->use_mdcbuf_filter)
		return PXE_BUG;

	/* It's SHA1 */
	if (len != 20)
		return PXE_PGP_CORRUPT_DATA;

	/* mdc_read should not call px_md_update */
	ctx->in_mdc_pkt = 1;

	/* read data */
	res = pullf_read_max(src, len, &data, tmpbuf);
	if (res < 0)
		return res;
	if (res == 0)
	{
		px_debug("no mdc");
		return PXE_PGP_CORRUPT_DATA;
	}

	/* is the packet sane? */
	if (res != 20)
	{
		px_debug("mdc_finish: read failed, res=%d", res);
		return PXE_PGP_CORRUPT_DATA;
	}

	/*
	 * ok, we got the hash, now check
	 */
	px_md_finish(ctx->mdc_ctx, hash);
	res = memcmp(hash, data, 20);
	px_memset(hash, 0, 20);
	px_memset(tmpbuf, 0, sizeof(tmpbuf));
	if (res != 0)
	{
		px_debug("mdc_finish: mdc failed");
		return PXE_PGP_CORRUPT_DATA;
	}
	ctx->mdc_checked = 1;
	return 0;
}

static int
mdc_read(void *priv, PullFilter *src, int len,
		 uint8 **data_p, uint8 *buf, int buflen)
{
	int			res;
	PGP_Context *ctx = priv;

	/* skip this filter? */
	if (ctx->use_mdcbuf_filter || ctx->in_mdc_pkt)
		return pullf_read(src, len, data_p);

	res = pullf_read(src, len, data_p);
	if (res < 0)
		return res;
	if (res == 0)
	{
		px_debug("mdc_read: unexpected eof");
		return PXE_PGP_CORRUPT_DATA;
	}
	px_md_update(ctx->mdc_ctx, *data_p, res);

	return res;
}

static struct PullFilterOps mdc_filter = {
	mdc_init, mdc_read, mdc_free
};


/*
 * Combined Pkt reader and MDC hasher.
 *
 * For the case of SYMENCRYPTED_DATA_MDC packet, where
 * the data part has 'context length', which means
 * that data packet ends 22 bytes before end of parent
 * packet, which is silly.
 */
#define MDCBUF_LEN 8192
struct MDCBufData
{
	PGP_Context *ctx;
	int			eof;
	int			buflen;
	int			avail;
	uint8	   *pos;
	int			mdc_avail;
	uint8		mdc_buf[22];
	uint8		buf[MDCBUF_LEN];
};

static int
mdcbuf_init(void **priv_p, void *arg, PullFilter *src)
{
	PGP_Context *ctx = arg;
	struct MDCBufData *st;

	st = palloc0(sizeof(*st));
	st->buflen = sizeof(st->buf);
	st->ctx = ctx;
	*priv_p = st;

	/* take over the work of mdc_filter */
	ctx->use_mdcbuf_filter = 1;

	return 0;
}

static int
mdcbuf_finish(struct MDCBufData *st)
{
	uint8		hash[20];
	int			res;

	st->eof = 1;

	if (st->mdc_buf[0] != 0xD3 || st->mdc_buf[1] != 0x14)
	{
		px_debug("mdcbuf_finish: bad MDC pkt hdr");
		return PXE_PGP_CORRUPT_DATA;
	}
	px_md_update(st->ctx->mdc_ctx, st->mdc_buf, 2);
	px_md_finish(st->ctx->mdc_ctx, hash);
	res = memcmp(hash, st->mdc_buf + 2, 20);
	px_memset(hash, 0, 20);
	if (res)
	{
		px_debug("mdcbuf_finish: MDC does not match");
		res = PXE_PGP_CORRUPT_DATA;
	}
	return res;
}

static void
mdcbuf_load_data(struct MDCBufData *st, uint8 *src, int len)
{
	uint8	   *dst = st->pos + st->avail;

	memcpy(dst, src, len);
	px_md_update(st->ctx->mdc_ctx, src, len);
	st->avail += len;
}

static void
mdcbuf_load_mdc(struct MDCBufData *st, uint8 *src, int len)
{
	memmove(st->mdc_buf + st->mdc_avail, src, len);
	st->mdc_avail += len;
}

static int
mdcbuf_refill(struct MDCBufData *st, PullFilter *src)
{
	uint8	   *data;
	int			res;
	int			need;

	/* put avail data in start */
	if (st->avail > 0 && st->pos != st->buf)
		memmove(st->buf, st->pos, st->avail);
	st->pos = st->buf;

	/* read new data */
	need = st->buflen + 22 - st->avail - st->mdc_avail;
	res = pullf_read(src, need, &data);
	if (res < 0)
		return res;
	if (res == 0)
		return mdcbuf_finish(st);

	/* add to buffer */
	if (res >= 22)
	{
		mdcbuf_load_data(st, st->mdc_buf, st->mdc_avail);
		st->mdc_avail = 0;

		mdcbuf_load_data(st, data, res - 22);
		mdcbuf_load_mdc(st, data + res - 22, 22);
	}
	else
	{
		int			canmove = st->mdc_avail + res - 22;

		if (canmove > 0)
		{
			mdcbuf_load_data(st, st->mdc_buf, canmove);
			st->mdc_avail -= canmove;
			memmove(st->mdc_buf, st->mdc_buf + canmove, st->mdc_avail);
		}
		mdcbuf_load_mdc(st, data, res);
	}
	return 0;
}

static int
mdcbuf_read(void *priv, PullFilter *src, int len,
			uint8 **data_p, uint8 *buf, int buflen)
{
	struct MDCBufData *st = priv;
	int			res;

	if (!st->eof && len > st->avail)
	{
		res = mdcbuf_refill(st, src);
		if (res < 0)
			return res;
	}

	if (len > st->avail)
		len = st->avail;

	*data_p = st->pos;
	st->pos += len;
	st->avail -= len;
	return len;
}

static void
mdcbuf_free(void *priv)
{
	struct MDCBufData *st = priv;

	px_md_free(st->ctx->mdc_ctx);
	st->ctx->mdc_ctx = NULL;
	px_memset(st, 0, sizeof(*st));
	pfree(st);
}

static struct PullFilterOps mdcbuf_filter = {
	mdcbuf_init, mdcbuf_read, mdcbuf_free
};


/*
 * Decrypt separate session key
 */
static int
decrypt_key(PGP_Context *ctx, const uint8 *src, int len)
{
	int			res;
	uint8		algo;
	PGP_CFB    *cfb;

	res = pgp_cfb_create(&cfb, ctx->s2k_cipher_algo,
						 ctx->s2k.key, ctx->s2k.key_len, 0, NULL);
	if (res < 0)
		return res;

	pgp_cfb_decrypt(cfb, src, 1, &algo);
	src++;
	len--;

	pgp_cfb_decrypt(cfb, src, len, ctx->sess_key);
	pgp_cfb_free(cfb);
	ctx->sess_key_len = len;
	ctx->cipher_algo = algo;

	if (pgp_get_cipher_key_size(algo) != len)
	{
		px_debug("sesskey bad len: algo=%d, expected=%d, got=%d",
				 algo, pgp_get_cipher_key_size(algo), len);
		return PXE_PGP_CORRUPT_DATA;
	}
	return 0;
}

/*
 * Handle key packet
 */
static int
parse_symenc_sesskey(PGP_Context *ctx, PullFilter *src)
{
	uint8	   *p;
	int			res;
	uint8		tmpbuf[PGP_MAX_KEY + 2];
	uint8		ver;

	GETBYTE(src, ver);
	GETBYTE(src, ctx->s2k_cipher_algo);
	if (ver != 4)
	{
		px_debug("bad key pkt ver");
		return PXE_PGP_CORRUPT_DATA;
	}

	/*
	 * read S2K info
	 */
	res = pgp_s2k_read(src, &ctx->s2k);
	if (res < 0)
		return res;
	ctx->s2k_mode = ctx->s2k.mode;
	ctx->s2k_count = s2k_decode_count(ctx->s2k.iter);
	ctx->s2k_digest_algo = ctx->s2k.digest_algo;

	/*
	 * generate key from password
	 */
	res = pgp_s2k_process(&ctx->s2k, ctx->s2k_cipher_algo,
						  ctx->sym_key, ctx->sym_key_len);
	if (res < 0)
		return res;

	/*
	 * do we have separate session key?
	 */
	res = pullf_read_max(src, PGP_MAX_KEY + 2, &p, tmpbuf);
	if (res < 0)
		return res;

	if (res == 0)
	{
		/*
		 * no, s2k key is session key
		 */
		memcpy(ctx->sess_key, ctx->s2k.key, ctx->s2k.key_len);
		ctx->sess_key_len = ctx->s2k.key_len;
		ctx->cipher_algo = ctx->s2k_cipher_algo;
		res = 0;
		ctx->use_sess_key = 0;
	}
	else
	{
		/*
		 * yes, decrypt it
		 */
		if (res < 17 || res > PGP_MAX_KEY + 1)
		{
			px_debug("expect key, but bad data");
			return PXE_PGP_CORRUPT_DATA;
		}
		ctx->use_sess_key = 1;
		res = decrypt_key(ctx, p, res);
	}

	px_memset(tmpbuf, 0, sizeof(tmpbuf));
	return res;
}

static int
copy_crlf(MBuf *dst, uint8 *data, int len, int *got_cr)
{
	uint8	   *data_end = data + len;
	uint8		tmpbuf[1024];
	uint8	   *tmp_end = tmpbuf + sizeof(tmpbuf);
	uint8	   *p;
	int			res;

	p = tmpbuf;
	if (*got_cr)
	{
		if (*data != '\n')
			*p++ = '\r';
		*got_cr = 0;
	}
	while (data < data_end)
	{
		if (*data == '\r')
		{
			if (data + 1 < data_end)
			{
				if (*(data + 1) == '\n')
					data++;
			}
			else
			{
				*got_cr = 1;
				break;
			}
		}
		*p++ = *data++;
		if (p >= tmp_end)
		{
			res = mbuf_append(dst, tmpbuf, p - tmpbuf);
			if (res < 0)
				return res;
			p = tmpbuf;
		}
	}
	if (p - tmpbuf > 0)
	{
		res = mbuf_append(dst, tmpbuf, p - tmpbuf);
		if (res < 0)
			return res;
	}
	px_memset(tmpbuf, 0, sizeof(tmpbuf));
	return 0;
}

static int
parse_literal_data(PGP_Context *ctx, MBuf *dst, PullFilter *pkt)
{
	int			type;
	int			name_len;
	int			res;
	uint8	   *buf;
	uint8		tmpbuf[4];
	int			got_cr = 0;

	GETBYTE(pkt, type);
	GETBYTE(pkt, name_len);

	/* skip name */
	while (name_len > 0)
	{
		res = pullf_read(pkt, name_len, &buf);
		if (res < 0)
			return res;
		if (res == 0)
			break;
		name_len -= res;
	}
	if (name_len > 0)
	{
		px_debug("parse_literal_data: unexpected eof");
		return PXE_PGP_CORRUPT_DATA;
	}

	/* skip date */
	res = pullf_read_max(pkt, 4, &buf, tmpbuf);
	if (res != 4)
	{
		px_debug("parse_literal_data: unexpected eof");
		return PXE_PGP_CORRUPT_DATA;
	}
	px_memset(tmpbuf, 0, 4);

	/*
	 * If called from an SQL function that returns text, pgp_decrypt() rejects
	 * inputs not self-identifying as text.
	 */
	if (ctx->text_mode)
		if (type != 't' && type != 'u')
		{
			px_debug("parse_literal_data: data type=%c", type);
			ctx->unexpected_binary = true;
		}

	ctx->unicode_mode = (type == 'u') ? 1 : 0;

	/* read data */
	while (1)
	{
		res = pullf_read(pkt, 32 * 1024, &buf);
		if (res <= 0)
			break;

		if (ctx->text_mode && ctx->convert_crlf)
			res = copy_crlf(dst, buf, res, &got_cr);
		else
			res = mbuf_append(dst, buf, res);
		if (res < 0)
			break;
	}
	if (res >= 0 && got_cr)
		res = mbuf_append(dst, (const uint8 *) "\r", 1);
	return res;
}

/* process_data_packets and parse_compressed_data call each other */
static int	process_data_packets(PGP_Context *ctx, MBuf *dst,
								 PullFilter *src, int allow_compr, int need_mdc);

static int
parse_compressed_data(PGP_Context *ctx, MBuf *dst, PullFilter *pkt)
{
	int			res;
	uint8		type;
	PullFilter *pf_decompr;
	uint8	   *discard_buf;

	GETBYTE(pkt, type);

	ctx->compress_algo = type;
	switch (type)
	{
		case PGP_COMPR_NONE:
			res = process_data_packets(ctx, dst, pkt, NO_COMPR, NO_MDC);
			break;

		case PGP_COMPR_ZIP:
		case PGP_COMPR_ZLIB:
			res = pgp_decompress_filter(&pf_decompr, ctx, pkt);
			if (res >= 0)
			{
				res = process_data_packets(ctx, dst, pf_decompr,
										   NO_COMPR, NO_MDC);
				pullf_free(pf_decompr);
			}
			break;

		case PGP_COMPR_BZIP2:
			px_debug("parse_compressed_data: bzip2 unsupported");
			/* report error in pgp_decrypt() */
			ctx->unsupported_compr = 1;

			/*
			 * Discard the compressed data, allowing it to first affect any
			 * MDC digest computation.
			 */
			while (1)
			{
				res = pullf_read(pkt, 32 * 1024, &discard_buf);
				if (res <= 0)
					break;
			}

			break;

		default:
			px_debug("parse_compressed_data: unknown compr type");
			res = PXE_PGP_CORRUPT_DATA;
	}

	return res;
}

static int
process_data_packets(PGP_Context *ctx, MBuf *dst, PullFilter *src,
					 int allow_compr, int need_mdc)
{
	uint8		tag;
	int			len,
				res;
	int			got_data = 0;
	int			got_mdc = 0;
	PullFilter *pkt = NULL;

	while (1)
	{
		res = pgp_parse_pkt_hdr(src, &tag, &len, ALLOW_CTX_SIZE);
		if (res <= 0)
			break;


		/* mdc packet should be last */
		if (got_mdc)
		{
			px_debug("process_data_packets: data after mdc");
			res = PXE_PGP_CORRUPT_DATA;
			break;
		}

		/*
		 * Context length inside SYMENCRYPTED_DATA_MDC packet needs special
		 * handling.
		 */
		if (need_mdc && res == PKT_CONTEXT)
			res = pullf_create(&pkt, &mdcbuf_filter, ctx, src);
		else
			res = pgp_create_pkt_reader(&pkt, src, len, res, ctx);
		if (res < 0)
			break;

		switch (tag)
		{
			case PGP_PKT_LITERAL_DATA:
				got_data = 1;
				res = parse_literal_data(ctx, dst, pkt);
				break;
			case PGP_PKT_COMPRESSED_DATA:
				if (allow_compr == 0)
				{
					px_debug("process_data_packets: unexpected compression");
					res = PXE_PGP_CORRUPT_DATA;
				}
				else if (got_data)
				{
					/*
					 * compr data must be alone
					 */
					px_debug("process_data_packets: only one cmpr pkt allowed");
					res = PXE_PGP_CORRUPT_DATA;
				}
				else
				{
					got_data = 1;
					res = parse_compressed_data(ctx, dst, pkt);
				}
				break;
			case PGP_PKT_MDC:
				if (need_mdc == NO_MDC)
				{
					px_debug("process_data_packets: unexpected MDC");
					res = PXE_PGP_CORRUPT_DATA;
					break;
				}

				res = mdc_finish(ctx, pkt, len);
				if (res >= 0)
					got_mdc = 1;
				break;
			default:
				px_debug("process_data_packets: unexpected pkt tag=%d", tag);
				res = PXE_PGP_CORRUPT_DATA;
		}

		pullf_free(pkt);
		pkt = NULL;

		if (res < 0)
			break;
	}

	if (pkt)
		pullf_free(pkt);

	if (res < 0)
		return res;

	if (!got_data)
	{
		px_debug("process_data_packets: no data");
		res = PXE_PGP_CORRUPT_DATA;
	}
	if (need_mdc && !got_mdc && !ctx->use_mdcbuf_filter)
	{
		px_debug("process_data_packets: got no mdc");
		res = PXE_PGP_CORRUPT_DATA;
	}
	return res;
}

static int
parse_symenc_data(PGP_Context *ctx, PullFilter *pkt, MBuf *dst)
{
	int			res;
	PGP_CFB    *cfb = NULL;
	PullFilter *pf_decrypt = NULL;
	PullFilter *pf_prefix = NULL;

	res = pgp_cfb_create(&cfb, ctx->cipher_algo,
						 ctx->sess_key, ctx->sess_key_len, 1, NULL);
	if (res < 0)
		goto out;

	res = pullf_create(&pf_decrypt, &pgp_decrypt_filter, cfb, pkt);
	if (res < 0)
		goto out;

	res = pullf_create(&pf_prefix, &prefix_filter, ctx, pf_decrypt);
	if (res < 0)
		goto out;

	res = process_data_packets(ctx, dst, pf_prefix, ALLOW_COMPR, NO_MDC);

out:
	if (pf_prefix)
		pullf_free(pf_prefix);
	if (pf_decrypt)
		pullf_free(pf_decrypt);
	if (cfb)
		pgp_cfb_free(cfb);

	return res;
}

static int
parse_symenc_mdc_data(PGP_Context *ctx, PullFilter *pkt, MBuf *dst)
{
	int			res;
	PGP_CFB    *cfb = NULL;
	PullFilter *pf_decrypt = NULL;
	PullFilter *pf_prefix = NULL;
	PullFilter *pf_mdc = NULL;
	uint8		ver;

	GETBYTE(pkt, ver);
	if (ver != 1)
	{
		px_debug("parse_symenc_mdc_data: pkt ver != 1");
		return PXE_PGP_CORRUPT_DATA;
	}

	res = pgp_cfb_create(&cfb, ctx->cipher_algo,
						 ctx->sess_key, ctx->sess_key_len, 0, NULL);
	if (res < 0)
		goto out;

	res = pullf_create(&pf_decrypt, &pgp_decrypt_filter, cfb, pkt);
	if (res < 0)
		goto out;

	res = pullf_create(&pf_mdc, &mdc_filter, ctx, pf_decrypt);
	if (res < 0)
		goto out;

	res = pullf_create(&pf_prefix, &prefix_filter, ctx, pf_mdc);
	if (res < 0)
		goto out;

	res = process_data_packets(ctx, dst, pf_prefix, ALLOW_COMPR, NEED_MDC);

out:
	if (pf_prefix)
		pullf_free(pf_prefix);
	if (pf_mdc)
		pullf_free(pf_mdc);
	if (pf_decrypt)
		pullf_free(pf_decrypt);
	if (cfb)
		pgp_cfb_free(cfb);

	return res;
}

/*
 * skip over packet contents
 */
int
pgp_skip_packet(PullFilter *pkt)
{
	int			res = 1;
	uint8	   *tmp;

	while (res > 0)
		res = pullf_read(pkt, 32 * 1024, &tmp);
	return res;
}

/*
 * expect to be at packet end, any data is error
 */
int
pgp_expect_packet_end(PullFilter *pkt)
{
	int			res;
	uint8	   *tmp;

	res = pullf_read(pkt, 32 * 1024, &tmp);
	if (res > 0)
	{
		px_debug("pgp_expect_packet_end: got data");
		return PXE_PGP_CORRUPT_DATA;
	}
	return res;
}

int
pgp_decrypt(PGP_Context *ctx, MBuf *msrc, MBuf *mdst)
{
	int			res;
	PullFilter *src = NULL;
	PullFilter *pkt = NULL;
	uint8		tag;
	int			len;
	int			got_key = 0;
	int			got_data = 0;

	res = pullf_create_mbuf_reader(&src, msrc);

	while (res >= 0)
	{
		res = pgp_parse_pkt_hdr(src, &tag, &len, NO_CTX_SIZE);
		if (res <= 0)
			break;

		res = pgp_create_pkt_reader(&pkt, src, len, res, ctx);
		if (res < 0)
			break;

		res = PXE_PGP_CORRUPT_DATA;
		switch (tag)
		{
			case PGP_PKT_MARKER:
				res = pgp_skip_packet(pkt);
				break;
			case PGP_PKT_PUBENCRYPTED_SESSKEY:
				/* fixme: skip those */
				res = pgp_parse_pubenc_sesskey(ctx, pkt);
				got_key = 1;
				break;
			case PGP_PKT_SYMENCRYPTED_SESSKEY:
				if (got_key)

					/*
					 * Theoretically, there could be several keys, both public
					 * and symmetric, all of which encrypt same session key.
					 * Decrypt should try with each one, before failing.
					 */
					px_debug("pgp_decrypt: using first of several keys");
				else
				{
					got_key = 1;
					res = parse_symenc_sesskey(ctx, pkt);
				}
				break;
			case PGP_PKT_SYMENCRYPTED_DATA:
				if (!got_key)
					px_debug("pgp_decrypt: have data but no key");
				else if (got_data)
					px_debug("pgp_decrypt: got second data packet");
				else
				{
					got_data = 1;
					ctx->disable_mdc = 1;
					res = parse_symenc_data(ctx, pkt, mdst);
				}
				break;
			case PGP_PKT_SYMENCRYPTED_DATA_MDC:
				if (!got_key)
					px_debug("pgp_decrypt: have data but no key");
				else if (got_data)
					px_debug("pgp_decrypt: several data pkts not supported");
				else
				{
					got_data = 1;
					ctx->disable_mdc = 0;
					res = parse_symenc_mdc_data(ctx, pkt, mdst);
				}
				break;
			default:
				px_debug("pgp_decrypt: unknown tag: 0x%02x", tag);
		}
		pullf_free(pkt);
		pkt = NULL;
	}

	if (pkt)
		pullf_free(pkt);

	if (src)
		pullf_free(src);

	if (res < 0)
		return res;

	/*
	 * Report a failure of the prefix_init() "quick check" now, rather than
	 * upon detection, to hinder timing attacks.  pgcrypto is not generally
	 * secure against timing attacks, but this helps.
	 */
	if (!got_data || ctx->corrupt_prefix)
		return PXE_PGP_CORRUPT_DATA;

	/*
	 * Code interpreting purportedly-decrypted data prior to this stage shall
	 * report no error other than PXE_PGP_CORRUPT_DATA.  (PXE_BUG is okay so
	 * long as it remains unreachable.)  This ensures that an attacker able to
	 * choose a ciphertext and receive a corresponding decryption error
	 * message cannot use that oracle to gather clues about the decryption
	 * key.  See "An Attack on CFB Mode Encryption As Used By OpenPGP" by
	 * Serge Mister and Robert Zuccherato.
	 *
	 * A problematic value in the first octet of a Literal Data or Compressed
	 * Data packet may indicate a simple user error, such as the need to call
	 * pgp_sym_decrypt_bytea instead of pgp_sym_decrypt.  Occasionally,
	 * though, it is the first symptom of the encryption key not matching the
	 * decryption key.  When this was the only problem encountered, report a
	 * specific error to guide the user; otherwise, we will have reported
	 * PXE_PGP_CORRUPT_DATA before now.  A key mismatch makes the other errors
	 * into red herrings, and this avoids leaking clues to attackers.
	 */
	if (ctx->unsupported_compr)
		return PXE_PGP_UNSUPPORTED_COMPR;
	if (ctx->unexpected_binary)
		return PXE_PGP_NOT_TEXT;

	return res;
}
