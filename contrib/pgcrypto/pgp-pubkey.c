/*
 * pgp-pubkey.c
 *	  Read public or secret key.
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
 * contrib/pgcrypto/pgp-pubkey.c
 */
#include "postgres.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

int
pgp_key_alloc(PGP_PubKey **pk_p)
{
	PGP_PubKey *pk;

	pk = px_alloc(sizeof(*pk));
	memset(pk, 0, sizeof(*pk));
	*pk_p = pk;
	return 0;
}

void
pgp_key_free(PGP_PubKey *pk)
{
	if (pk == NULL)
		return;

	switch (pk->algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
			pgp_mpi_free(pk->pub.elg.p);
			pgp_mpi_free(pk->pub.elg.g);
			pgp_mpi_free(pk->pub.elg.y);
			pgp_mpi_free(pk->sec.elg.x);
			break;
		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			pgp_mpi_free(pk->pub.rsa.n);
			pgp_mpi_free(pk->pub.rsa.e);
			pgp_mpi_free(pk->sec.rsa.d);
			pgp_mpi_free(pk->sec.rsa.p);
			pgp_mpi_free(pk->sec.rsa.q);
			pgp_mpi_free(pk->sec.rsa.u);
			break;
		case PGP_PUB_DSA_SIGN:
			pgp_mpi_free(pk->pub.dsa.p);
			pgp_mpi_free(pk->pub.dsa.q);
			pgp_mpi_free(pk->pub.dsa.g);
			pgp_mpi_free(pk->pub.dsa.y);
			pgp_mpi_free(pk->sec.dsa.x);
			break;
	}
	px_memset(pk, 0, sizeof(*pk));
	px_free(pk);
}

static int
calc_key_id(PGP_PubKey *pk)
{
	int			res;
	PX_MD	   *md;
	int			len;
	uint8		hdr[3];
	uint8		hash[20];

	res = pgp_load_digest(PGP_DIGEST_SHA1, &md);
	if (res < 0)
		return res;

	len = 1 + 4 + 1;
	switch (pk->algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
			len += 2 + pk->pub.elg.p->bytes;
			len += 2 + pk->pub.elg.g->bytes;
			len += 2 + pk->pub.elg.y->bytes;
			break;
		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			len += 2 + pk->pub.rsa.n->bytes;
			len += 2 + pk->pub.rsa.e->bytes;
			break;
		case PGP_PUB_DSA_SIGN:
			len += 2 + pk->pub.dsa.p->bytes;
			len += 2 + pk->pub.dsa.q->bytes;
			len += 2 + pk->pub.dsa.g->bytes;
			len += 2 + pk->pub.dsa.y->bytes;
			break;
	}

	hdr[0] = 0x99;
	hdr[1] = len >> 8;
	hdr[2] = len & 0xFF;
	px_md_update(md, hdr, 3);

	px_md_update(md, &pk->ver, 1);
	px_md_update(md, pk->time, 4);
	px_md_update(md, &pk->algo, 1);

	switch (pk->algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
			pgp_mpi_hash(md, pk->pub.elg.p);
			pgp_mpi_hash(md, pk->pub.elg.g);
			pgp_mpi_hash(md, pk->pub.elg.y);
			break;
		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			pgp_mpi_hash(md, pk->pub.rsa.n);
			pgp_mpi_hash(md, pk->pub.rsa.e);
			break;
		case PGP_PUB_DSA_SIGN:
			pgp_mpi_hash(md, pk->pub.dsa.p);
			pgp_mpi_hash(md, pk->pub.dsa.q);
			pgp_mpi_hash(md, pk->pub.dsa.g);
			pgp_mpi_hash(md, pk->pub.dsa.y);
			break;
	}

	px_md_finish(md, hash);
	px_md_free(md);

	memcpy(pk->key_id, hash + 12, 8);
	px_memset(hash, 0, 20);

	return 0;
}

int
_pgp_read_public_key(PullFilter *pkt, PGP_PubKey **pk_p)
{
	int			res;
	PGP_PubKey *pk;

	res = pgp_key_alloc(&pk);
	if (res < 0)
		return res;

	/* get version */
	GETBYTE(pkt, pk->ver);
	if (pk->ver != 4)
	{
		res = PXE_PGP_NOT_V4_KEYPKT;
		goto out;
	}

	/* read time */
	res = pullf_read_fixed(pkt, 4, pk->time);
	if (res < 0)
		goto out;

	/* pubkey algorithm */
	GETBYTE(pkt, pk->algo);

	switch (pk->algo)
	{
		case PGP_PUB_DSA_SIGN:
			res = pgp_mpi_read(pkt, &pk->pub.dsa.p);
			if (res < 0)
				break;
			res = pgp_mpi_read(pkt, &pk->pub.dsa.q);
			if (res < 0)
				break;
			res = pgp_mpi_read(pkt, &pk->pub.dsa.g);
			if (res < 0)
				break;
			res = pgp_mpi_read(pkt, &pk->pub.dsa.y);
			if (res < 0)
				break;

			res = calc_key_id(pk);
			break;

		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			res = pgp_mpi_read(pkt, &pk->pub.rsa.n);
			if (res < 0)
				break;
			res = pgp_mpi_read(pkt, &pk->pub.rsa.e);
			if (res < 0)
				break;

			res = calc_key_id(pk);

			if (pk->algo != PGP_PUB_RSA_SIGN)
				pk->can_encrypt = 1;
			break;

		case PGP_PUB_ELG_ENCRYPT:
			res = pgp_mpi_read(pkt, &pk->pub.elg.p);
			if (res < 0)
				break;
			res = pgp_mpi_read(pkt, &pk->pub.elg.g);
			if (res < 0)
				break;
			res = pgp_mpi_read(pkt, &pk->pub.elg.y);
			if (res < 0)
				break;

			res = calc_key_id(pk);

			pk->can_encrypt = 1;
			break;

		default:
			px_debug("unknown public algo: %d", pk->algo);
			res = PXE_PGP_UNKNOWN_PUBALGO;
	}

out:
	if (res < 0)
		pgp_key_free(pk);
	else
		*pk_p = pk;

	return res;
}

#define HIDE_CLEAR 0
#define HIDE_CKSUM 255
#define HIDE_SHA1 254

static int
check_key_sha1(PullFilter *src, PGP_PubKey *pk)
{
	int			res;
	uint8		got_sha1[20];
	uint8		my_sha1[20];
	PX_MD	   *md;

	res = pullf_read_fixed(src, 20, got_sha1);
	if (res < 0)
		return res;

	res = pgp_load_digest(PGP_DIGEST_SHA1, &md);
	if (res < 0)
		goto err;
	switch (pk->algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
			pgp_mpi_hash(md, pk->sec.elg.x);
			break;
		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			pgp_mpi_hash(md, pk->sec.rsa.d);
			pgp_mpi_hash(md, pk->sec.rsa.p);
			pgp_mpi_hash(md, pk->sec.rsa.q);
			pgp_mpi_hash(md, pk->sec.rsa.u);
			break;
		case PGP_PUB_DSA_SIGN:
			pgp_mpi_hash(md, pk->sec.dsa.x);
			break;
	}
	px_md_finish(md, my_sha1);
	px_md_free(md);

	if (memcmp(my_sha1, got_sha1, 20) != 0)
	{
		px_debug("key sha1 check failed");
		res = PXE_PGP_KEYPKT_CORRUPT;
	}
err:
	px_memset(got_sha1, 0, 20);
	px_memset(my_sha1, 0, 20);
	return res;
}

static int
check_key_cksum(PullFilter *src, PGP_PubKey *pk)
{
	int			res;
	unsigned	got_cksum,
				my_cksum = 0;
	uint8		buf[2];

	res = pullf_read_fixed(src, 2, buf);
	if (res < 0)
		return res;

	got_cksum = ((unsigned) buf[0] << 8) + buf[1];
	switch (pk->algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
			my_cksum = pgp_mpi_cksum(0, pk->sec.elg.x);
			break;
		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			my_cksum = pgp_mpi_cksum(0, pk->sec.rsa.d);
			my_cksum = pgp_mpi_cksum(my_cksum, pk->sec.rsa.p);
			my_cksum = pgp_mpi_cksum(my_cksum, pk->sec.rsa.q);
			my_cksum = pgp_mpi_cksum(my_cksum, pk->sec.rsa.u);
			break;
		case PGP_PUB_DSA_SIGN:
			my_cksum = pgp_mpi_cksum(0, pk->sec.dsa.x);
			break;
	}
	if (my_cksum != got_cksum)
	{
		px_debug("key cksum check failed");
		return PXE_PGP_KEYPKT_CORRUPT;
	}
	return 0;
}

static int
process_secret_key(PullFilter *pkt, PGP_PubKey **pk_p,
				   const uint8 *key, int key_len)
{
	int			res;
	int			hide_type;
	int			cipher_algo;
	int			bs;
	uint8		iv[512];
	PullFilter *pf_decrypt = NULL,
			   *pf_key;
	PGP_CFB    *cfb = NULL;
	PGP_S2K		s2k;
	PGP_PubKey *pk;

	/* first read public key part */
	res = _pgp_read_public_key(pkt, &pk);
	if (res < 0)
		return res;

	/*
	 * is secret key encrypted?
	 */
	GETBYTE(pkt, hide_type);
	if (hide_type == HIDE_SHA1 || hide_type == HIDE_CKSUM)
	{
		if (key == NULL)
			return PXE_PGP_NEED_SECRET_PSW;
		GETBYTE(pkt, cipher_algo);
		res = pgp_s2k_read(pkt, &s2k);
		if (res < 0)
			return res;

		res = pgp_s2k_process(&s2k, cipher_algo, key, key_len);
		if (res < 0)
			return res;

		bs = pgp_get_cipher_block_size(cipher_algo);
		if (bs == 0)
		{
			px_debug("unknown cipher algo=%d", cipher_algo);
			return PXE_PGP_UNSUPPORTED_CIPHER;
		}
		res = pullf_read_fixed(pkt, bs, iv);
		if (res < 0)
			return res;

		/*
		 * create decrypt filter
		 */
		res = pgp_cfb_create(&cfb, cipher_algo, s2k.key, s2k.key_len, 0, iv);
		if (res < 0)
			return res;
		res = pullf_create(&pf_decrypt, &pgp_decrypt_filter, cfb, pkt);
		if (res < 0)
			return res;
		pf_key = pf_decrypt;
	}
	else if (hide_type == HIDE_CLEAR)
	{
		pf_key = pkt;
	}
	else
	{
		px_debug("unknown hide type");
		return PXE_PGP_KEYPKT_CORRUPT;
	}

	/* read secret key */
	switch (pk->algo)
	{
		case PGP_PUB_RSA_SIGN:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			res = pgp_mpi_read(pf_key, &pk->sec.rsa.d);
			if (res < 0)
				break;
			res = pgp_mpi_read(pf_key, &pk->sec.rsa.p);
			if (res < 0)
				break;
			res = pgp_mpi_read(pf_key, &pk->sec.rsa.q);
			if (res < 0)
				break;
			res = pgp_mpi_read(pf_key, &pk->sec.rsa.u);
			if (res < 0)
				break;
			break;
		case PGP_PUB_ELG_ENCRYPT:
			res = pgp_mpi_read(pf_key, &pk->sec.elg.x);
			break;
		case PGP_PUB_DSA_SIGN:
			res = pgp_mpi_read(pf_key, &pk->sec.dsa.x);
			break;
		default:
			px_debug("unknown public algo: %d", pk->algo);
			res = PXE_PGP_KEYPKT_CORRUPT;
	}
	/* read checksum / sha1 */
	if (res >= 0)
	{
		if (hide_type == HIDE_SHA1)
			res = check_key_sha1(pf_key, pk);
		else
			res = check_key_cksum(pf_key, pk);
	}
	if (res >= 0)
		res = pgp_expect_packet_end(pf_key);

	if (pf_decrypt)
		pullf_free(pf_decrypt);
	if (cfb)
		pgp_cfb_free(cfb);

	if (res < 0)
		pgp_key_free(pk);
	else
		*pk_p = pk;

	return res;
}

static int
internal_read_key(PullFilter *src, PGP_PubKey **pk_p,
				  const uint8 *psw, int psw_len, int pubtype)
{
	PullFilter *pkt = NULL;
	int			res;
	uint8		tag;
	int			len;
	PGP_PubKey *enc_key = NULL;
	PGP_PubKey *pk = NULL;
	int			got_main_key = 0;

	/*
	 * Search for encryption key.
	 *
	 * Error out on anything fancy.
	 */
	while (1)
	{
		res = pgp_parse_pkt_hdr(src, &tag, &len, 0);
		if (res <= 0)
			break;
		res = pgp_create_pkt_reader(&pkt, src, len, res, NULL);
		if (res < 0)
			break;

		switch (tag)
		{
			case PGP_PKT_PUBLIC_KEY:
			case PGP_PKT_SECRET_KEY:
				if (got_main_key)
				{
					res = PXE_PGP_MULTIPLE_KEYS;
					break;
				}
				got_main_key = 1;
				res = pgp_skip_packet(pkt);
				break;

			case PGP_PKT_PUBLIC_SUBKEY:
				if (pubtype != 0)
					res = PXE_PGP_EXPECT_SECRET_KEY;
				else
					res = _pgp_read_public_key(pkt, &pk);
				break;

			case PGP_PKT_SECRET_SUBKEY:
				if (pubtype != 1)
					res = PXE_PGP_EXPECT_PUBLIC_KEY;
				else
					res = process_secret_key(pkt, &pk, psw, psw_len);
				break;

			case PGP_PKT_SIGNATURE:
			case PGP_PKT_MARKER:
			case PGP_PKT_TRUST:
			case PGP_PKT_USER_ID:
			case PGP_PKT_USER_ATTR:
			case PGP_PKT_PRIV_61:
				res = pgp_skip_packet(pkt);
				break;
			default:
				px_debug("unknown/unexpected packet: %d", tag);
				res = PXE_PGP_UNEXPECTED_PKT;
		}
		pullf_free(pkt);
		pkt = NULL;

		if (pk != NULL)
		{
			if (res >= 0 && pk->can_encrypt)
			{
				if (enc_key == NULL)
				{
					enc_key = pk;
					pk = NULL;
				}
				else
					res = PXE_PGP_MULTIPLE_SUBKEYS;
			}

			if (pk)
				pgp_key_free(pk);
			pk = NULL;
		}

		if (res < 0)
			break;
	}

	if (pkt)
		pullf_free(pkt);

	if (res < 0)
	{
		if (enc_key)
			pgp_key_free(enc_key);
		return res;
	}

	if (!enc_key)
		res = PXE_PGP_NO_USABLE_KEY;
	else
		*pk_p = enc_key;
	return res;
}

int
pgp_set_pubkey(PGP_Context *ctx, MBuf *keypkt,
			   const uint8 *key, int key_len, int pubtype)
{
	int			res;
	PullFilter *src;
	PGP_PubKey *pk = NULL;

	res = pullf_create_mbuf_reader(&src, keypkt);
	if (res < 0)
		return res;

	res = internal_read_key(src, &pk, key, key_len, pubtype);
	pullf_free(src);

	if (res >= 0)
		ctx->pub_key = pk;

	return res < 0 ? res : 0;
}
