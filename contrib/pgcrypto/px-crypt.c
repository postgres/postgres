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
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: px-crypt.c,v 1.1 2001/08/21 01:32:01 momjian Exp $
 */

#include <postgres.h>
#include "px-crypt.h"


#ifndef PX_SYSTEM_CRYPT

static char *
run_crypt_des(const char *psw, const char *salt,
			 char *buf, unsigned len)
{
	char	   *res;

	res = px_crypt_des(psw, salt);
	if (strlen(res) > len - 1)
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
	if (!res)
		return NULL;
	strcpy(buf, res);
	return buf;
}

static struct
{
	char		*id;
	unsigned	id_len;
	char	   *(*crypt) (const char *psw, const char *salt,
									  char *buf, unsigned len);
}			px_crypt_list[] =

{
	{ "$2a$", 4, run_crypt_bf },
	{ "$2$", 3, NULL },							/* N/A */
	{ "$1$", 3, run_crypt_md5 },
	{ "_", 1, run_crypt_des },
	{ "", 0, run_crypt_des },
	{ NULL, 0, NULL }
};

char *
px_crypt(const char *psw, const char *salt, char *buf, unsigned len)
{
	int			i;

	for (i = 0; px_crypt_list[i].id; i++)
	{
		if (!px_crypt_list[i].id_len)
			break;
		if (!strncmp(salt, px_crypt_list[i].id, px_crypt_list[i].id_len))
			break;
	}

	if (px_crypt_list[i].crypt == NULL)
		return NULL;

	return px_crypt_list[i].crypt(psw, salt, buf, len);
}

#else							/* PX_SYSTEM_CRYPT */

extern char *crypt(const char *psw, const char *salt);

char *
px_crypt(const char *psw, const char *salt,
		 char *buf, unsigned len)
{
	char	   *res;

	res = crypt(psw, salt);
	if (!res || strlen(res) >= len)
		return NULL;
	strcpy(buf, res);
	return buf;
}
#endif

/*
 * salt generators
 */

static int my_rand64()
{
	return random() % 64;
}

static uint
gen_des_salt(char *buf)
{
	buf[0] = px_crypt_a64[my_rand64()];
	buf[1] = px_crypt_a64[my_rand64()];
	buf[2] = 0;
	
	return 2;
}

static uint
gen_xdes_salt(char *buf)
{
	strcpy(buf, "_12345678");
	
	px_crypt_to64(buf+1, (long)PX_XDES_ROUNDS, 4);
	px_crypt_to64(buf+5, random(), 4);
	
	return 9;
}

static uint
gen_md5_salt(char *buf)
{
	int i;
	strcpy(buf, "$1$12345678$");
	
	for (i = 0; i < 8; i++)
		buf[3 + i] = px_crypt_a64[my_rand64()];
	
	return 12;
}

static uint
gen_bf_salt(char *buf)
{
	int i, count;
	char *s;
	char saltbuf[16+3];
	unsigned slen = 16;
	uint32 *v;

	for (i = 0; i < slen; i++)
		saltbuf[i] = random() & 255;
	saltbuf[16] = 0;
	saltbuf[17] = 0;
	saltbuf[18] = 0;

	strcpy(buf, "$2a$00$0123456789012345678901");

	count = PX_BF_ROUNDS;
	buf[4] = '0' + count / 10;
	buf[5] = '0' + count % 10;
	
	s = buf + 7;
	for (i = 0; i < slen; )
	{
		v = (uint32 *)&saltbuf[i];
		if (i + 3 <= slen)
			px_crypt_to64(s, *v, 4);
		else
			/* slen-i could be 1,2 make it 2,3 */
			px_crypt_to64(s, *v, slen-i+1);
		s += 4;
		i += 3;
	}
		
	s = buf;
	/*s = _crypt_gensalt_blowfish_rn(count, saltbuf, 16, buf, PX_MAX_CRYPT);*/
	
	return s ? strlen(s) : 0;
}

struct generator {
	char *name;
	uint (*gen)(char *buf);
};

static struct generator gen_list [] = {
	{ "des", gen_des_salt },
	{ "md5", gen_md5_salt },
	{ "xdes", gen_xdes_salt },
	{ "bf", gen_bf_salt },
	{ NULL, NULL }
};

uint
px_gen_salt(const char *salt_type, char *buf)
{
	int i;
	struct generator *g;
	for (i = 0; gen_list[i].name; i++) {
		g = &gen_list[i];
		if (!strcasecmp(g->name, salt_type))
			return g->gen(buf);
	}

	return 0;
}

