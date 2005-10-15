/*
 * mbuf.c
 *		Memory buffer operations.
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
 * $PostgreSQL: pgsql/contrib/pgcrypto/mbuf.c,v 1.3 2005/10/15 02:49:06 momjian Exp $
 */

#include "postgres.h"

#include "px.h"
#include "mbuf.h"

#define STEP  (16*1024)

struct MBuf
{
	uint8	   *data;
	uint8	   *data_end;
	uint8	   *read_pos;
	uint8	   *buf_end;
	int			no_write:1;
	int			own_data:1;
};

int
mbuf_avail(MBuf * mbuf)
{
	return mbuf->data_end - mbuf->read_pos;
}

int
mbuf_size(MBuf * mbuf)
{
	return mbuf->data_end - mbuf->data;
}

int
mbuf_tell(MBuf * mbuf)
{
	return mbuf->read_pos - mbuf->data;
}

int
mbuf_free(MBuf * mbuf)
{
	if (mbuf->own_data)
	{
		memset(mbuf->data, 0, mbuf->buf_end - mbuf->data);
		px_free(mbuf->data);
	}
	px_free(mbuf);
	return 0;
}

static void
prepare_room(MBuf * mbuf, int block_len)
{
	uint8	   *newbuf;
	unsigned	newlen;

	if (mbuf->data_end + block_len <= mbuf->buf_end)
		return;

	newlen = (mbuf->buf_end - mbuf->data)
		+ ((block_len + STEP + STEP - 1) & -STEP);

	newbuf = px_realloc(mbuf->data, newlen);

	mbuf->buf_end = newbuf + newlen;
	mbuf->data_end = newbuf + (mbuf->data_end - mbuf->data);
	mbuf->read_pos = newbuf + (mbuf->read_pos - mbuf->data);
	mbuf->data = newbuf;

	return;
}

int
mbuf_append(MBuf * dst, const uint8 *buf, int len)
{
	if (dst->no_write)
	{
		px_debug("mbuf_append: no_write");
		return PXE_BUG;
	}

	prepare_room(dst, len);

	memcpy(dst->data_end, buf, len);
	dst->data_end += len;

	return 0;
}

MBuf *
mbuf_create(int len)
{
	MBuf	   *mbuf;

	if (!len)
		len = 8192;

	mbuf = px_alloc(sizeof *mbuf);
	mbuf->data = px_alloc(len);
	mbuf->buf_end = mbuf->data + len;
	mbuf->data_end = mbuf->data;
	mbuf->read_pos = mbuf->data;

	mbuf->no_write = 0;
	mbuf->own_data = 1;

	return mbuf;
}

MBuf *
mbuf_create_from_data(const uint8 *data, int len)
{
	MBuf	   *mbuf;

	mbuf = px_alloc(sizeof *mbuf);
	mbuf->data = (uint8 *) data;
	mbuf->buf_end = mbuf->data + len;
	mbuf->data_end = mbuf->data + len;
	mbuf->read_pos = mbuf->data;

	mbuf->no_write = 1;
	mbuf->own_data = 0;

	return mbuf;
}


int
mbuf_grab(MBuf * mbuf, int len, uint8 **data_p)
{
	if (len > mbuf_avail(mbuf))
		len = mbuf_avail(mbuf);

	mbuf->no_write = 1;

	*data_p = mbuf->read_pos;
	mbuf->read_pos += len;
	return len;
}

int
mbuf_rewind(MBuf * mbuf)
{
	mbuf->read_pos = mbuf->data;
	return 0;
}

int
mbuf_steal_data(MBuf * mbuf, uint8 **data_p)
{
	int			len = mbuf_size(mbuf);

	mbuf->no_write = 1;
	mbuf->own_data = 0;

	*data_p = mbuf->data;

	mbuf->data = mbuf->data_end = mbuf->read_pos = mbuf->buf_end = NULL;

	return len;
}

/*
 * PullFilter
 */

struct PullFilter
{
	PullFilter *src;
	const PullFilterOps *op;
	int			buflen;
	uint8	   *buf;
	int			pos;
	void	   *priv;
};

int
pullf_create(PullFilter ** pf_p, const PullFilterOps * op, void *init_arg, PullFilter * src)
{
	PullFilter *pf;
	void	   *priv;
	int			res;

	if (op->init != NULL)
	{
		res = op->init(&priv, init_arg, src);
		if (res < 0)
			return res;
	}
	else
	{
		priv = init_arg;
		res = 0;
	}

	pf = px_alloc(sizeof(*pf));
	memset(pf, 0, sizeof(*pf));
	pf->buflen = res;
	pf->op = op;
	pf->priv = priv;
	pf->src = src;
	if (pf->buflen > 0)
	{
		pf->buf = px_alloc(pf->buflen);
		pf->pos = 0;
	}
	else
	{
		pf->buf = NULL;
		pf->pos = 0;
	}
	*pf_p = pf;
	return 0;
}

void
pullf_free(PullFilter * pf)
{
	if (pf->op->free)
		pf->op->free(pf->priv);

	if (pf->buf)
	{
		memset(pf->buf, 0, pf->buflen);
		px_free(pf->buf);
	}

	memset(pf, 0, sizeof(*pf));
	px_free(pf);
}

/* may return less data than asked, 0 means eof */
int
pullf_read(PullFilter * pf, int len, uint8 **data_p)
{
	int			res;

	if (pf->op->pull)
	{
		if (pf->buflen && len > pf->buflen)
			len = pf->buflen;
		res = pf->op->pull(pf->priv, pf->src, len, data_p,
						   pf->buf, pf->buflen);
	}
	else
		res = pullf_read(pf->src, len, data_p);
	return res;
}

int
pullf_read_max(PullFilter * pf, int len, uint8 **data_p, uint8 *tmpbuf)
{
	int			res,
				total;
	uint8	   *tmp;

	res = pullf_read(pf, len, data_p);
	if (res <= 0 || res == len)
		return res;

	/* read was shorter, use tmpbuf */
	memcpy(tmpbuf, *data_p, res);
	*data_p = tmpbuf;
	len -= res;
	total = res;

	while (len > 0)
	{
		res = pullf_read(pf, len, &tmp);
		if (res < 0)
		{
			/* so the caller must clear only on success */
			memset(tmpbuf, 0, total);
			return res;
		}
		if (res == 0)
			break;
		memcpy(tmpbuf + total, tmp, res);
		total += res;
	}
	return total;
}

/*
 * caller wants exatly len bytes and dont bother with references
 */
int
pullf_read_fixed(PullFilter * src, int len, uint8 *dst)
{
	int			res;
	uint8	   *p;

	res = pullf_read_max(src, len, &p, dst);
	if (res < 0)
		return res;
	if (res != len)
	{
		px_debug("pullf_read_fixed: need=%d got=%d", len, res);
		return PXE_MBUF_SHORT_READ;
	}
	if (p != dst)
		memcpy(dst, p, len);
	return 0;
}

/*
 * read from MBuf
 */
static int
pull_from_mbuf(void *arg, PullFilter * src, int len,
			   uint8 **data_p, uint8 *buf, int buflen)
{
	MBuf	   *mbuf = arg;

	return mbuf_grab(mbuf, len, data_p);
}

static const struct PullFilterOps mbuf_reader = {
	NULL, pull_from_mbuf, NULL
};

int
pullf_create_mbuf_reader(PullFilter ** mp_p, MBuf * src)
{
	return pullf_create(mp_p, &mbuf_reader, src, NULL);
}


/*
 * PushFilter
 */

struct PushFilter
{
	PushFilter *next;
	const PushFilterOps *op;
	int			block_size;
	uint8	   *buf;
	int			pos;
	void	   *priv;
};

int
pushf_create(PushFilter ** mp_p, const PushFilterOps * op, void *init_arg, PushFilter * next)
{
	PushFilter *mp;
	void	   *priv;
	int			res;

	if (op->init != NULL)
	{
		res = op->init(next, init_arg, &priv);
		if (res < 0)
			return res;
	}
	else
	{
		priv = init_arg;
		res = 0;
	}

	mp = px_alloc(sizeof(*mp));
	memset(mp, 0, sizeof(*mp));
	mp->block_size = res;
	mp->op = op;
	mp->priv = priv;
	mp->next = next;
	if (mp->block_size > 0)
	{
		mp->buf = px_alloc(mp->block_size);
		mp->pos = 0;
	}
	else
	{
		mp->buf = NULL;
		mp->pos = 0;
	}
	*mp_p = mp;
	return 0;
}

void
pushf_free(PushFilter * mp)
{
	if (mp->op->free)
		mp->op->free(mp->priv);

	if (mp->buf)
	{
		memset(mp->buf, 0, mp->block_size);
		px_free(mp->buf);
	}

	memset(mp, 0, sizeof(*mp));
	px_free(mp);
}

void
pushf_free_all(PushFilter * mp)
{
	PushFilter *tmp;

	while (mp)
	{
		tmp = mp->next;
		pushf_free(mp);
		mp = tmp;
	}
}

static int
wrap_process(PushFilter * mp, const uint8 *data, int len)
{
	int			res;

	if (mp->op->push != NULL)
		res = mp->op->push(mp->next, mp->priv, data, len);
	else
		res = pushf_write(mp->next, data, len);
	if (res > 0)
		return PXE_BUG;
	return res;
}

/* consumes all data, returns len on success */
int
pushf_write(PushFilter * mp, const uint8 *data, int len)
{
	int			need,
				res;

	/*
	 * no buffering
	 */
	if (mp->block_size <= 0)
		return wrap_process(mp, data, len);

	/*
	 * try to empty buffer
	 */
	need = mp->block_size - mp->pos;
	if (need > 0)
	{
		if (len < need)
		{
			memcpy(mp->buf + mp->pos, data, len);
			mp->pos += len;
			return 0;
		}
		memcpy(mp->buf + mp->pos, data, need);
		len -= need;
		data += need;
	}

	/*
	 * buffer full, process
	 */
	res = wrap_process(mp, mp->buf, mp->block_size);
	if (res < 0)
		return res;
	mp->pos = 0;

	/*
	 * now process directly from data
	 */
	while (len > 0)
	{
		if (len > mp->block_size)
		{
			res = wrap_process(mp, data, mp->block_size);
			if (res < 0)
				return res;
			data += mp->block_size;
			len -= mp->block_size;
		}
		else
		{
			memcpy(mp->buf, data, len);
			mp->pos += len;
			break;
		}
	}
	return 0;
}

int
pushf_flush(PushFilter * mp)
{
	int			res;

	while (mp)
	{
		if (mp->block_size > 0)
		{
			res = wrap_process(mp, mp->buf, mp->pos);
			if (res < 0)
				return res;
		}

		if (mp->op->flush)
		{
			res = mp->op->flush(mp->next, mp->priv);
			if (res < 0)
				return res;
		}

		mp = mp->next;
	}
	return 0;
}


/*
 * write to MBuf
 */
static int
push_into_mbuf(PushFilter * next, void *arg, const uint8 *data, int len)
{
	int			res = 0;
	MBuf	   *mbuf = arg;

	if (len > 0)
		res = mbuf_append(mbuf, data, len);
	return res < 0 ? res : 0;
}

static const struct PushFilterOps mbuf_filter = {
	NULL, push_into_mbuf, NULL, NULL
};

int
pushf_create_mbuf_writer(PushFilter ** res, MBuf * dst)
{
	return pushf_create(res, &mbuf_filter, dst, NULL);
}
