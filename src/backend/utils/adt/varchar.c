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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/varchar.c,v 1.73 2001/01/24 19:43:14 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_type.h"
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
 * bpcharin -
 *	  converts a string of char() type to the internal representation.
 *	  len is the length specified in () plus VARHDRSZ bytes.
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
	int			len;
	int			i;

	if (atttypmod < (int32) VARHDRSZ)
	{
		/* If typmod is -1 (or invalid), use the actual string length */
		len = strlen(s);
		atttypmod = len + VARHDRSZ;
	}
	else
#ifdef MULTIBYTE
	{
		/*
		 * truncate multi-byte string preserving multi-byte
		 * boundary
		 */
		len = pg_mbcliplen(s, atttypmod - VARHDRSZ, atttypmod - VARHDRSZ);
	}
#else
		len = atttypmod - VARHDRSZ;
#endif

	result = (BpChar *) palloc(atttypmod);
	VARATT_SIZEP(result) = atttypmod;
	r = VARDATA(result);
	for (i = 0; i < len; i++, r++, s++)
	{
		*r = *s;
		if (*r == '\0')
			break;
	}

#ifdef CYR_RECODE
	convertstr(VARDATA(result), len, 0);
#endif

	/* blank pad the string if necessary */
#ifdef MULTIBYTE
	for (; i < atttypmod - VARHDRSZ; i++)
#else
	for (; i < len; i++)
#endif
		*r++ = ' ';

	PG_RETURN_BPCHAR_P(result);
}

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

/* bpchar()
 * Converts a char() type to a specific internal length.
 * len is the length specified in () plus VARHDRSZ bytes.
 */
Datum
bpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *str = PG_GETARG_BPCHAR_P(0);
	int32		len = PG_GETARG_INT32(1);
	BpChar	   *result;
	char	   *r,
			   *s;
	int			rlen,
				slen;
	int			i;

	/* No work if typmod is invalid or supplied data matches it already */
	if (len < (int32) VARHDRSZ || len == VARSIZE(str))
		PG_RETURN_BPCHAR_P(str);

	rlen = len - VARHDRSZ;

#ifdef STRINGDEBUG
	printf("bpchar- convert string length %d (%d) ->%d (%d)\n",
		   VARSIZE(str) - VARHDRSZ, VARSIZE(str), rlen, len);
#endif

	result = (BpChar *) palloc(len);
	VARATT_SIZEP(result) = len;
	r = VARDATA(result);

#ifdef MULTIBYTE
	/*
	 * truncate multi-byte string in a way not to break multi-byte
	 * boundary
	 */
	if (VARSIZE(str) > len)
		slen = pg_mbcliplen(VARDATA(str), VARSIZE(str) - VARHDRSZ, rlen);
	else
		slen = VARSIZE(str) - VARHDRSZ;
#else
	slen = VARSIZE(str) - VARHDRSZ;
#endif
	s = VARDATA(str);

#ifdef STRINGDEBUG
	printf("bpchar- string is '");
#endif

	for (i = 0; (i < rlen) && (i < slen); i++)
	{
#ifdef STRINGDEBUG
		printf("%c", *s);
#endif
		*r++ = *s++;
	}

#ifdef STRINGDEBUG
	printf("'\n");
#endif

	/* blank pad the string if necessary */
	for (; i < rlen; i++)
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
	FunctionCallInfoData	locfcinfo;
	/*
	 * Since bpchar() is a built-in function, we should only need to
	 * look it up once per run.
	 */
	static FmgrInfo			bpchar_finfo;

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
		len = NAMEDATALEN-1;

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
 * varcharin -
 *	  converts a string of varchar() type to the internal representation.
 *	  len is the length specified in () plus VARHDRSZ bytes.
 */
Datum
varcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarChar	   *result;
	int			len;

	len = strlen(s) + VARHDRSZ;
	if (atttypmod >= (int32) VARHDRSZ && len > atttypmod)
#ifdef MULTIBYTE
 		len = pg_mbcliplen(s, len - VARHDRSZ, atttypmod - VARHDRSZ) + VARHDRSZ;
#else
		len = atttypmod;		/* clip the string at max length */
#endif

	result = (VarChar *) palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), s, len - VARHDRSZ);

#ifdef CYR_RECODE
	convertstr(VARDATA(result), len, 0);
#endif

	PG_RETURN_VARCHAR_P(result);
}

Datum
varcharout(PG_FUNCTION_ARGS)
{
	VarChar	   *s = PG_GETARG_VARCHAR_P(0);
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

/* varchar()
 * Converts a varchar() type to the specified size.
 * slen is the length specified in () plus VARHDRSZ bytes.
 */
Datum
varchar(PG_FUNCTION_ARGS)
{
	VarChar	   *s = PG_GETARG_VARCHAR_P(0);
	int32		slen = PG_GETARG_INT32(1);
	VarChar	   *result;
	int			len;

	len = VARSIZE(s);
	if (slen < (int32) VARHDRSZ || len <= slen)
		PG_RETURN_VARCHAR_P(s);

	/* only reach here if we need to truncate string... */

#ifdef MULTIBYTE

	/*
	 * truncate multi-byte string preserving multi-byte
	 * boundary
	 */
	len = pg_mbcliplen(VARDATA(s), slen - VARHDRSZ, slen - VARHDRSZ);
	slen = len + VARHDRSZ;
#else
	len = slen - VARHDRSZ;
#endif

	result = (VarChar *) palloc(slen);
	VARATT_SIZEP(result) = slen;
	memcpy(VARDATA(result), VARDATA(s), len);

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
	FunctionCallInfoData	locfcinfo;
	/*
	 * Since varchar() is a built-in function, we should only need to
	 * look it up once per run.
	 */
	static FmgrInfo			varchar_finfo;

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

	if (len1 != len2)
		result = false;
	else
		result = (strncmp(VARDATA(arg1), VARDATA(arg2), len1) == 0);

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

	if (len1 != len2)
		result = true;
	else
		result = (strncmp(VARDATA(arg1), VARDATA(arg2), len1) != 0);

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
 * trailing blanks in comparisons.  (varchar can use plain hashvarlena.)
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
	VarChar	   *arg = PG_GETARG_VARCHAR_P(0);
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
	VarChar	   *arg = PG_GETARG_VARCHAR_P(0);

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
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	if (len1 != len2)
		result = false;
	else
		result = (strncmp(VARDATA(arg1), VARDATA(arg2), len1) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
varcharne(PG_FUNCTION_ARGS)
{
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	if (len1 != len2)
		result = true;
	else
		result = (strncmp(VARDATA(arg1), VARDATA(arg2), len1) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
varcharlt(PG_FUNCTION_ARGS)
{
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
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
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
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
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
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
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
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
	VarChar	   *arg1 = PG_GETARG_VARCHAR_P(0);
	VarChar	   *arg2 = PG_GETARG_VARCHAR_P(1);
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
