/*
 * pgcrypto.c
 *		Cryptographic digests for PostgreSQL.
 *
 * Copyright (c) 2000 Marko Kreen
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
 * $Id: pgcrypto.c,v 1.7 2001/03/22 03:59:10 momjian Exp $
 */

#include "postgres.h"

#include "utils/builtins.h"

#include "pgcrypto.h"

/*
 * NAMEDATALEN is used for hash names
 */
#if NAMEDATALEN < 16
#error "NAMEDATALEN < 16: too small"
#endif


/* exported functions */
Datum		digest(PG_FUNCTION_ARGS);
Datum		digest_exists(PG_FUNCTION_ARGS);

/* private stuff */
static pg_digest *
			find_digest(pg_digest * hbuf, text *name, int silent);


/* SQL function: hash(text, text) returns text */
PG_FUNCTION_INFO_V1(digest);

Datum
digest(PG_FUNCTION_ARGS)
{
	text	   *arg;
	text	   *name;
	uint		len,
				hlen;
	pg_digest  *h,
				_hbuf;
	text	   *res;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	name = PG_GETARG_TEXT_P(1);
	h = find_digest(&_hbuf, name, 0);	/* will give error if fails */

	hlen = h->length(h);

	res = (text *) palloc(hlen + VARHDRSZ);
	VARATT_SIZEP(res) = hlen + VARHDRSZ;

	arg = PG_GETARG_TEXT_P(0);
	len = VARSIZE(arg) - VARHDRSZ;

	h->digest(h, VARDATA(arg), len, VARDATA(res));

	PG_FREE_IF_COPY(arg, 0);
	PG_FREE_IF_COPY(name, 1);

	PG_RETURN_TEXT_P(res);
}

/* check if given hash exists */
PG_FUNCTION_INFO_V1(digest_exists);

Datum
digest_exists(PG_FUNCTION_ARGS)
{
	text	   *name;
	pg_digest	_hbuf,
			   *res;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	name = PG_GETARG_TEXT_P(0);

	res = find_digest(&_hbuf, name, 1);

	PG_FREE_IF_COPY(name, 0);

	if (res != NULL)
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

static pg_digest *
find_digest(pg_digest * hbuf, text *name, int silent)
{
	pg_digest  *p;
	char		buf[NAMEDATALEN];
	uint		len;

	len = VARSIZE(name) - VARHDRSZ;
	if (len >= NAMEDATALEN)
	{
		if (silent)
			return NULL;
		elog(ERROR, "Hash type does not exist (name too long)");
	}

	memcpy(buf, VARDATA(name), len);
	buf[len] = 0;

	p = pg_find_digest(hbuf, buf);

	if (p == NULL && !silent)
		elog(ERROR, "Hash type does not exist: '%s'", buf);
	return p;
}
