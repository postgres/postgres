/*
 * pgp-info.c
 *	  Provide info about PGP data.
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
 * contrib/pgcrypto/pgp-info.c
 */
#include "postgres.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

static int
read_pubkey_keyid(PullFilter *pkt, uint8 *keyid_buf)
{
	int			res;
	PGP_PubKey *pk = NULL;

	res = _pgp_read_public_key(pkt, &pk);
	if (res < 0)
		goto err;

	/* skip secret key part, if it exists */
	res = pgp_skip_packet(pkt);
	if (res < 0)
		goto err;

	/* is it encryption key */
	switch (pk->algo)
	{
		case PGP_PUB_ELG_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT:
		case PGP_PUB_RSA_ENCRYPT_SIGN:
			memcpy(keyid_buf, pk->key_id, 8);
			res = 1;
			break;
		default:
			res = 0;
	}

err:
	pgp_key_free(pk);
	return res;
}

static int
read_pubenc_keyid(PullFilter *pkt, uint8 *keyid_buf)
{
	uint8		ver;
	int			res;

	GETBYTE(pkt, ver);
	if (ver != 3)
		return -1;

	res = pullf_read_fixed(pkt, 8, keyid_buf);
	if (res < 0)
		return res;

	return pgp_skip_packet(pkt);
}

static const char hextbl[] = "0123456789ABCDEF";

static int
print_key(uint8 *keyid, char *dst)
{
	int			i;
	unsigned	c;

	for (i = 0; i < 8; i++)
	{
		c = keyid[i];
		*dst++ = hextbl[(c >> 4) & 0x0F];
		*dst++ = hextbl[c & 0x0F];
	}
	*dst = 0;
	return 8 * 2;
}

static const uint8 any_key[] =
{0, 0, 0, 0, 0, 0, 0, 0};

/*
 * dst should have room for 17 bytes
 */
int
pgp_get_keyid(MBuf *pgp_data, char *dst)
{
	int			res;
	PullFilter *src;
	PullFilter *pkt = NULL;
	int			len;
	uint8		tag;
	int			got_pub_key = 0,
				got_symenc_key = 0,
				got_pubenc_key = 0;
	int			got_data = 0;
	uint8		keyid_buf[8];
	int			got_main_key = 0;


	res = pullf_create_mbuf_reader(&src, pgp_data);
	if (res < 0)
		return res;

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
			case PGP_PKT_SECRET_KEY:
			case PGP_PKT_PUBLIC_KEY:
				/* main key is for signing, so ignore it */
				if (!got_main_key)
				{
					got_main_key = 1;
					res = pgp_skip_packet(pkt);
				}
				else
					res = PXE_PGP_MULTIPLE_KEYS;
				break;
			case PGP_PKT_SECRET_SUBKEY:
			case PGP_PKT_PUBLIC_SUBKEY:
				res = read_pubkey_keyid(pkt, keyid_buf);
				if (res < 0)
					break;
				if (res > 0)
					got_pub_key++;
				break;
			case PGP_PKT_PUBENCRYPTED_SESSKEY:
				got_pubenc_key++;
				res = read_pubenc_keyid(pkt, keyid_buf);
				break;
			case PGP_PKT_SYMENCRYPTED_DATA:
			case PGP_PKT_SYMENCRYPTED_DATA_MDC:
				/* don't skip it, just stop */
				got_data = 1;
				break;
			case PGP_PKT_SYMENCRYPTED_SESSKEY:
				got_symenc_key++;
				/* fallthru */
			case PGP_PKT_SIGNATURE:
			case PGP_PKT_MARKER:
			case PGP_PKT_TRUST:
			case PGP_PKT_USER_ID:
			case PGP_PKT_USER_ATTR:
			case PGP_PKT_PRIV_61:
				res = pgp_skip_packet(pkt);
				break;
			default:
				res = PXE_PGP_CORRUPT_DATA;
		}

		if (pkt)
			pullf_free(pkt);
		pkt = NULL;

		if (res < 0 || got_data)
			break;
	}

	pullf_free(src);
	if (pkt)
		pullf_free(pkt);

	if (res < 0)
		return res;

	/* now check sanity */
	if (got_pub_key && got_pubenc_key)
		res = PXE_PGP_CORRUPT_DATA;

	if (got_pub_key > 1)
		res = PXE_PGP_MULTIPLE_KEYS;

	if (got_pubenc_key > 1)
		res = PXE_PGP_MULTIPLE_KEYS;

	/*
	 * if still ok, look what we got
	 */
	if (res >= 0)
	{
		if (got_pubenc_key || got_pub_key)
		{
			if (memcmp(keyid_buf, any_key, 8) == 0)
			{
				memcpy(dst, "ANYKEY", 7);
				res = 6;
			}
			else
				res = print_key(keyid_buf, dst);
		}
		else if (got_symenc_key)
		{
			memcpy(dst, "SYMKEY", 7);
			res = 6;
		}
		else
			res = PXE_PGP_NO_USABLE_KEY;
	}

	return res;
}
