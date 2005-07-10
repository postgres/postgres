/*
 * pgp-pubenc.c
 *    Encrypt session key with public key.
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
 * $PostgreSQL: pgsql/contrib/pgcrypto/pgp-pubenc.c,v 1.1 2005/07/10 13:46:29 momjian Exp $
 */
#include <postgres.h>

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

/*
 * padded msg: 02 || non-zero pad bytes || 00 || msg
 */
static int
pad_eme_pkcs1_v15(uint8 *data, int data_len, int res_len, uint8 **res_p)
{
	int res;
	uint8 *buf, *p;
	int pad_len = res_len - 2 - data_len;

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

/*
 * Decide the padded message length in bytes.
 * It should be as large as possible, but not larger
 * than p.
 *
 * To get max size (and assuming p may have weird sizes):
 * ((p->bytes * 8 - 6) > p->bits) ? (p->bytes - 1) : p->bytes
 *
 * Following mirrors gnupg behaviour.
 */
static int
decide_msglen(PGP_MPI *p)
{
	return p->bytes - 1;
}

static int
create_secmsg(PGP_Context *ctx, PGP_MPI **msg_p)
{
	uint8 *secmsg;
	int res, i, full_bytes;
	unsigned cksum = 0;
	int klen = ctx->sess_key_len;
	uint8 *padded = NULL;
	PGP_MPI *m = NULL;
	PGP_PubKey *pk = ctx->pub_key;

	/*
	 * Refuse to operate with keys < 1024
	 */
	if (pk->elg_p->bits < 1024)
		return PXE_PGP_SHORT_ELGAMAL_KEY;
	
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
	full_bytes = decide_msglen(pk->elg_p);
	res = pad_eme_pkcs1_v15(secmsg, klen + 3, full_bytes, &padded);
	if (res >= 0)
	{
		/* first byte will be 0x02 */
		int full_bits = full_bytes * 8 - 6;
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

int pgp_write_pubenc_sesskey(PGP_Context *ctx, PushFilter *dst)
{
	int res;
	PGP_PubKey *pk = ctx->pub_key;
	PGP_MPI *m = NULL, *c1 = NULL, *c2 = NULL;
	uint8 ver = 3;
	uint8 algo = PGP_PUB_ELG_ENCRYPT;
	PushFilter *pkt = NULL;

	if (pk == NULL) {
		px_debug("no pubkey?\n");
		return PXE_BUG;
	}
	if (!pk->elg_p || !pk->elg_g || !pk->elg_y) {
		px_debug("pubkey not loaded?\n");
		return PXE_BUG;
	}

	/*
	 * sesskey packet
	 */
	res = create_secmsg(ctx, &m);
	if (res < 0)
		goto err;

	/*
	 * encrypt it
	 */
	res = pgp_elgamal_encrypt(pk, m, &c1, &c2);
	if (res < 0)
		goto err;

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
	res = pgp_mpi_write(pkt, c1);
	if (res < 0)
		goto err;
	res = pgp_mpi_write(pkt, c2);
	if (res < 0)
		goto err;

	/*
	 * done, signal packet end
	 */
	res = pushf_flush(pkt);
err:
	if (pkt)
		pushf_free(pkt);
	if (m)
		pgp_mpi_free(m);
	if (c1)
		pgp_mpi_free(c1);
	if (c2)
		pgp_mpi_free(c2);

	return res;
}


