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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/pgp-pgsql.c
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "mbuf.h"
#include "pgp.h"
#include "px.h"
#include "utils/array.h"
#include "utils/builtins.h"

/*
 * public functions
 */
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
PG_FUNCTION_INFO_V1(pgp_armor_headers);

/*
 * returns src in case of no conversion or error
 */
static text *
convert_charset(text *src, int cset_from, int cset_to)
{
	int			src_len = VARSIZE_ANY_EXHDR(src);
	unsigned char *dst;
	unsigned char *csrc = (unsigned char *) VARDATA_ANY(src);
	text	   *res;

	dst = pg_do_encoding_conversion(csrc, src_len, cset_from, cset_to);
	if (dst == csrc)
		return src;

	res = cstring_to_text((char *) dst);
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

static bool
string_is_ascii(const char *str)
{
	const char *p;

	for (p = str; *p; p++)
	{
		if (IS_HIGHBIT_SET(*p))
			return false;
	}
	return true;
}

static void
clear_and_pfree(text *p)
{
	px_memset(p, 0, VARSIZE_ANY(p));
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
	int			s2k_count;
	int			s2k_cipher_algo;
	int			s2k_digest_algo;
	int			compress_algo;
	int			use_sess_key;
	int			disable_mdc;
	int			unicode_mode;
};

static void
fill_expect(struct debug_expect *ex, int text_mode)
{
	ex->debug = 0;
	ex->expect = 0;
	ex->cipher_algo = -1;
	ex->s2k_mode = -1;
	ex->s2k_count = -1;
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
check_expect(PGP_Context *ctx, struct debug_expect *ex)
{
	EX_CHECK(cipher_algo);
	EX_CHECK(s2k_mode);
	EX_CHECK(s2k_count);
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
set_arg(PGP_Context *ctx, char *key, char *val,
		struct debug_expect *ex)
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
	else if (strcmp(key, "s2k-count") == 0)
		res = pgp_set_s2k_count(ctx, atoi(val));
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

	/*
	 * The remaining options are for debugging/testing and are therefore not
	 * documented in the user-facing docs.
	 */
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
	else if (ex != NULL && strcmp(key, "expect-s2k-count") == 0)
	{
		ex->expect = 1;
		ex->s2k_count = atoi(val);
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
 * Find next word.  Handle ',' and '=' as words.  Skip whitespace.
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
parse_args(PGP_Context *ctx, uint8 *args, int arg_len,
		   struct debug_expect *ex)
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
	return mbuf_create_from_data((uint8 *) VARDATA_ANY(data),
								 VARSIZE_ANY_EXHDR(data));
}

static void
init_work(PGP_Context **ctx_p, int is_text,
		  text *args, struct debug_expect *ex)
{
	int			err = pgp_init(ctx_p);

	fill_expect(ex, is_text);

	if (err == 0 && args != NULL)
		err = parse_args(*ctx_p, (uint8 *) VARDATA_ANY(args),
						 VARSIZE_ANY_EXHDR(args), ex);

	if (err)
		px_THROW_ERROR(err);

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
	dst = mbuf_create(VARSIZE_ANY(data) + 128);

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
		err = pgp_set_symkey(ctx, (uint8 *) VARDATA_ANY(key),
							 VARSIZE_ANY_EXHDR(key));

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
		px_THROW_ERROR(err);
	}

	/* res_len includes VARHDRSZ */
	res_len = mbuf_steal_data(dst, &restmp);
	res = (bytea *) restmp;
	SET_VARSIZE(res, res_len);

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

	src = mbuf_create_from_data((uint8 *) VARDATA_ANY(data),
								VARSIZE_ANY_EXHDR(data));
	dst = mbuf_create(VARSIZE_ANY(data) + 2048);

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
			psw = (uint8 *) VARDATA_ANY(keypsw);
			psw_len = VARSIZE_ANY_EXHDR(keypsw);
		}
		kbuf = create_mbuf_from_vardata(key);
		err = pgp_set_pubkey(ctx, kbuf, psw, psw_len, 1);
		mbuf_free(kbuf);
	}
	else
		err = pgp_set_symkey(ctx, (uint8 *) VARDATA_ANY(key),
							 VARSIZE_ANY_EXHDR(key));

	/* decrypt */
	if (err >= 0)
	{
		err = pgp_decrypt(ctx, src, dst);

		if (ex.expect)
			check_expect(ctx, &ex);

		/* remember the setting */
		got_unicode = pgp_get_unicode_mode(ctx);
	}

	mbuf_free(src);
	pgp_free(ctx);

	if (err)
	{
		px_set_debug_handler(NULL);
		mbuf_free(dst);
		px_THROW_ERROR(err);
	}

	res_len = mbuf_steal_data(dst, &restmp);
	mbuf_free(dst);

	/* res_len includes VARHDRSZ */
	res = (bytea *) restmp;
	SET_VARSIZE(res, res_len);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_PP(2);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_PP(2);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_PP(2);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_PP(2);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_PP(2);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		arg = PG_GETARG_BYTEA_PP(2);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		psw = PG_GETARG_BYTEA_PP(2);
	if (PG_NARGS() > 3)
		arg = PG_GETARG_BYTEA_PP(3);

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

	data = PG_GETARG_BYTEA_PP(0);
	key = PG_GETARG_BYTEA_PP(1);
	if (PG_NARGS() > 2)
		psw = PG_GETARG_BYTEA_PP(2);
	if (PG_NARGS() > 3)
		arg = PG_GETARG_BYTEA_PP(3);

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

/*
 * Helper function for pg_armor. Converts arrays of keys and values into
 * plain C arrays, and checks that they don't contain invalid characters.
 */
static int
parse_key_value_arrays(ArrayType *key_array, ArrayType *val_array,
					   char ***p_keys, char ***p_values)
{
	int			nkdims = ARR_NDIM(key_array);
	int			nvdims = ARR_NDIM(val_array);
	char	  **keys,
			  **values;
	Datum	   *key_datums,
			   *val_datums;
	bool	   *key_nulls,
			   *val_nulls;
	int			key_count,
				val_count;
	int			i;

	if (nkdims > 1 || nkdims != nvdims)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));
	if (nkdims == 0)
		return 0;

	deconstruct_array(key_array,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &key_datums, &key_nulls, &key_count);

	deconstruct_array(val_array,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &val_datums, &val_nulls, &val_count);

	if (key_count != val_count)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("mismatched array dimensions")));

	keys = (char **) palloc(sizeof(char *) * key_count);
	values = (char **) palloc(sizeof(char *) * val_count);

	for (i = 0; i < key_count; i++)
	{
		char	   *v;

		/* Check that the key doesn't contain anything funny */
		if (key_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for header key")));

		v = TextDatumGetCString(key_datums[i]);

		if (!string_is_ascii(v))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("header key must not contain non-ASCII characters")));
		if (strstr(v, ": "))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("header key must not contain \": \"")));
		if (strchr(v, '\n'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("header key must not contain newlines")));
		keys[i] = v;

		/* And the same for the value */
		if (val_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for header value")));

		v = TextDatumGetCString(val_datums[i]);

		if (!string_is_ascii(v))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("header value must not contain non-ASCII characters")));
		if (strchr(v, '\n'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("header value must not contain newlines")));

		values[i] = v;
	}

	*p_keys = keys;
	*p_values = values;
	return key_count;
}

Datum
pg_armor(PG_FUNCTION_ARGS)
{
	bytea	   *data;
	text	   *res;
	int			data_len;
	StringInfoData buf;
	int			num_headers;
	char	  **keys = NULL,
			  **values = NULL;

	data = PG_GETARG_BYTEA_PP(0);
	data_len = VARSIZE_ANY_EXHDR(data);
	if (PG_NARGS() == 3)
	{
		num_headers = parse_key_value_arrays(PG_GETARG_ARRAYTYPE_P(1),
											 PG_GETARG_ARRAYTYPE_P(2),
											 &keys, &values);
	}
	else if (PG_NARGS() == 1)
		num_headers = 0;
	else
		elog(ERROR, "unexpected number of arguments %d", PG_NARGS());

	initStringInfo(&buf);

	pgp_armor_encode((uint8 *) VARDATA_ANY(data), data_len, &buf,
					 num_headers, keys, values);

	res = palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(res, VARHDRSZ + buf.len);
	memcpy(VARDATA(res), buf.data, buf.len);
	pfree(buf.data);

	PG_FREE_IF_COPY(data, 0);
	PG_RETURN_TEXT_P(res);
}

Datum
pg_dearmor(PG_FUNCTION_ARGS)
{
	text	   *data;
	bytea	   *res;
	int			data_len;
	int			ret;
	StringInfoData buf;

	data = PG_GETARG_TEXT_PP(0);
	data_len = VARSIZE_ANY_EXHDR(data);

	initStringInfo(&buf);

	ret = pgp_armor_decode((uint8 *) VARDATA_ANY(data), data_len, &buf);
	if (ret < 0)
		px_THROW_ERROR(ret);
	res = palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(res, VARHDRSZ + buf.len);
	memcpy(VARDATA(res), buf.data, buf.len);
	pfree(buf.data);

	PG_FREE_IF_COPY(data, 0);
	PG_RETURN_TEXT_P(res);
}

/* cross-call state for pgp_armor_headers */
typedef struct
{
	int			nheaders;
	char	  **keys;
	char	  **values;
} pgp_armor_headers_state;

Datum
pgp_armor_headers(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	pgp_armor_headers_state *state;
	char	   *utf8key;
	char	   *utf8val;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *data = PG_GETARG_TEXT_PP(0);
		int			res;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();

		/* we need the state allocated in the multi call context */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		state = (pgp_armor_headers_state *) palloc(sizeof(pgp_armor_headers_state));

		res = pgp_extract_armor_headers((uint8 *) VARDATA_ANY(data),
										VARSIZE_ANY_EXHDR(data),
										&state->nheaders, &state->keys,
										&state->values);
		if (res < 0)
			px_THROW_ERROR(res);

		MemoryContextSwitchTo(oldcontext);
		funcctx->user_fctx = state;
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (pgp_armor_headers_state *) funcctx->user_fctx;

	if (funcctx->call_cntr >= state->nheaders)
		SRF_RETURN_DONE(funcctx);
	else
	{
		char	   *values[2];

		/* we assume that the keys (and values) are in UTF-8. */
		utf8key = state->keys[funcctx->call_cntr];
		utf8val = state->values[funcctx->call_cntr];

		values[0] = pg_any_to_server(utf8key, strlen(utf8key), PG_UTF8);
		values[1] = pg_any_to_server(utf8val, strlen(utf8val), PG_UTF8);

		/* build a tuple */
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
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

	data = PG_GETARG_BYTEA_PP(0);
	buf = create_mbuf_from_vardata(data);
	res = palloc(VARHDRSZ + 17);

	res_len = pgp_get_keyid(buf, VARDATA(res));
	mbuf_free(buf);
	if (res_len < 0)
		px_THROW_ERROR(res_len);
	SET_VARSIZE(res, VARHDRSZ + res_len);

	PG_FREE_IF_COPY(data, 0);
	PG_RETURN_TEXT_P(res);
}
