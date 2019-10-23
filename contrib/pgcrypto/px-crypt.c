/*
 * px-crypt.c
 *		Wrapper for various crypt algorithms.
 *
 * Copyright (c) 2001 Marko Kreen
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
 * contrib/pgcrypto/px-crypt.c
 */

#include "postgres.h"

#include "px-crypt.h"
#include "px.h"

static char *
run_crypt_des(const char *psw, const char *salt,
			  char *buf, unsigned len)
{
	char	   *res;

	res = px_crypt_des(psw, salt);
	if (res == NULL || strlen(res) > len - 1)
		return NULL;
	strcpy(buf, res);
	return buf;
}

static char *
run_crypt_md5(const char *psw, const char *salt,
			  char *buf, unsigned len)
{
	char	   *res;

	res = px_crypt_md5(psw, salt, buf, len);
	return res;
}

static char *
run_crypt_bf(const char *psw, const char *salt,
			 char *buf, unsigned len)
{
	char	   *res;

	res = _crypt_blowfish_rn(psw, salt, buf, len);
	return res;
}

struct px_crypt_algo
{
	char	   *id;
	unsigned	id_len;
	char	   *(*crypt) (const char *psw, const char *salt,
						  char *buf, unsigned len);
};

static const struct px_crypt_algo
			px_crypt_list[] = {
	{"$2a$", 4, run_crypt_bf},
	{"$2x$", 4, run_crypt_bf},
	{"$2$", 3, NULL},			/* N/A */
	{"$1$", 3, run_crypt_md5},
	{"_", 1, run_crypt_des},
	{"", 0, run_crypt_des},
	{NULL, 0, NULL}
};

char *
px_crypt(const char *psw, const char *salt, char *buf, unsigned len)
{
	const struct px_crypt_algo *c;

	for (c = px_crypt_list; c->id; c++)
	{
		if (!c->id_len)
			break;
		if (strncmp(salt, c->id, c->id_len) == 0)
			break;
	}

	if (c->crypt == NULL)
		return NULL;

	return c->crypt(psw, salt, buf, len);
}

/*
 * salt generators
 */

struct generator
{
	char	   *name;
	char	   *(*gen) (unsigned long count, const char *input, int size,
						char *output, int output_size);
	int			input_len;
	int			def_rounds;
	int			min_rounds;
	int			max_rounds;
};

static struct generator gen_list[] = {
	{"des", _crypt_gensalt_traditional_rn, 2, 0, 0, 0},
	{"md5", _crypt_gensalt_md5_rn, 6, 0, 0, 0},
	{"xdes", _crypt_gensalt_extended_rn, 3, PX_XDES_ROUNDS, 1, 0xFFFFFF},
	{"bf", _crypt_gensalt_blowfish_rn, 16, PX_BF_ROUNDS, 4, 31},
	{NULL, NULL, 0, 0, 0, 0}
};

int
px_gen_salt(const char *salt_type, char *buf, int rounds)
{
	struct generator *g;
	char	   *p;
	char		rbuf[16];

	for (g = gen_list; g->name; g++)
		if (pg_strcasecmp(g->name, salt_type) == 0)
			break;

	if (g->name == NULL)
		return PXE_UNKNOWN_SALT_ALGO;

	if (g->def_rounds)
	{
		if (rounds == 0)
			rounds = g->def_rounds;

		if (rounds < g->min_rounds || rounds > g->max_rounds)
			return PXE_BAD_SALT_ROUNDS;
	}

	if (!pg_strong_random(rbuf, g->input_len))
		return PXE_NO_RANDOM;

	p = g->gen(rounds, rbuf, g->input_len, buf, PX_MAX_SALT_LEN);
	px_memset(rbuf, 0, sizeof(rbuf));

	if (p == NULL)
		return PXE_BAD_SALT_ROUNDS;

	return strlen(p);
}
