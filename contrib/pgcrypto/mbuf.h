/*
 * mbuf.h
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/mbuf.h
 */

#ifndef __PX_MBUF_H
#define __PX_MBUF_H

typedef struct MBuf MBuf;
typedef struct PushFilter PushFilter;
typedef struct PullFilter PullFilter;
typedef struct PushFilterOps PushFilterOps;
typedef struct PullFilterOps PullFilterOps;

struct PushFilterOps
{
	/*
	 * should return needed buffer size, 0- no buffering, <0 on error if NULL,
	 * no buffering, and priv=init_arg
	 */
	int			(*init) (PushFilter *next, void *init_arg, void **priv_p);

	/*
	 * send data to next.  should consume all? if null, it will be simply
	 * copied (in-place) returns 0 on error
	 */
	int			(*push) (PushFilter *next, void *priv,
						 const uint8 *src, int len);
	int			(*flush) (PushFilter *next, void *priv);
	void		(*free) (void *priv);
};

struct PullFilterOps
{
	/*
	 * should return needed buffer size, 0- no buffering, <0 on error if NULL,
	 * no buffering, and priv=init_arg
	 */
	int			(*init) (void **priv_p, void *init_arg, PullFilter *src);

	/*
	 * request data from src, put result ptr to data_p can use ptr from src or
	 * use buf as work area if NULL in-place copy
	 */
	int			(*pull) (void *priv, PullFilter *src, int len,
						 uint8 **data_p, uint8 *buf, int buflen);
	void		(*free) (void *priv);
};

/*
 * Memory buffer
 */
MBuf	   *mbuf_create(int len);
MBuf	   *mbuf_create_from_data(uint8 *data, int len);
int			mbuf_tell(MBuf *mbuf);
int			mbuf_avail(MBuf *mbuf);
int			mbuf_size(MBuf *mbuf);
int			mbuf_grab(MBuf *mbuf, int len, uint8 **data_p);
int			mbuf_steal_data(MBuf *mbuf, uint8 **data_p);
int			mbuf_append(MBuf *dst, const uint8 *buf, int cnt);
int			mbuf_rewind(MBuf *mbuf);
int			mbuf_free(MBuf *mbuf);

/*
 * Push filter
 */
int			pushf_create(PushFilter **res, const PushFilterOps *ops, void *init_arg,
						 PushFilter *next);
int			pushf_write(PushFilter *mp, const uint8 *data, int len);
void		pushf_free_all(PushFilter *mp);
void		pushf_free(PushFilter *mp);
int			pushf_flush(PushFilter *mp);

int			pushf_create_mbuf_writer(PushFilter **mp_p, MBuf *mbuf);

/*
 * Pull filter
 */
int			pullf_create(PullFilter **res, const PullFilterOps *ops,
						 void *init_arg, PullFilter *src);
int			pullf_read(PullFilter *mp, int len, uint8 **data_p);
int			pullf_read_max(PullFilter *mp, int len,
						   uint8 **data_p, uint8 *tmpbuf);
void		pullf_free(PullFilter *mp);
int			pullf_read_fixed(PullFilter *src, int len, uint8 *dst);

int			pullf_create_mbuf_reader(PullFilter **pf_p, MBuf *mbuf);

#define GETBYTE(pf, dst) \
	do { \
		uint8 __b; \
		int __res = pullf_read_fixed(pf, 1, &__b); \
		if (__res < 0) \
			return __res; \
		(dst) = __b; \
	} while (0)

#endif							/* __PX_MBUF_H */
