/*-------------------------------------------------------------------------
 *
 * varchar.c
 *	  Functions for the built-in types char(n) and varchar(n).
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/varchar.c,v 1.102.4.2 2006/05/21 20:06:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

#include "mb/pg_wchar.h"


/*
 * CHAR() and VARCHAR() types are part of the ANSI SQL standard. CHAR()
 * is for blank-padded string whose length is specified in CREATE TABLE.
 * VARCHAR is for storing string whose length is at most the length specified
 * at CREATE TABLE time.
 *
 * It's hard to implement these types because we cannot figure out
 * the length of the type from the type itself. I changed (hopefully all) the
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
	size_t		len,
				maxlen;
	int			i;
	int			charlen;		/* number of charcters in the input string */

	len = strlen(s);
	charlen = pg_mbstrlen(s);

	/* If typmod is -1 (or invalid), use the actual string length */
	if (atttypmod < (int32) VARHDRSZ)
		maxlen = charlen;
	else
		maxlen = atttypmod - VARHDRSZ;

	if (charlen > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
		size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);

		/*
		 * at this point, len is the actual BYTE length of the input
		 * string, maxlen is the max number of CHARACTERS allowed for this
		 * bpchar type.
		 */
		if (strspn(s + mbmaxlen, " ") == len - mbmaxlen)
			len = mbmaxlen;
		else
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
					 errmsg("value too long for type character(%d)",
							(int) maxlen)));

		/*
		 * XXX: at this point, maxlen is the necessary byte length, not
		 * the number of CHARACTERS!
		 */
		maxlen = len;
	}
	else
	{
		/*
		 * XXX: at this point, maxlen is the necessary byte length, not
		 * the number of CHARACTERS!
		 */
		maxlen = len + (maxlen - charlen);
	}

	result = palloc(maxlen + VARHDRSZ);
	VARATT_SIZEP(result) = maxlen + VARHDRSZ;
	r = VARDATA(result);
	for (i = 0; i < len; i++)
		*r++ = *s++;

	/* blank pad the string if necessary */
	for (; i < maxlen; i++)
		*r++ = ' ';

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

	PG_RETURN_CSTRING(result);
}

/*
 *		bpcharrecv			- converts external binary format to bpchar
 */
Datum
bpcharrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as textrecv, so share code */
	return textrecv(fcinfo);
}

/*
 *		bpcharsend			- converts bpchar to binary format
 */
Datum
bpcharsend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as textsend, so share code */
	return textsend(fcinfo);
}


/*
 * Converts a CHARACTER type to the specified size.
 *
 * maxlen is the typmod, ie, declared length plus VARHDRSZ bytes.
 * isExplicit is true if this is for an explicit cast to char(N).
 *
 * Truncation rules: for an explicit cast, silently truncate to the given
 * length; for an implicit cast, raise error unless extra characters are
 * all spaces.	(This is sort-of per SQL: the spec would actually have us
 * raise a "completion condition" for the explicit cast case, but Postgres
 * hasn't got such a concept.)
 */
Datum
bpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *source = PG_GETARG_BPCHAR_P(0);
	int32		maxlen = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	BpChar	   *result;
	int32		len;
	char	   *r;
	char	   *s;
	int			i;
	int			charlen;		/* number of charcters in the input string
								 * + VARHDRSZ */

	len = VARSIZE(source);

	charlen = pg_mbstrlen_with_len(VARDATA(source), len - VARHDRSZ) + VARHDRSZ;

	/* No work if typmod is invalid or supplied data matches it already */
	if (maxlen < (int32) VARHDRSZ || charlen == maxlen)
		PG_RETURN_BPCHAR_P(source);

	if (charlen > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
		size_t		maxmblen;

		maxmblen = pg_mbcharcliplen(VARDATA(source), len - VARHDRSZ,
									maxlen - VARHDRSZ) + VARHDRSZ;

		if (!isExplicit)
		{
			for (i = maxmblen - VARHDRSZ; i < len - VARHDRSZ; i++)
				if (*(VARDATA(source) + i) != ' ')
					ereport(ERROR,
							(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
						  errmsg("value too long for type character(%d)",
								 maxlen - VARHDRSZ)));
		}

		len = maxmblen;

		/*
		 * XXX: at this point, maxlen is the necessary byte
		 * length+VARHDRSZ, not the number of CHARACTERS!
		 */
		maxlen = len;
	}
	else
	{
		/*
		 * XXX: at this point, maxlen is the necessary byte
		 * length+VARHDRSZ, not the number of CHARACTERS!
		 */
		maxlen = len + (maxlen - charlen);
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
 *	 varchar - varchar(n)
 *
 * Note: varchar piggybacks on type text for most operations, and so has no
 * C-coded functions except for I/O and typmod checking.
 *****************************************************************************/

/*
 * Convert a C string to VARCHAR internal representation.  atttypmod
 * is the declared length of the type plus VARHDRSZ.
 *
 * Note that atttypmod is regarded as the number of characters, which
 * is not necessarily the same as the number of bytes.
 *
 * If the C string is too long, raise an error, unless the extra characters
 * are spaces, in which case they're truncated.  (per SQL)
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
	size_t		len,
				maxlen;

	len = strlen(s);
	maxlen = atttypmod - VARHDRSZ;

	if (atttypmod >= (int32) VARHDRSZ && len > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
		size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);

		if (strspn(s + mbmaxlen, " ") == len - mbmaxlen)
			len = mbmaxlen;
		else
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				  errmsg("value too long for type character varying(%d)",
						 (int) maxlen)));
	}

	result = palloc(len + VARHDRSZ);
	VARATT_SIZEP(result) = len + VARHDRSZ;
	memcpy(VARDATA(result), s, len);

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

	PG_RETURN_CSTRING(result);
}

/*
 *		varcharrecv			- converts external binary format to varchar
 */
Datum
varcharrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as textrecv, so share code */
	return textrecv(fcinfo);
}

/*
 *		varcharsend			- converts varchar to binary format
 */
Datum
varcharsend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as textsend, so share code */
	return textsend(fcinfo);
}


/*
 * Converts a VARCHAR type to the specified size.
 *
 * maxlen is the typmod, ie, declared length plus VARHDRSZ bytes.
 * isExplicit is true if this is for an explicit cast to varchar(N).
 *
 * Truncation rules: for an explicit cast, silently truncate to the given
 * length; for an implicit cast, raise error unless extra characters are
 * all spaces.	(This is sort-of per SQL: the spec would actually have us
 * raise a "completion condition" for the explicit cast case, but Postgres
 * hasn't got such a concept.)
 */
Datum
varchar(PG_FUNCTION_ARGS)
{
	VarChar    *source = PG_GETARG_VARCHAR_P(0);
	int32		maxlen = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	VarChar    *result;
	int32		len;
	size_t		maxmblen;
	int			i;

	len = VARSIZE(source);
	/* No work if typmod is invalid or supplied data fits it already */
	if (maxlen < (int32) VARHDRSZ || len <= maxlen)
		PG_RETURN_VARCHAR_P(source);

	/* only reach here if string is too long... */

	/* truncate multibyte string preserving multibyte boundary */
	maxmblen = pg_mbcharcliplen(VARDATA(source), len - VARHDRSZ,
								maxlen - VARHDRSZ);

	if (!isExplicit)
	{
		for (i = maxmblen; i < len - VARHDRSZ; i++)
			if (*(VARDATA(source) + i) != ' ')
				ereport(ERROR,
						(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				  errmsg("value too long for type character varying(%d)",
						 maxlen - VARHDRSZ)));
	}

	len = maxmblen + VARHDRSZ;
	result = palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), VARDATA(source), len - VARHDRSZ);

	PG_RETURN_VARCHAR_P(result);
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

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() <= 1)
		PG_RETURN_INT32(VARSIZE(arg) - VARHDRSZ);

	PG_RETURN_INT32(
			  pg_mbstrlen_with_len(VARDATA(arg), VARSIZE(arg) - VARHDRSZ)
		);
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

	/*
	 * Since we only care about equality or not-equality, we can avoid all
	 * the expense of strcoll() here, and just do bitwise comparison.
	 */
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

	/*
	 * Since we only care about equality or not-equality, we can avoid all
	 * the expense of strcoll() here, and just do bitwise comparison.
	 */
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
 * trailing blanks in comparisons.
 *
 * Note: currently there is no need for locale-specific behavior here,
 * but if we ever change the semantics of bpchar comparison to trust
 * strcoll() completely, we'd need to do something different in non-C locales.
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

	result = hash_any((unsigned char *) keydata, keylen);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(key, 0);

	return result;
}
