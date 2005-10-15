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
 * $PostgreSQL: pgsql/contrib/pgcrypto/pgp-mpi-internal.c,v 1.4 2005/10/15 02:49:06 momjian Exp $
 */
#include "postgres.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

int
pgp_elgamal_encrypt(PGP_PubKey * pk, PGP_MPI * _m,
					PGP_MPI ** c1_p, PGP_MPI ** c2_p)
{
	return PXE_PGP_NO_BIGNUM;
}

int
pgp_elgamal_decrypt(PGP_PubKey * pk, PGP_MPI * _c1, PGP_MPI * _c2,
					PGP_MPI ** msg_p)
{
	return PXE_PGP_NO_BIGNUM;
}

int
pgp_rsa_encrypt(PGP_PubKey * pk, PGP_MPI * m, PGP_MPI ** c)
{
	return PXE_PGP_NO_BIGNUM;
}

int
pgp_rsa_decrypt(PGP_PubKey * pk, PGP_MPI * c, PGP_MPI ** m)
{
	return PXE_PGP_NO_BIGNUM;
}
