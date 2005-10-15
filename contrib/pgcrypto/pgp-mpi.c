/*
 * pgp-mpi.c
 *	  OpenPGP MPI helper functions.
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
 * $PostgreSQL: pgsql/contrib/pgcrypto/pgp-mpi.c,v 1.4 2005/10/15 02:49:06 momjian Exp $
 */
#include "postgres.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

int
pgp_mpi_alloc(int bits, PGP_MPI ** mpi)
{
	PGP_MPI    *n;
	int			len = (bits + 7) / 8;

	if (bits < 0 || bits > 0xFFFF)
	{
		px_debug("pgp_mpi_alloc: unreasonable request: bits=%d", bits);
		return PXE_PGP_CORRUPT_DATA;
	}
	n = px_alloc(sizeof(*n) + len);
	n->bits = bits;
	n->bytes = len;
	n->data = (uint8 *) (n) + sizeof(*n);
	*mpi = n;
	return 0;
}

int
pgp_mpi_create(uint8 *data, int bits, PGP_MPI ** mpi)
{
	int			res;
	PGP_MPI    *n;

	res = pgp_mpi_alloc(bits, &n);
	if (res < 0)
		return res;
	memcpy(n->data, data, n->bytes);
	*mpi = n;
	return 0;
}

int
pgp_mpi_free(PGP_MPI * mpi)
{
	if (mpi == NULL)
		return 0;
	memset(mpi, 0, sizeof(*mpi) + mpi->bytes);
	px_free(mpi);
	return 0;
}

int
pgp_mpi_read(PullFilter * src, PGP_MPI ** mpi)
{
	int			res;
	uint8		hdr[2];
	int			bits;
	PGP_MPI    *n;

	res = pullf_read_fixed(src, 2, hdr);
	if (res < 0)
		return res;
	bits = ((unsigned) hdr[0] << 8) + hdr[1];

	res = pgp_mpi_alloc(bits, &n);
	if (res < 0)
		return res;

	res = pullf_read_fixed(src, n->bytes, n->data);
	if (res < 0)
		pgp_mpi_free(n);
	else
		*mpi = n;
	return res;
}

int
pgp_mpi_write(PushFilter * dst, PGP_MPI * n)
{
	int			res;
	uint8		buf[2];

	buf[0] = n->bits >> 8;
	buf[1] = n->bits & 0xFF;
	res = pushf_write(dst, buf, 2);
	if (res >= 0)
		res = pushf_write(dst, n->data, n->bytes);
	return res;
}

int
pgp_mpi_hash(PX_MD * md, PGP_MPI * n)
{
	uint8		buf[2];

	buf[0] = n->bits >> 8;
	buf[1] = n->bits & 0xFF;
	px_md_update(md, buf, 2);
	px_md_update(md, n->data, n->bytes);

	return 0;
}

unsigned
pgp_mpi_cksum(unsigned cksum, PGP_MPI * n)
{
	int			i;

	cksum += n->bits >> 8;
	cksum += n->bits & 0xFF;
	for (i = 0; i < n->bytes; i++)
		cksum += n->data[i];

	return cksum & 0xFFFF;
}
