/*
 * pgp-pubenc.c
 *	  Encrypt session key with public key.
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
 * contrib/pgcrypto/pgp-pubenc.c
 */
#include "postgres.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

/*
 * padded msg: 02 || non-zero pad bytes || 00 || msg
 */
static int
pad_eme_pkcs1_v15(uint8 *data, int data_len, int res_len, uint8 **res_p)
{
	int			res;
	uint8	   *buf,
			   *p;
	int			pad_len = res_len - 2 - data_len;

	if (pad_len < 8)
		return PXE_BUG;

	buf = px_alloc(res_len);
	buf[0] = 0x02;
	res = px_get_random_bytes(buf + 1, pad_len);
	if (res < 0)
	{
		px_free(buf);
		return res;
	}

	/* pad must not contain zero bytes */
	p = buf + 1;
	while (p < buf + 1 + pad_len)
	{
		if (*p == 0)
		{
			res = px_get_random_bytes(p, 1);
			if (res < 0)
				break;
		}
		if (*p != 0)
			p++;
	}

	if (res < 0)
	{
		memset(buf, 0, res_len);
		px_free(buf);
		return res;
	}

	buf[pad_len + 1] = 0;
	memcpy(buf + pad_len + 2, data, data_len);
	*res_p = buf;

	return 0;
}

static int
create_secmsg(PGP_Context *ctx, PGP_MPI **msg_p, int full_bytes)
{
	uint8	   *secmsg;
	int			res,
				i;
	unsigned	cksum = 0;
	int			klen = ctx->sess_key_len;
	uint8	   *padded = NULL;
	PGP_MPI    *m = NULL;

	/* calc checksum */
	for (i = 0; i < klen; i++)
		cksum += ctx->sess_key[i];

	/*
	 * create "secret message"
	 */
	secmsg = px_alloc(klen + 3);
	secmsg[0] = ctx->cipher_algo;
	memcpy(secmsg + 1, ctx->sess_key, klen);
	secmsg[klen + 1] = (cksum >> 8) & 0xFF;
	secmsg[klen + 2] = cksum & 0xFF;

	/*
	 * now create a large integer of it
	 */
	res = pad_eme_pkcs1_v15(secmsg, klen + 3, full_bytes, &padded);
	if (res >= 0)
	{
		/* first byte will be 0x02 */
		int			full_bits = full_bytes * 8 - 6;

		res = pgp_mpi_create(padded, full_bits, &m);
	}

	if (padded)
	{
		memset(padded, 0, full_bytes);
		px_free(padded);
	}
	memset(secmsg, 0, klen + 3);
	px_free(secmsg);

	if (res >= 0)
		*msg_p = m;

	return res;
}

static int
encrypt_and_write_elgamal(PGP_Context *ctx, PGP_PubKey *pk, PushFilter *pkt)
{
	int			res;
	PGP_MPI    *m = NULL,
			   *c1 = NULL,
			   *c2 = NULL;

	/* create padded msg */
	res = create_secmsg(ctx, &m, pk->pub.elg.p->bytes - 1);
	if (res < 0)
		goto err;

	/* encrypt it */
	res = pgp_elgamal_encrypt(pk, m, &c1, &c2);
	if (res < 0)
		goto err;

	/* write out */
	res = pgp_mpi_write(pkt, c1);
	if (res < 0)
		goto err;
	res = pgp_mpi_write(pkt, c2);

err:
	pgp_mpi_free(m);
	pgp_mpi_free(c1);
	pgp_mpi_free(c2);
	return res;
}

static int
encrypt_and_write_rsa(PGP_Context *ctx, PGP_PubKey *pk, PushFilter *pkt)
{
	int			res;
	PGP_MPI    *m = NULL,
			   *c = NULL;

	/* create padded msg */
	res = create_secmsg(ctx, &m, pk->pub.rsa.n->bytes - 1);
	if (res < 0)
		goto err;

	/* encrypt it */
	res = pgp_rsa_encrypt(pk, m, &c);
	if (res < 0)
		goto err;

	/* write out */
	res = pgp_mpi_write(pkt, c);

err:
	pgp_mpi_free(m);
	pgp_mpi_free(c);
	return res;
}

int
pgp_write_pubenc_sesskey(PGP_Context *ctx, PushFilter *dst)
{
	int			res;
	PGP_PubKey *pk = ctx->pub_key;
	uint8		ver = 3;
	PushFilter *pkt = NULL;
	uint8		algo;

	if (pk == NULL)
	{
		px_debug("no pubkey?\n");
		return PXE_BUG;
	}

	algo = pk->algo;

	/*
	 * now write packet
	 */
	res = pgp_create_pkt_writer(dst, PGP_PKT_PUBENCRYPTED_SESSKEY, &pkt);
	if (res < 0)
		goto err;
	res = pushf_write(pkt, &ver, 1);
	if (res < 0)
		goto err;
	res = pushf_write(pkt, pk->key_id, 8);
	if (res < 0)
		goto err;
	res = pushf_write(pkt, &algo, 1);
	if (res < 0)
		goto err;

	switch (algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
			res = encrypt_and_write_elgamal(ctx, pk, pkt);
			break;
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			res = encrypt_and_write_rsa(ctx, pk, pkt);
			break;
	}
	if (res < 0)
		goto err;

	/*
	 * done, signal packet end
	 */
	res = pushf_flush(pkt);
err:
	if (pkt)
		pushf_free(pkt);

	return res;
}
