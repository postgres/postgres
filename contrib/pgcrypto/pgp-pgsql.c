/*
 * pgp-pgsql.c
 *		PostgreSQL wrappers for pgp.
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
 * $PostgreSQL: pgsql/contrib/pgcrypto/pgp-pgsql.c,v 1.8 2006/11/10 06:28:29 neilc Exp $
 */

#include "postgres.h"

#include "fmgr.h"
#include "parser/scansup.h"
#include "mb/pg_wchar.h"

#include "mbuf.h"
#include "px.h"
#include "pgp.h"

/*
 * public functions
 */
Datum		pgp_sym_encrypt_text(PG_FUNCTION_ARGS);
Datum		pgp_sym_encrypt_bytea(PG_FUNCTION_ARGS);
Datum		pgp_sym_decrypt_text(PG_FUNCTION_ARGS);
Datum		pgp_sym_decrypt_bytea(PG_FUNCTION_ARGS);

Datum		pgp_pub_encrypt_text(PG_FUNCTION_ARGS);
Datum		pgp_pub_encrypt_bytea(PG_FUNCTION_ARGS);
Datum		pgp_pub_decrypt_text(PG_FUNCTION_ARGS);
Datum		pgp_pub_decrypt_bytea(PG_FUNCTION_ARGS);

Datum		pgp_key_id_w(PG_FUNCTION_ARGS);

Datum		pg_armor(PG_FUNCTION_ARGS);
Datum		pg_dearmor(PG_FUNCTION_ARGS);

/* function headers */

PG_FUNCTION_INFO_V1(pgp_sym_encrypt_bytea);
PG_FUNCTION_INFO_V1(pgp_sym_encrypt_text);
PG_FUNCTION_INFO_V1(pgp_sym_decrypt_bytea);
PG_FUNCTION_INFO_V1(pgp_sym_decrypt_text);

PG_FUNCTION_INFO_V1(pgp_pub_encrypt_bytea);
PG_FUNCTION_INFO_V1(pgp_pub_encrypt_text);
PG_FUNCTION_INFO_V1(pgp_pub_decrypt_bytea);
PG_FUNCTION_INFO_V1(pgp_pub_decrypt_text);

PG_FUNCTION_INFO_V1(pgp_key_id_w);

PG_FUNCTION_INFO_V1(pg_armor);
PG_FUNCTION_INFO_V1(pg_dearmor);

/*
 * Mix a block of data into RNG.
 */
static void
add_block_entropy(PX_MD * md, text *data)
{
	uint8		sha1[20];

	px_md_reset(md);
	px_md_update(md, (uint8 *) VARDATA(data), VARSIZE(data) - VARHDRSZ);
	px_md_finish(md, sha1);

	px_add_entropy(sha1, 20);

	memset(sha1, 0, 20);
}

/*
 * Mix user data into RNG.	It is for user own interests to have
 * RNG state shuffled.
 */
static void
add_entropy(text *data1, text *data2, text *data3)
{
	PX_MD	   *md;
	uint8		rnd[3];

	if (!data1 && !data2 && !data3)
		return;

	if (px_get_random_bytes(rnd, 3) < 0)
		return;

	if (px_find_digest("sha1", &md) < 0)
		return;

	/*
	 * Try to make the feeding unpredictable.
	 *
	 * Prefer data over keys, as it's rather likely that key is same in
	 * several calls.
	 */

	/* chance: 7/8 */
	if (data1 && rnd[0] >= 32)
		add_block_entropy(md, data1);

	/* chance: 5/8 */
	if (data2 && rnd[1] >= 160)
		add_block_entropy(md, data2);

	/* chance: 5/8 */
	if (data3 && rnd[2] >= 160)
		add_block_entropy(md, data3);

	px_md_free(md);
	memset(rnd, 0, sizeof(rnd));
}

/*
 * returns src in case of no conversion or error
 */
static text *
convert_charset(text *src, int cset_from, int cset_to)
{
	int			src_len = VARSIZE(src) - VARHDRSZ;
	int			dst_len;
	unsigned char *dst;
	unsigned char *csrc = (unsigned char *) VARDATA(src);
	text	   *res;

	dst = pg_do_encoding_conversion(csrc, src_len, cset_from, cset_to);
	if (dst == csrc)
		return src;

	dst_len = strlen((char *) dst);
	res = palloc(dst_len + VARHDRSZ);
	memcpy(VARDATA(res), dst, dst_len);
	VARATT_SIZEP(res) = VARHDRSZ + dst_len;
	pfree(dst);
	return res;
}

static text *
convert_from_utf8(text *src)
{
	return convert_charset(src, PG_UTF8, GetDatabaseEncoding());
}

static text *
convert_to_utf8(text *src)
{
	return convert_charset(src, GetDatabaseEncoding(), PG_UTF8);
}

static void
clear_and_pfree(text *p)
{
	memset(p, 0, VARSIZE(p));
	pfree(p);
}

/*
 * expect-* arguments storage
 */
struct debug_expect
{
	int			debug;
	int			expect;
	int			cipher_algo;
	int			s2k_mode;
	int			s2k_cipher_algo;
	int			s2k_digest_algo;
	int			compress_algo;
	int			use_sess_key;
	int			disable_mdc;
	int			unicode_mode;
};

static void
fill_expect(struct debug_expect * ex, int text_mode)
{
	ex->debug = 0;
	ex->expect = 0;
	ex->cipher_algo = -1;
	ex->s2k_mode = -1;
	ex->s2k_cipher_algo = -1;
	ex->s2k_digest_algo = -1;
	ex->compress_algo = -1;
	ex->use_sess_key = -1;
	ex->disable_mdc = -1;
	ex->unicode_mode = -1;
}

#define EX_MSG(arg) \
	ereport(NOTICE, (errmsg( \
		"pgp_decrypt: unexpected %s: expected %d got %d", \
		CppAsString(arg), ex->arg, ctx->arg)))

#define EX_CHECK(arg) do { \
		if (ex->arg >= 0 && ex->arg != ctx->arg) EX_MSG(arg); \
	} while (0)

static void
check_expect(PGP_Context * ctx, struct debug_expect * ex)
{
	EX_CHECK(cipher_algo);
	EX_CHECK(s2k_mode);
	EX_CHECK(s2k_digest_algo);
	EX_CHECK(use_sess_key);
	if (ctx->use_sess_key)
		EX_CHECK(s2k_cipher_algo);
	EX_CHECK(disable_mdc);
	EX_CHECK(compress_algo);
	EX_CHECK(unicode_mode);
}

static void
show_debug(const char *msg)
{
	ereport(NOTICE, (errmsg("dbg: %s", msg)));
}

static int
set_arg(PGP_Context * ctx, char *key, char *val,
		struct debug_expect * ex)
{
	int			res = 0;

	if (strcmp(key, "cipher-algo") == 0)
		res = pgp_set_cipher_algo(ctx, val);
	else if (strcmp(key, "disable-mdc") == 0)
		res = pgp_disable_mdc(ctx, atoi(val));
	else if (strcmp(key, "sess-key") == 0)
		res = pgp_set_sess_key(ctx, atoi(val));
	else if (strcmp(key, "s2k-mode") == 0)
		res = pgp_set_s2k_mode(ctx, atoi(val));
	else if (strcmp(key, "s2k-digest-algo") == 0)
		res = pgp_set_s2k_digest_algo(ctx, val);
	else if (strcmp(key, "s2k-cipher-algo") == 0)
		res = pgp_set_s2k_cipher_algo(ctx, val);
	else if (strcmp(key, "compress-algo") == 0)
		res = pgp_set_compress_algo(ctx, atoi(val));
	else if (strcmp(key, "compress-level") == 0)
		res = pgp_set_compress_level(ctx, atoi(val));
	else if (strcmp(key, "convert-crlf") == 0)
		res = pgp_set_convert_crlf(ctx, atoi(val));
	else if (strcmp(key, "unicode-mode") == 0)
		res = pgp_set_unicode_mode(ctx, atoi(val));
	/* decrypt debug */
	else if (ex != NULL && strcmp(key, "debug") == 0)
		ex->debug = atoi(val);
	else if (ex != NULL && strcmp(key, "expect-cipher-algo") == 0)
	{
		ex->expect = 1;
		ex->cipher_algo = pgp_get_cipher_code(val);
	}
	else if (ex != NULL && strcmp(key, "expect-disable-mdc") == 0)
	{
		ex->expect = 1;
		ex->disable_mdc = atoi(val);
	}
	else if (ex != NULL && strcmp(key, "expect-sess-key") == 0)
	{
		ex->expect = 1;
		ex->use_sess_key = atoi(val);
	}
	else if (ex != NULL && strcmp(key, "expect-s2k-mode") == 0)
	{
		ex->expect = 1;
		ex->s2k_mode = atoi(val);
	}
	else if (ex != NULL && strcmp(key, "expect-s2k-digest-algo") == 0)
	{
		ex->expect = 1;
		ex->s2k_digest_algo = pgp_get_digest_code(val);
	}
	else if (ex != NULL && strcmp(key, "expect-s2k-cipher-algo") == 0)
	{
		ex->expect = 1;
		ex->s2k_cipher_algo = pgp_get_cipher_code(val);
	}
	else if (ex != NULL && strcmp(key, "expect-compress-algo") == 0)
	{
		ex->expect = 1;
		ex->compress_algo = atoi(val);
	}
	else if (ex != NULL && strcmp(key, "expect-unicode-mode") == 0)
	{
		ex->expect = 1;
		ex->unicode_mode = atoi(val);
	}
	else
		res = PXE_ARGUMENT_ERROR;

	return res;
}

/*
 * Find next word.	Handle ',' and '=' as words.  Skip whitespace.
 * Put word info into res_p, res_len.
 * Returns ptr to next word.
 */
static char *
getword(char *p, char **res_p, int *res_len)
{
	/* whitespace at start */
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
		p++;

	/* word data */
	*res_p = p;
	if (*p == '=' || *p == ',')
		p++;
	else
		while (*p && !(*p == ' ' || *p == '\t' || *p == '\n'
					   || *p == '=' || *p == ','))
			p++;

	/* word end */
	*res_len = p - *res_p;

	/* whitespace at end */
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
		p++;

	return p;
}

/*
 * Convert to lowercase asciiz string.
 */
static char *
downcase_convert(const uint8 *s, int len)
{
	int			c,
				i;
	char	   *res = palloc(len + 1);

	for (i = 0; i < len; i++)
	{
		c = s[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		res[i] = c;
	}
	res[len] = 0;
	return res;
}

static int
parse_args(PGP_Context * ctx, uint8 *args, int arg_len,
		   struct debug_expect * ex)
{
	char	   *str = downcase_convert(args, arg_len);
	char	   *key,
			   *val;
	int			key_len,
				val_len;
	int			res = 0;
	char	   *p = str;

	while (*p)
	{
		res = PXE_ARGUMENT_ERROR;
		p = getword(p, &key, &key_len);
		if (*p++ != '=')
			break;
		p = getword(p, &val, &val_len);
		if (*p == '\0')
			;
		else if (*p++ != ',')
			break;

		if (*key == 0 || *val == 0 || val_len == 0)
			break;

		key[key_len] = 0;
		val[val_len] = 0;

		res = set_arg(ctx, key, val, ex);
		if (res < 0)
			break;
	}
	pfree(str);
	return res;
}

static MBuf *
create_mbuf_from_vardata(text *data)
{
	return mbuf_create_from_data((uint8 *) VARDATA(data),
								 VARSIZE(data) - VARHDRSZ);
}

static void
init_work(PGP_Context ** ctx_p, int is_text,
		  text *args, struct debug_expect * ex)
{
	int			err = pgp_init(ctx_p);

	fill_expect(ex, is_text);

	if (err == 0 && args != NULL)
		err = parse_args(*ctx_p, (uint8 *) VARDATA(args),
						 VARSIZE(args) - VARHDRSZ, ex);

	if (err)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("%s", px_strerror(err))));
	}

	if (ex->debug)
		px_set_debug_handler(show_debug);

	pgp_set_text_mode(*ctx_p, is_text);
}

static bytea *
encrypt_internal(int is_pubenc, int is_text,
				 text *data, text *key, text *args)
{
	MBuf	   *src,
			   *dst;
	uint8		tmp[VARHDRSZ];
	uint8	   *restmp;
	bytea	   *res;
	int			res_len;
	PGP_Context *ctx;
	int			err;
	struct debug_expect ex;
	text	   *tmp_data = NULL;

	/*
	 * Add data and key info RNG.
	 */
	add_entropy(data, key, NULL);

	init_work(&ctx, is_text, args, &ex);

	if (is_text && pgp_get_unicode_mode(ctx))
	{
		tmp_data = convert_to_utf8(data);
		if (tmp_data == data)
			tmp_data = NULL;
		else
			data = tmp_data;
	}

	src = create_mbuf_from_vardata(data);
	dst = mbuf_create(VARSIZE(data) + 128);

	/*
	 * reserve room for header
	 */
	mbuf_append(dst, tmp, VARHDRSZ);

	/*
	 * set key
	 */
	if (is_pubenc)
	{
		MBuf	   *kbuf = create_mbuf_from_vardata(key);

		err = pgp_set_pubkey(ctx, kbuf,
							 NULL, 0, 0);
		mbuf_free(kbuf);
	}
	else
		err = pgp_set_symkey(ctx, (uint8 *) VARDATA(key),
							 VARSIZE(key) - VARHDRSZ);

	/*
	 * encrypt
	 */
	if (err >= 0)
		err = pgp_encrypt(ctx, src, dst);

	/*
	 * check for error
	 */
	if (err)
	{
		if (ex.debug)
			px_set_debug_handler(NULL);
		if (tmp_data)
			clear_and_pfree(tmp_data);
		pgp_free(ctx);
		mbuf_free(src);
		mbuf_free(dst);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("%s", px_strerror(err))));
	}

	/* res_len includes VARHDRSZ */
	res_len = mbuf_steal_data(dst, &restmp);
	res = (bytea *) restmp;
	VARATT_SIZEP(res) = res_len;

	if (tmp_data)
		clear_and_pfree(tmp_data);
	pgp_free(ctx);
	mbuf_free(src);
	mbuf_free(dst);

	px_set_debug_handler(NULL);

	return res;
}

static bytea *
decrypt_internal(int is_pubenc, int need_text, text *data,
				 text *key, text *keypsw, text *args)
{
	int			err;
	MBuf	   *src = NULL,
			   *dst = NULL;
	uint8		tmp[VARHDRSZ];
	uint8	   *restmp;
	bytea	   *res;
	int			res_len;
	PGP_Context *ctx = NULL;
	struct debug_expect ex;
	int			got_unicode = 0;


	init_work(&ctx, need_text, args, &ex);

	src = mbuf_create_from_data((uint8 *) VARDATA(data),
								VARSIZE(data) - VARHDRSZ);
	dst = mbuf_create(VARSIZE(data) + 2048);

	/*
	 * reserve room for header
	 */
	mbuf_append(dst, tmp, VARHDRSZ);

	/*
	 * set key
	 */
	if (is_pubenc)
	{
		uint8	   *psw = NULL;
		int			psw_len = 0;
		MBuf	   *kbuf;

		if (keypsw)
		{
			psw = (uint8 *) VARDATA(keypsw);
			psw_len = VARSIZE(keypsw) - VARHDRSZ;
		}
		kbuf = create_mbuf_from_vardata(key);
		err = pgp_set_pubkey(ctx, kbuf, psw, psw_len, 1);
		mbuf_free(kbuf);
	}
	else
		err = pgp_set_symkey(ctx, (uint8 *) VARDATA(key),
							 VARSIZE(key) - VARHDRSZ);

	/*
	 * decrypt
	 */
	if (err >= 0)
		err = pgp_decrypt(ctx, src, dst);

	/*
	 * failed?
	 */
	if (err < 0)
		goto out;

	if (ex.expect)
		check_expect(ctx, &ex);

	/* remember the setting */
	got_unicode = pgp_get_unicode_mode(ctx);

out:
	if (src)
		mbuf_free(src);
	if (ctx)
		pgp_free(ctx);

	if (err)
	{
		px_set_debug_handler(NULL);
		if (dst)
			mbuf_free(dst);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("%s", px_strerror(err))));
	}

	res_len = mbuf_steal_data(dst, &restmp);
	mbuf_free(dst);

	/* res_len includes VARHDRSZ */
	res = (bytea *) restmp;
	VARATT_SIZEP(res) = res_len;

	if (need_text && got_unicode)
	{
		text	   *utf = convert_from_utf8(res);

		if (utf != res)
		{
			clear_and_pfree(res);
			res = utf;
		}
	}
	px_set_debug_handler(NULL);

	/*
	 * add successfull decryptions also into RNG
	 */
	add_entropy(res, key, keypsw);

	return res;
}

/*
 * Wrappers for symmetric-key functions
 */
Datum
pgp_sym_encrypt_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_P(2);

	res = encrypt_internal(0, 0, data, key, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(arg, 2);
	PG_RETURN_TEXT_P(res);
}

Datum
pgp_sym_encrypt_text(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_P(2);

	res = encrypt_internal(0, 1, data, key, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(arg, 2);
	PG_RETURN_TEXT_P(res);
}


Datum
pgp_sym_decrypt_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_P(2);

	res = decrypt_internal(0, 0, data, key, NULL, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(arg, 2);
	PG_RETURN_TEXT_P(res);
}

Datum
pgp_sym_decrypt_text(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_P(2);

	res = decrypt_internal(0, 1, data, key, NULL, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(arg, 2);
	PG_RETURN_TEXT_P(res);
}

/*
 * Wrappers for public-key functions
 */

Datum
pgp_pub_encrypt_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_P(2);

	res = encrypt_internal(1, 0, data, key, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(arg, 2);
	PG_RETURN_TEXT_P(res);
}

Datum
pgp_pub_encrypt_text(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_P(2);

	res = encrypt_internal(1, 1, data, key, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(arg, 2);
	PG_RETURN_TEXT_P(res);
}


Datum
pgp_pub_decrypt_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *psw = NULL,
			   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		psw = PG_GETARG_BYTEA_P(2);
	if (PG_NARGS() > 3)
		arg = PG_GETARG_BYTEA_P(3);

	res = decrypt_internal(1, 0, data, key, psw, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(psw, 2);
	if (PG_NARGS() > 3)
		PG_FREE_IF_COPY(arg, 3);
	PG_RETURN_TEXT_P(res);
}

Datum
pgp_pub_decrypt_text(PG_FUNCTION_ARGS)
{
	bytea	   *data,
			   *key;
	text	   *psw = NULL,
			   *arg = NULL;
	text	   *res;

	data = PG_GETARG_BYTEA_P(0);
	key = PG_GETARG_BYTEA_P(1);
	if (PG_NARGS() > 2)
		psw = PG_GETARG_BYTEA_P(2);
	if (PG_NARGS() > 3)
		arg = PG_GETARG_BYTEA_P(3);

	res = decrypt_internal(1, 1, data, key, psw, arg);

	PG_FREE_IF_COPY(data, 0);
	PG_FREE_IF_COPY(key, 1);
	if (PG_NARGS() > 2)
		PG_FREE_IF_COPY(psw, 2);
	if (PG_NARGS() > 3)
		PG_FREE_IF_COPY(arg, 3);
	PG_RETURN_TEXT_P(res);
}


/*
 * Wrappers for PGP ascii armor
 */

Datum
pg_armor(PG_FUNCTION_ARGS)
{
	bytea	   *data;
	text	   *res;
	int			data_len,
				res_len,
				guess_len;

	data = PG_GETARG_BYTEA_P(0);
	data_len = VARSIZE(data) - VARHDRSZ;

	guess_len = pgp_armor_enc_len(data_len);
	res = palloc(VARHDRSZ + guess_len);

	res_len = pgp_armor_encode((uint8 *) VARDATA(data), data_len,
							   (uint8 *) VARDATA(res));
	if (res_len > guess_len)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("Overflow - encode estimate too small")));
	VARATT_SIZEP(res) = VARHDRSZ + res_len;

	PG_FREE_IF_COPY(data, 0);
	PG_RETURN_TEXT_P(res);
}

Datum
pg_dearmor(PG_FUNCTION_ARGS)
{
	text	   *data;
	bytea	   *res;
	int			data_len,
				res_len,
				guess_len;

	data = PG_GETARG_TEXT_P(0);
	data_len = VARSIZE(data) - VARHDRSZ;

	guess_len = pgp_armor_dec_len(data_len);
	res = palloc(VARHDRSZ + guess_len);

	res_len = pgp_armor_decode((uint8 *) VARDATA(data), data_len,
							   (uint8 *) VARDATA(res));
	if (res_len < 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("%s", px_strerror(res_len))));
	if (res_len > guess_len)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("Overflow - decode estimate too small")));
	VARATT_SIZEP(res) = VARHDRSZ + res_len;

	PG_FREE_IF_COPY(data, 0);
	PG_RETURN_TEXT_P(res);
}

/*
 * Wrappers for PGP key id
 */

Datum
pgp_key_id_w(PG_FUNCTION_ARGS)
{
	bytea	   *data;
	text	   *res;
	int			res_len;
	MBuf	   *buf;

	data = PG_GETARG_BYTEA_P(0);
	buf = create_mbuf_from_vardata(data);
	res = palloc(VARHDRSZ + 17);

	res_len = pgp_get_keyid(buf, VARDATA(res));
	mbuf_free(buf);
	if (res_len < 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("%s", px_strerror(res_len))));
	VARATT_SIZEP(res) = VARHDRSZ + res_len;

	PG_FREE_IF_COPY(data, 0);
	PG_RETURN_TEXT_P(res);
}
