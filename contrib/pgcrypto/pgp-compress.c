/*
 * pgp-compress.c
 *	  ZIP and ZLIB compression via zlib.
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
 * contrib/pgcrypto/pgp-compress.c
 */

#include "postgres.h"

#include "pgp.h"
#include "px.h"

/*
 * Compressed pkt writer
 */

#ifdef HAVE_LIBZ

#include <zlib.h>

#define ZIP_OUT_BUF 8192
#define ZIP_IN_BLOCK 8192

struct ZipStat
{
	uint8		type;
	int			buf_len;
	int			hdr_done;
	z_stream	stream;
	uint8		buf[ZIP_OUT_BUF];
};

static void *
z_alloc(void *priv, unsigned n_items, unsigned item_len)
{
	return px_alloc(n_items * item_len);
}

static void
z_free(void *priv, void *addr)
{
	px_free(addr);
}

static int
compress_init(PushFilter *next, void *init_arg, void **priv_p)
{
	int			res;
	struct ZipStat *st;
	PGP_Context *ctx = init_arg;
	uint8		type = ctx->compress_algo;

	if (type != PGP_COMPR_ZLIB && type != PGP_COMPR_ZIP)
		return PXE_PGP_UNSUPPORTED_COMPR;

	/*
	 * init
	 */
	st = px_alloc(sizeof(*st));
	memset(st, 0, sizeof(*st));
	st->buf_len = ZIP_OUT_BUF;
	st->stream.zalloc = z_alloc;
	st->stream.zfree = z_free;

	if (type == PGP_COMPR_ZIP)
		res = deflateInit2(&st->stream, ctx->compress_level,
						   Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
	else
		res = deflateInit(&st->stream, ctx->compress_level);
	if (res != Z_OK)
	{
		px_free(st);
		return PXE_PGP_COMPRESSION_ERROR;
	}
	*priv_p = st;

	return ZIP_IN_BLOCK;
}

/* writes compressed data packet */

/* can handle zero-len incoming data, but shouldn't */
static int
compress_process(PushFilter *next, void *priv, const uint8 *data, int len)
{
	int			res,
				n_out;
	struct ZipStat *st = priv;

	/*
	 * process data
	 */
	st->stream.next_in = unconstify(uint8 *, data);
	st->stream.avail_in = len;
	while (st->stream.avail_in > 0)
	{
		st->stream.next_out = st->buf;
		st->stream.avail_out = st->buf_len;
		res = deflate(&st->stream, Z_NO_FLUSH);
		if (res != Z_OK)
			return PXE_PGP_COMPRESSION_ERROR;

		n_out = st->buf_len - st->stream.avail_out;
		if (n_out > 0)
		{
			res = pushf_write(next, st->buf, n_out);
			if (res < 0)
				return res;
		}
	}

	return 0;
}

static int
compress_flush(PushFilter *next, void *priv)
{
	int			res,
				zres,
				n_out;
	struct ZipStat *st = priv;

	st->stream.next_in = NULL;
	st->stream.avail_in = 0;
	while (1)
	{
		st->stream.next_out = st->buf;
		st->stream.avail_out = st->buf_len;
		zres = deflate(&st->stream, Z_FINISH);
		if (zres != Z_STREAM_END && zres != Z_OK)
			return PXE_PGP_COMPRESSION_ERROR;

		n_out = st->buf_len - st->stream.avail_out;
		if (n_out > 0)
		{
			res = pushf_write(next, st->buf, n_out);
			if (res < 0)
				return res;
		}
		if (zres == Z_STREAM_END)
			break;
	}
	return 0;
}

static void
compress_free(void *priv)
{
	struct ZipStat *st = priv;

	deflateEnd(&st->stream);
	px_memset(st, 0, sizeof(*st));
	px_free(st);
}

static const PushFilterOps
			compress_filter = {
	compress_init, compress_process, compress_flush, compress_free
};

int
pgp_compress_filter(PushFilter **res, PGP_Context *ctx, PushFilter *dst)
{
	return pushf_create(res, &compress_filter, ctx, dst);
}

/*
 * Decompress
 */
struct DecomprData
{
	int			buf_len;		/* = ZIP_OUT_BUF */
	int			buf_data;		/* available data */
	uint8	   *pos;
	z_stream	stream;
	int			eof;
	uint8		buf[ZIP_OUT_BUF];
};

static int
decompress_init(void **priv_p, void *arg, PullFilter *src)
{
	PGP_Context *ctx = arg;
	struct DecomprData *dec;
	int			res;

	if (ctx->compress_algo != PGP_COMPR_ZLIB
		&& ctx->compress_algo != PGP_COMPR_ZIP)
		return PXE_PGP_UNSUPPORTED_COMPR;

	dec = px_alloc(sizeof(*dec));
	memset(dec, 0, sizeof(*dec));
	dec->buf_len = ZIP_OUT_BUF;
	*priv_p = dec;

	dec->stream.zalloc = z_alloc;
	dec->stream.zfree = z_free;

	if (ctx->compress_algo == PGP_COMPR_ZIP)
		res = inflateInit2(&dec->stream, -15);
	else
		res = inflateInit(&dec->stream);
	if (res != Z_OK)
	{
		px_free(dec);
		px_debug("decompress_init: inflateInit error");
		return PXE_PGP_COMPRESSION_ERROR;
	}

	return 0;
}

static int
decompress_read(void *priv, PullFilter *src, int len,
				uint8 **data_p, uint8 *buf, int buflen)
{
	int			res;
	int			flush;
	struct DecomprData *dec = priv;

restart:
	if (dec->buf_data > 0)
	{
		if (len > dec->buf_data)
			len = dec->buf_data;
		*data_p = dec->pos;
		dec->pos += len;
		dec->buf_data -= len;
		return len;
	}

	if (dec->eof)
		return 0;

	if (dec->stream.avail_in == 0)
	{
		uint8	   *tmp;

		res = pullf_read(src, 8192, &tmp);
		if (res < 0)
			return res;
		dec->stream.next_in = tmp;
		dec->stream.avail_in = res;
	}

	dec->stream.next_out = dec->buf;
	dec->stream.avail_out = dec->buf_len;
	dec->pos = dec->buf;

	/*
	 * Z_SYNC_FLUSH is tell zlib to output as much as possible. It should do
	 * it anyway (Z_NO_FLUSH), but seems to reserve the right not to.  So lets
	 * follow the API.
	 */
	flush = dec->stream.avail_in ? Z_SYNC_FLUSH : Z_FINISH;
	res = inflate(&dec->stream, flush);
	if (res != Z_OK && res != Z_STREAM_END)
	{
		px_debug("decompress_read: inflate error: %d", res);
		return PXE_PGP_CORRUPT_DATA;
	}

	dec->buf_data = dec->buf_len - dec->stream.avail_out;
	if (res == Z_STREAM_END)
	{
		uint8	   *tmp;

		/*
		 * A stream must be terminated by a normal packet.  If the last stream
		 * packet in the source stream is a full packet, a normal empty packet
		 * must follow.  Since the underlying packet reader doesn't know that
		 * the compressed stream has been ended, we need to to consume the
		 * terminating packet here.  This read does not harm even if the
		 * stream has already ended.
		 */
		res = pullf_read(src, 1, &tmp);

		if (res < 0)
			return res;
		else if (res > 0)
		{
			px_debug("decompress_read: extra bytes after end of stream");
			return PXE_PGP_CORRUPT_DATA;
		}
		dec->eof = 1;
	}
	goto restart;
}

static void
decompress_free(void *priv)
{
	struct DecomprData *dec = priv;

	inflateEnd(&dec->stream);
	px_memset(dec, 0, sizeof(*dec));
	px_free(dec);
}

static const PullFilterOps
			decompress_filter = {
	decompress_init, decompress_read, decompress_free
};

int
pgp_decompress_filter(PullFilter **res, PGP_Context *ctx, PullFilter *src)
{
	return pullf_create(res, &decompress_filter, ctx, src);
}
#else							/* !HAVE_LIBZ */

int
pgp_compress_filter(PushFilter **res, PGP_Context *ctx, PushFilter *dst)
{
	return PXE_PGP_UNSUPPORTED_COMPR;
}

int
pgp_decompress_filter(PullFilter **res, PGP_Context *ctx, PullFilter *src)
{
	return PXE_PGP_UNSUPPORTED_COMPR;
}

#endif
