/*-------------------------------------------------------------------------
 *
 * varchar.c
 *	  Functions for the built-in types char(n) and varchar(n).
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/varchar.c,v 1.79 2001/06/01 17:49:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif


/*
 * CHAR() and VARCHAR() types are part of the ANSI SQL standard. CHAR()
 * is for blank-padded string whose length is specified in CREATE TABLE.
 * VARCHAR is for storing string whose length is at most the length specified
 * at CREATE TABLE time.
 *
 * It's hard to implement these types because we cannot figure out
 * the length of the type from the type itself. I change (hopefully all) the
 * fmgr calls that invoke input functions of a data type to supply the
 * length also. (eg. in INSERTs, we have the tupleDescriptor which contains
 * the length of the attributes and hence the exact length of the char() or
 * varchar(). We pass this to bpcharin() or varcharin().) In the case where
 * we cannot determine the length, we pass in -1 instead and the input string
 * must be null-terminated.
 *
 * We actually implement this as a varlena so that we don't have to pass in
 * the length for the comparison functions. (The difference between these
 * types and "text" is that we truncate and possibly blank-pad the string
 * at insertion time.)
 *
 *															  - ay 6/95
 */


/*****************************************************************************
 *	 bpchar - char()														 *
 *****************************************************************************/

/*
 * Convert a C string to CHARACTER internal representation.  atttypmod
 * is the declared length of the type plus VARHDRSZ.
 *
 * If the C string is too long, raise an error, unless the extra
 * characters are spaces, in which case they're truncated.  (per SQL)
 */
Datum
bpcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	BpChar	   *result;
	char	   *r;
	size_t		len, maxlen;
	int			i;

	len = strlen(s);

	/* If typmod is -1 (or invalid), use the actual string length */
	if (atttypmod < (int32) VARHDRSZ)
		maxlen = len;
	else
		maxlen = atttypmod - VARHDRSZ;

	if (len > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
#ifdef MULTIBYTE
		size_t mbmaxlen = pg_mbcliplen(s, len, maxlen);

		if (strspn(s + mbmaxlen, " ") == len - mbmaxlen)
			len = mbmaxlen;
		else
			elog(ERROR, "value too long for type character(%d)", maxlen);
		Assert(len <= maxlen);
#else
		if (strspn(s + maxlen, " ") == len - maxlen)
			len = maxlen;
		else
			elog(ERROR, "value too long for type character(%d)", maxlen);
#endif
	}

	result = palloc(maxlen + VARHDRSZ);
	VARATT_SIZEP(result) = maxlen + VARHDRSZ;
	r = VARDATA(result);
	for (i = 0; i < len; i++)
		*r++ = *s++;

	/* blank pad the string if necessary */
	for (; i < maxlen; i++)
		*r++ = ' ';

#ifdef CYR_RECODE
	convertstr(VARDATA(result), len, 0);
#endif

	PG_RETURN_BPCHAR_P(result);
}


/*
 * Convert a CHARACTER value to a C string.
 */
Datum
bpcharout(PG_FUNCTION_ARGS)
{
	BpChar	   *s = PG_GETARG_BPCHAR_P(0);
	char	   *result;
	int			len;

	/* copy and add null term */
	len = VARSIZE(s) - VARHDRSZ;
	result = (char *) palloc(len + 1);
	memcpy(result, VARDATA(s), len);
	result[len] = '\0';

#ifdef CYR_RECODE
	convertstr(result, len, 1);
#endif

	PG_RETURN_CSTRING(result);
}


/*
 * Converts a CHARACTER type to the specified size.  maxlen is the new
 * declared length plus VARHDRSZ bytes.  Truncation
 * rules see bpcharin() above.
 */
Datum
bpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *source = PG_GETARG_BPCHAR_P(0);
	int32		maxlen = PG_GETARG_INT32(1);
	BpChar	   *result;
	int32		len;
	char	   *r;
	char	   *s;
	int			i;

	len = VARSIZE(source);
	/* No work if typmod is invalid or supplied data matches it already */
	if (maxlen < (int32) VARHDRSZ || len == maxlen)
		PG_RETURN_BPCHAR_P(source);

	if (len > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
#ifdef MULTIBYTE
		size_t		maxmblen;

		maxmblen = pg_mbcliplen(VARDATA(source), len - VARHDRSZ,
								maxlen - VARHDRSZ) + VARHDRSZ;

		for (i = maxmblen - VARHDRSZ; i < len - VARHDRSZ; i++)
			if (*(VARDATA(source) + i) != ' ')
				elog(ERROR, "value too long for type character(%d)",
					 maxlen - VARHDRSZ);

		len = maxmblen;
		Assert(len <= maxlen);
#else
		for (i = maxlen - VARHDRSZ; i < len - VARHDRSZ; i++)
			if (*(VARDATA(source) + i) != ' ')
				elog(ERROR, "value too long for type character(%d)",
					 maxlen - VARHDRSZ);

		len = maxlen;
#endif
	}

	s = VARDATA(source);

	result = palloc(maxlen);
	VARATT_SIZEP(result) = maxlen;
	r = VARDATA(result);

	for (i = 0; i < len - VARHDRSZ; i++)
		*r++ = *s++;

	/* blank pad the string if necessary */
	for (; i < maxlen - VARHDRSZ; i++)
		*r++ = ' ';

	PG_RETURN_BPCHAR_P(result);
}


/* _bpchar()
 * Converts an array of char() elements to a specific internal length.
 * len is the length specified in () plus VARHDRSZ bytes.
 */
Datum
_bpchar(PG_FUNCTION_ARGS)
{
	ArrayType  *v = (ArrayType *) PG_GETARG_VARLENA_P(0);
	int32		len = PG_GETARG_INT32(1);
	FunctionCallInfoData locfcinfo;

	/*
	 * Since bpchar() is a built-in function, we should only need to look
	 * it up once per run.
	 */
	static FmgrInfo bpchar_finfo;

	if (bpchar_finfo.fn_oid == InvalidOid)
		fmgr_info(F_BPCHAR, &bpchar_finfo);

	MemSet(&locfcinfo, 0, sizeof(locfcinfo));
	locfcinfo.flinfo = &bpchar_finfo;
	locfcinfo.nargs = 2;
	/* We assume we are "strict" and need not worry about null inputs */
	locfcinfo.arg[0] = PointerGetDatum(v);
	locfcinfo.arg[1] = Int32GetDatum(len);

	return array_map(&locfcinfo, BPCHAROID, BPCHAROID);
}


/* bpchar_char()
 * Convert bpchar(1) to char.
 *
 * If input is multiple chars, only the first is returned.
 */
Datum
bpchar_char(PG_FUNCTION_ARGS)
{
	BpChar	   *s = PG_GETARG_BPCHAR_P(0);

	PG_RETURN_CHAR(*VARDATA(s));
}

/* char_bpchar()
 * Convert char to bpchar(1).
 */
Datum
char_bpchar(PG_FUNCTION_ARGS)
{
	char		c = PG_GETARG_CHAR(0);
	BpChar	   *result;

	result = (BpChar *) palloc(VARHDRSZ + 1);

	VARATT_SIZEP(result) = VARHDRSZ + 1;
	*(VARDATA(result)) = c;

	PG_RETURN_BPCHAR_P(result);
}


/* bpchar_name()
 * Converts a bpchar() type to a NameData type.
 */
Datum
bpchar_name(PG_FUNCTION_ARGS)
{
	BpChar	   *s = PG_GETARG_BPCHAR_P(0);
	Name		result;
	int			len;

	len = VARSIZE(s) - VARHDRSZ;

	/* Truncate to max length for a Name */
	if (len >= NAMEDATALEN)
		len = NAMEDATALEN - 1;

	/* Remove trailing blanks */
	while (len > 0)
	{
		if (*(VARDATA(s) + len - 1) != ' ')
			break;
		len--;
	}

	result = (NameData *) palloc(NAMEDATALEN);
	memcpy(NameStr(*result), VARDATA(s), len);

	/* Now null pad to full length... */
	while (len < NAMEDATALEN)
	{
		*(NameStr(*result) + len) = '\0';
		len++;
	}

	PG_RETURN_NAME(result);
}

/* name_bpchar()
 * Converts a NameData type to a bpchar type.
 */
Datum
name_bpchar(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);
	BpChar	   *result;
	int			len;

	len = strlen(NameStr(*s));
	result = (BpChar *) palloc(VARHDRSZ + len);
	memcpy(VARDATA(result), NameStr(*s), len);
	VARATT_SIZEP(result) = len + VARHDRSZ;

	PG_RETURN_BPCHAR_P(result);
}


/*****************************************************************************
 *	 varchar - varchar()													 *
 *****************************************************************************/

/*
 * Convert a C string to VARCHAR internal representation.  atttypmod
 * is the declared length of the type plus VARHDRSZ.
 *
 * If the C string is too long, raise an error, unless the extra
 * characters are spaces, in which case they're truncated.  (per SQL)
 */
Datum
varcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarChar    *result;
	size_t		len, maxlen;

	len = strlen(s);
	maxlen = atttypmod - VARHDRSZ;

	if (atttypmod >= (int32) VARHDRSZ && len > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
#ifdef MULTIBYTE
		size_t mbmaxlen = pg_mbcliplen(s, len, maxlen);

		if (strspn(s + mbmaxlen, " ") == len - mbmaxlen)
			len = mbmaxlen;
#else
		if (strspn(s + maxlen, " ") == len - maxlen)
			len = maxlen;
#endif
		else
			elog(ERROR, "value too long for type character varying(%d)", maxlen);
	}

	result = palloc(len + VARHDRSZ);
	VARATT_SIZEP(result) = len + VARHDRSZ;
	memcpy(VARDATA(result), s, len);

#ifdef CYR_RECODE
	convertstr(VARDATA(result), len, 0);
#endif

	PG_RETURN_VARCHAR_P(result);
}


/*
 * Convert a VARCHAR value to a C string.
 */
Datum
varcharout(PG_FUNCTION_ARGS)
{
	VarChar    *s = PG_GETARG_VARCHAR_P(0);
	char	   *result;
	int32		len;

	/* copy and add null term */
	len = VARSIZE(s) - VARHDRSZ;
	result = palloc(len + 1);
	memcpy(result, VARDATA(s), len);
	result[len] = '\0';

#ifdef CYR_RECODE
	convertstr(result, len, 1);
#endif

	PG_RETURN_CSTRING(result);
}


/*
 * Converts a VARCHAR type to the specified size.  maxlen is the new
 * declared length plus VARHDRSZ bytes.  Truncation
 * rules see varcharin() above.
 */
Datum
varchar(PG_FUNCTION_ARGS)
{
	VarChar    *source = PG_GETARG_VARCHAR_P(0);
	int32		maxlen = PG_GETARG_INT32(1);
	VarChar    *result;
	int32		len;
	int			i;

	len = VARSIZE(source);
	/* No work if typmod is invalid or supplied data fits it already */
	if (maxlen < (int32) VARHDRSZ || len <= maxlen)
		PG_RETURN_VARCHAR_P(source);

	/* only reach here if string is too long... */

#ifdef MULTIBYTE
	{
		size_t		maxmblen;

		/* truncate multi-byte string preserving multi-byte boundary */
		maxmblen = pg_mbcliplen(VARDATA(source), len - VARHDRSZ,
								maxlen - VARHDRSZ) + VARHDRSZ;

		for (i = maxmblen - VARHDRSZ; i < len - VARHDRSZ; i++)
			if (*(VARDATA(source) + i) != ' ')
				elog(ERROR, "value too long for type character varying(%d)",
					 maxlen - VARHDRSZ);

		len = maxmblen;
	}
#else
	for (i = maxlen - VARHDRSZ; i < len - VARHDRSZ; i++)
		if (*(VARDATA(source) + i) != ' ')
			elog(ERROR, "value too long for type character varying(%d)",
				 maxlen - VARHDRSZ);

	/* clip extra spaces */
	len = maxlen;
#endif

	result = palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), VARDATA(source), len - VARHDRSZ);

	PG_RETURN_VARCHAR_P(result);
}


/* _varchar()
 * Converts an array of varchar() elements to the specified size.
 * len is the length specified in () plus VARHDRSZ bytes.
 */
Datum
_varchar(PG_FUNCTION_ARGS)
{
	ArrayType  *v = (ArrayType *) PG_GETARG_VARLENA_P(0);
	int32		len = PG_GETARG_INT32(1);
	FunctionCallInfoData locfcinfo;

	/*
	 * Since varchar() is a built-in function, we should only need to look
	 * it up once per run.
	 */
	static FmgrInfo varchar_finfo;

	if (varchar_finfo.fn_oid == InvalidOid)
		fmgr_info(F_VARCHAR, &varchar_finfo);

	MemSet(&locfcinfo, 0, sizeof(locfcinfo));
	locfcinfo.flinfo = &varchar_finfo;
	locfcinfo.nargs = 2;
	/* We assume we are "strict" and need not worry about null inputs */
	locfcinfo.arg[0] = PointerGetDatum(v);
	locfcinfo.arg[1] = Int32GetDatum(len);

	return array_map(&locfcinfo, VARCHAROID, VARCHAROID);
}



/*****************************************************************************
 * Exported functions
 *****************************************************************************/

/* "True" length (not counting trailing blanks) of a BpChar */
static int
bcTruelen(BpChar *arg)
{
	char	   *s = VARDATA(arg);
	int			i;
	int			len;

	len = VARSIZE(arg) - VARHDRSZ;
	for (i = len - 1; i >= 0; i--)
	{
		if (s[i] != ' ')
			break;
	}
	return i + 1;
}

Datum
bpcharlen(PG_FUNCTION_ARGS)
{
	BpChar	   *arg = PG_GETARG_BPCHAR_P(0);

#ifdef MULTIBYTE
	unsigned char *s;
	int			len,
				l,
				wl;

	l = VARSIZE(arg) - VARHDRSZ;
	len = 0;
	s = VARDATA(arg);
	while (l > 0)
	{
		wl = pg_mblen(s);
		l -= wl;
		s += wl;
		len++;
	}
	PG_RETURN_INT32(len);
#else
	PG_RETURN_INT32(VARSIZE(arg) - VARHDRSZ);
#endif
}

Datum
bpcharoctetlen(PG_FUNCTION_ARGS)
{
	BpChar	   *arg = PG_GETARG_BPCHAR_P(0);

	PG_RETURN_INT32(VARSIZE(arg) - VARHDRSZ);
}


/*****************************************************************************
 *	Comparison Functions used for bpchar
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 *****************************************************************************/

Datum
bpchareq(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	/* fast path for different-length inputs */
	if (len1 != len2)
		result = false;
	else
		result = (varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bpcharne(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	/* fast path for different-length inputs */
	if (len1 != len2)
		result = true;
	else
		result = (varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bpcharlt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
bpcharle(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
bpchargt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp > 0);
}

Datum
bpcharge(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
bpcharcmp(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_P(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}


/*
 * bpchar needs a specialized hash function because we want to ignore
 * trailing blanks in comparisons.	(varchar can use plain hashvarlena.)
 */
Datum
hashbpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *key = PG_GETARG_BPCHAR_P(0);
	char	   *keydata;
	int			keylen;
	Datum		result;

	keydata = VARDATA(key);
	keylen = bcTruelen(key);

	result = hash_any(keydata, keylen);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(key, 0);

	return result;
}


/*****************************************************************************
 *	Functions used for varchar
 *****************************************************************************/

Datum
varcharlen(PG_FUNCTION_ARGS)
{
	VarChar    *arg = PG_GETARG_VARCHAR_P(0);

#ifdef MULTIBYTE
	unsigned char *s;
	int			len,
				l,
				wl;

	len = 0;
	s = VARDATA(arg);
	l = VARSIZE(arg) - VARHDRSZ;
	while (l > 0)
	{
		wl = pg_mblen(s);
		l -= wl;
		s += wl;
		len++;
	}
	PG_RETURN_INT32(len);
#else
	PG_RETURN_INT32(VARSIZE(arg) - VARHDRSZ);
#endif
}

Datum
varcharoctetlen(PG_FUNCTION_ARGS)
{
	VarChar    *arg = PG_GETARG_VARCHAR_P(0);

	PG_RETURN_INT32(VARSIZE(arg) - VARHDRSZ);
}


/*****************************************************************************
 *	Comparison Functions used for varchar
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 *****************************************************************************/

Datum
varchareq(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	/* fast path for different-length inputs */
	if (len1 != len2)
		result = false;
	else
		result = (varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
varcharne(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	/* fast path for different-length inputs */
	if (len1 != len2)
		result = true;
	else
		result = (varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
varcharlt(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
varcharle(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
varchargt(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp > 0);
}

Datum
varcharge(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
varcharcmp(PG_FUNCTION_ARGS)
{
	VarChar    *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar    *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = varstr_cmp(VARDATA(arg1), len1, VARDATA(arg2), len2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}
