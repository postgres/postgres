/*
 * pgp-mpi-openssl.c
 *	  OpenPGP MPI functions using OpenSSL BIGNUM code.
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
 * $PostgreSQL: pgsql/contrib/pgcrypto/pgp-mpi-openssl.c,v 1.4 2005/10/15 02:49:06 momjian Exp $
 */
#include "postgres.h"

#include <openssl/bn.h>

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

static BIGNUM *
mpi_to_bn(PGP_MPI * n)
{
	BIGNUM	   *bn = BN_bin2bn(n->data, n->bytes, NULL);

	if (!bn)
		return NULL;
	if (BN_num_bits(bn) != n->bits)
	{
		px_debug("mpi_to_bn: bignum conversion failed: mpi=%d, bn=%d",
				 n->bits, BN_num_bits(bn));
		BN_clear_free(bn);
		return NULL;
	}
	return bn;
}

static PGP_MPI *
bn_to_mpi(BIGNUM *bn)
{
	int			res;
	PGP_MPI    *n;

	res = pgp_mpi_alloc(BN_num_bits(bn), &n);
	if (res < 0)
		return NULL;

	if (BN_num_bytes(bn) != n->bytes)
	{
		px_debug("bn_to_mpi: bignum conversion failed: bn=%d, mpi=%d",
				 BN_num_bytes(bn), n->bytes);
		pgp_mpi_free(n);
		return NULL;
	}
	BN_bn2bin(bn, n->data);
	return n;
}

/*
 * Decide the number of bits in the random componont k
 *
 * It should be in the same range as p for signing (which
 * is deprecated), but can be much smaller for encrypting.
 *
 * Until I research it further, I just mimic gpg behaviour.
 * It has a special mapping table, for values <= 5120,
 * above that it uses 'arbitrary high number'.	Following
 * algorihm hovers 10-70 bits above gpg values.  And for
 * larger p, it uses gpg's algorihm.
 *
 * The point is - if k gets large, encryption will be
 * really slow.  It does not matter for decryption.
 */
static int
decide_k_bits(int p_bits)
{
	if (p_bits <= 5120)
		return p_bits / 10 + 160;
	else
		return (p_bits / 8 + 200) * 3 / 2;
}

int
pgp_elgamal_encrypt(PGP_PubKey * pk, PGP_MPI * _m,
					PGP_MPI ** c1_p, PGP_MPI ** c2_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	int			k_bits;
	BIGNUM	   *m = mpi_to_bn(_m);
	BIGNUM	   *p = mpi_to_bn(pk->pub.elg.p);
	BIGNUM	   *g = mpi_to_bn(pk->pub.elg.g);
	BIGNUM	   *y = mpi_to_bn(pk->pub.elg.y);
	BIGNUM	   *k = BN_new();
	BIGNUM	   *yk = BN_new();
	BIGNUM	   *c1 = BN_new();
	BIGNUM	   *c2 = BN_new();
	BN_CTX	   *tmp = BN_CTX_new();

	if (!m || !p || !g || !y || !k || !yk || !c1 || !c2 || !tmp)
		goto err;

	/*
	 * generate k
	 */
	k_bits = decide_k_bits(BN_num_bits(p));
	if (!BN_rand(k, k_bits, 0, 0))
		goto err;

	/*
	 * c1 = g^k c2 = m * y^k
	 */
	if (!BN_mod_exp(c1, g, k, p, tmp))
		goto err;
	if (!BN_mod_exp(yk, y, k, p, tmp))
		goto err;
	if (!BN_mod_mul(c2, m, yk, p, tmp))
		goto err;

	/* result */
	*c1_p = bn_to_mpi(c1);
	*c2_p = bn_to_mpi(c2);
	if (*c1_p && *c2_p)
		res = 0;
err:
	if (tmp)
		BN_CTX_free(tmp);
	if (c2)
		BN_clear_free(c2);
	if (c1)
		BN_clear_free(c1);
	if (yk)
		BN_clear_free(yk);
	if (k)
		BN_clear_free(k);
	if (y)
		BN_clear_free(y);
	if (g)
		BN_clear_free(g);
	if (p)
		BN_clear_free(p);
	if (m)
		BN_clear_free(m);
	return res;
}

int
pgp_elgamal_decrypt(PGP_PubKey * pk, PGP_MPI * _c1, PGP_MPI * _c2,
					PGP_MPI ** msg_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	BIGNUM	   *c1 = mpi_to_bn(_c1);
	BIGNUM	   *c2 = mpi_to_bn(_c2);
	BIGNUM	   *p = mpi_to_bn(pk->pub.elg.p);
	BIGNUM	   *x = mpi_to_bn(pk->sec.elg.x);
	BIGNUM	   *c1x = BN_new();
	BIGNUM	   *div = BN_new();
	BIGNUM	   *m = BN_new();
	BN_CTX	   *tmp = BN_CTX_new();

	if (!c1 || !c2 || !p || !x || !c1x || !div || !m || !tmp)
		goto err;

	/*
	 * m = c2 / (c1^x)
	 */
	if (!BN_mod_exp(c1x, c1, x, p, tmp))
		goto err;
	if (!BN_mod_inverse(div, c1x, p, tmp))
		goto err;
	if (!BN_mod_mul(m, c2, div, p, tmp))
		goto err;

	/* result */
	*msg_p = bn_to_mpi(m);
	if (*msg_p)
		res = 0;
err:
	if (tmp)
		BN_CTX_free(tmp);
	if (m)
		BN_clear_free(m);
	if (div)
		BN_clear_free(div);
	if (c1x)
		BN_clear_free(c1x);
	if (x)
		BN_clear_free(x);
	if (p)
		BN_clear_free(p);
	if (c2)
		BN_clear_free(c2);
	if (c1)
		BN_clear_free(c1);
	return res;
}

int
pgp_rsa_encrypt(PGP_PubKey * pk, PGP_MPI * _m, PGP_MPI ** c_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	BIGNUM	   *m = mpi_to_bn(_m);
	BIGNUM	   *e = mpi_to_bn(pk->pub.rsa.e);
	BIGNUM	   *n = mpi_to_bn(pk->pub.rsa.n);
	BIGNUM	   *c = BN_new();
	BN_CTX	   *tmp = BN_CTX_new();

	if (!m || !e || !n || !c || !tmp)
		goto err;

	/*
	 * c = m ^ e
	 */
	if (!BN_mod_exp(c, m, e, n, tmp))
		goto err;

	*c_p = bn_to_mpi(c);
	if (*c_p)
		res = 0;
err:
	if (tmp)
		BN_CTX_free(tmp);
	if (c)
		BN_clear_free(c);
	if (n)
		BN_clear_free(n);
	if (e)
		BN_clear_free(e);
	if (m)
		BN_clear_free(m);
	return res;
}

int
pgp_rsa_decrypt(PGP_PubKey * pk, PGP_MPI * _c, PGP_MPI ** m_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	BIGNUM	   *c = mpi_to_bn(_c);
	BIGNUM	   *d = mpi_to_bn(pk->sec.rsa.d);
	BIGNUM	   *n = mpi_to_bn(pk->pub.rsa.n);
	BIGNUM	   *m = BN_new();
	BN_CTX	   *tmp = BN_CTX_new();

	if (!m || !d || !n || !c || !tmp)
		goto err;

	/*
	 * m = c ^ d
	 */
	if (!BN_mod_exp(m, c, d, n, tmp))
		goto err;

	*m_p = bn_to_mpi(m);
	if (*m_p)
		res = 0;
err:
	if (tmp)
		BN_CTX_free(tmp);
	if (m)
		BN_clear_free(m);
	if (n)
		BN_clear_free(n);
	if (d)
		BN_clear_free(d);
	if (c)
		BN_clear_free(c);
	return res;
}
