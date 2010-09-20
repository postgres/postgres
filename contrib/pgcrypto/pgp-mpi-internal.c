/*
 * pgp-mpi-internal.c
 *	  OpenPGP MPI functions.
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
 * contrib/pgcrypto/pgp-mpi-internal.c
 */
#include "postgres.h"

#include "imath.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

static mpz_t *
mp_new()
{
	mpz_t	   *mp = mp_int_alloc();

	mp_int_init_size(mp, 256);
	return mp;
}

static void
mp_clear_free(mpz_t *a)
{
	if (!a)
		return;
	/* fixme: no clear? */
	mp_int_free(a);
}


static int
mp_px_rand(uint32 bits, mpz_t *res)
{
	int			err;
	unsigned	bytes = (bits + 7) / 8;
	int			last_bits = bits & 7;
	uint8	   *buf;

	buf = px_alloc(bytes);
	err = px_get_random_bytes(buf, bytes);
	if (err < 0)
	{
		px_free(buf);
		return err;
	}

	/* clear unnecessary bits and set last bit to one */
	if (last_bits)
	{
		buf[0] >>= 8 - last_bits;
		buf[0] |= 1 << (last_bits - 1);
	}
	else
		buf[0] |= 1 << 7;

	mp_int_read_unsigned(res, buf, bytes);

	px_free(buf);

	return 0;
}

static void
mp_modmul(mpz_t *a, mpz_t *b, mpz_t *p, mpz_t *res)
{
	mpz_t	   *tmp = mp_new();

	mp_int_mul(a, b, tmp);
	mp_int_mod(tmp, p, res);
	mp_clear_free(tmp);
}

static mpz_t *
mpi_to_bn(PGP_MPI *n)
{
	mpz_t	   *bn = mp_new();

	mp_int_read_unsigned(bn, n->data, n->bytes);

	if (!bn)
		return NULL;
	if (mp_int_count_bits(bn) != n->bits)
	{
		px_debug("mpi_to_bn: bignum conversion failed: mpi=%d, bn=%d",
				 n->bits, mp_int_count_bits(bn));
		mp_clear_free(bn);
		return NULL;
	}
	return bn;
}

static PGP_MPI *
bn_to_mpi(mpz_t *bn)
{
	int			res;
	PGP_MPI    *n;
	int			bytes;

	res = pgp_mpi_alloc(mp_int_count_bits(bn), &n);
	if (res < 0)
		return NULL;

	bytes = (mp_int_count_bits(bn) + 7) / 8;
	if (bytes != n->bytes)
	{
		px_debug("bn_to_mpi: bignum conversion failed: bn=%d, mpi=%d",
				 bytes, n->bytes);
		pgp_mpi_free(n);
		return NULL;
	}
	mp_int_to_unsigned(bn, n->data, n->bytes);
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
pgp_elgamal_encrypt(PGP_PubKey *pk, PGP_MPI *_m,
					PGP_MPI **c1_p, PGP_MPI **c2_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	int			k_bits;
	mpz_t	   *m = mpi_to_bn(_m);
	mpz_t	   *p = mpi_to_bn(pk->pub.elg.p);
	mpz_t	   *g = mpi_to_bn(pk->pub.elg.g);
	mpz_t	   *y = mpi_to_bn(pk->pub.elg.y);
	mpz_t	   *k = mp_new();
	mpz_t	   *yk = mp_new();
	mpz_t	   *c1 = mp_new();
	mpz_t	   *c2 = mp_new();

	if (!m || !p || !g || !y || !k || !yk || !c1 || !c2)
		goto err;

	/*
	 * generate k
	 */
	k_bits = decide_k_bits(mp_int_count_bits(p));
	res = mp_px_rand(k_bits, k);
	if (res < 0)
		return res;

	/*
	 * c1 = g^k c2 = m * y^k
	 */
	mp_int_exptmod(g, k, p, c1);
	mp_int_exptmod(y, k, p, yk);
	mp_modmul(m, yk, p, c2);

	/* result */
	*c1_p = bn_to_mpi(c1);
	*c2_p = bn_to_mpi(c2);
	if (*c1_p && *c2_p)
		res = 0;
err:
	mp_clear_free(c2);
	mp_clear_free(c1);
	mp_clear_free(yk);
	mp_clear_free(k);
	mp_clear_free(y);
	mp_clear_free(g);
	mp_clear_free(p);
	mp_clear_free(m);
	return res;
}

int
pgp_elgamal_decrypt(PGP_PubKey *pk, PGP_MPI *_c1, PGP_MPI *_c2,
					PGP_MPI **msg_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	mpz_t	   *c1 = mpi_to_bn(_c1);
	mpz_t	   *c2 = mpi_to_bn(_c2);
	mpz_t	   *p = mpi_to_bn(pk->pub.elg.p);
	mpz_t	   *x = mpi_to_bn(pk->sec.elg.x);
	mpz_t	   *c1x = mp_new();
	mpz_t	   *div = mp_new();
	mpz_t	   *m = mp_new();

	if (!c1 || !c2 || !p || !x || !c1x || !div || !m)
		goto err;

	/*
	 * m = c2 / (c1^x)
	 */
	mp_int_exptmod(c1, x, p, c1x);
	mp_int_invmod(c1x, p, div);
	mp_modmul(c2, div, p, m);

	/* result */
	*msg_p = bn_to_mpi(m);
	if (*msg_p)
		res = 0;
err:
	mp_clear_free(m);
	mp_clear_free(div);
	mp_clear_free(c1x);
	mp_clear_free(x);
	mp_clear_free(p);
	mp_clear_free(c2);
	mp_clear_free(c1);
	return res;
}

int
pgp_rsa_encrypt(PGP_PubKey *pk, PGP_MPI *_m, PGP_MPI **c_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	mpz_t	   *m = mpi_to_bn(_m);
	mpz_t	   *e = mpi_to_bn(pk->pub.rsa.e);
	mpz_t	   *n = mpi_to_bn(pk->pub.rsa.n);
	mpz_t	   *c = mp_new();

	if (!m || !e || !n || !c)
		goto err;

	/*
	 * c = m ^ e
	 */
	mp_int_exptmod(m, e, n, c);

	*c_p = bn_to_mpi(c);
	if (*c_p)
		res = 0;
err:
	mp_clear_free(c);
	mp_clear_free(n);
	mp_clear_free(e);
	mp_clear_free(m);
	return res;
}

int
pgp_rsa_decrypt(PGP_PubKey *pk, PGP_MPI *_c, PGP_MPI **m_p)
{
	int			res = PXE_PGP_MATH_FAILED;
	mpz_t	   *c = mpi_to_bn(_c);
	mpz_t	   *d = mpi_to_bn(pk->sec.rsa.d);
	mpz_t	   *n = mpi_to_bn(pk->pub.rsa.n);
	mpz_t	   *m = mp_new();

	if (!m || !d || !n || !c)
		goto err;

	/*
	 * m = c ^ d
	 */
	mp_int_exptmod(c, d, n, m);

	*m_p = bn_to_mpi(m);
	if (*m_p)
		res = 0;
err:
	mp_clear_free(m);
	mp_clear_free(n);
	mp_clear_free(d);
	mp_clear_free(c);
	return res;
}
