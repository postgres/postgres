/*-------------------------------------------------------------------------
 *
 * varlena.c
 *	  Functions for the variable-length built-in types.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/varlena.c,v 1.139.2.3 2006/05/21 20:05:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/tuptoaster.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/crypt.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "regex/regex.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"


typedef struct varlena unknown;

#define DatumGetUnknownP(X)			((unknown *) PG_DETOAST_DATUM(X))
#define DatumGetUnknownPCopy(X)		((unknown *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_UNKNOWN_P(n)		DatumGetUnknownP(PG_GETARG_DATUM(n))
#define PG_GETARG_UNKNOWN_P_COPY(n) DatumGetUnknownPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_UNKNOWN_P(x)		PG_RETURN_POINTER(x)

#define PG_TEXTARG_GET_STR(arg_) \
	DatumGetCString(DirectFunctionCall1(textout, PG_GETARG_DATUM(arg_)))
#define PG_TEXT_GET_STR(textp_) \
	DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp_)))
#define PG_STR_GET_TEXT(str_) \
	DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(str_)))
#define TEXTLEN(textp) \
	text_length(PointerGetDatum(textp))
#define TEXTPOS(buf_text, from_sub_text) \
	text_position(buf_text, from_sub_text, 1)
#define LEFT(buf_text, from_sub_text) \
	text_substring(PointerGetDatum(buf_text), \
					1, \
					TEXTPOS(buf_text, from_sub_text) - 1, false)

static int	text_cmp(text *arg1, text *arg2);
static int32 text_length(Datum str);
static int32 text_position(text *t1, text *t2, int matchnum);
static text *text_substring(Datum str,
			   int32 start,
			   int32 length,
			   bool length_not_specified);

static void appendStringInfoText(StringInfo str, const text *t);


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/


#define VAL(CH)			((CH) - '0')
#define DIG(VAL)		((VAL) + '0')

/*
 *		byteain			- converts from printable representation of byte array
 *
 *		Non-printable characters must be passed as '\nnn' (octal) and are
 *		converted to internal form.  '\' must be passed as '\\'.
 *		ereport(ERROR, ...) if bad form.
 *
 *		BUGS:
 *				The input is scaned twice.
 *				The error checking of input is minimal.
 */
Datum
byteain(PG_FUNCTION_ARGS)
{
	char	   *inputText = PG_GETARG_CSTRING(0);
	char	   *tp;
	char	   *rp;
	int			byte;
	bytea	   *result;

	for (byte = 0, tp = inputText; *tp != '\0'; byte++)
	{
		if (tp[0] != '\\')
			tp++;
		else if ((tp[0] == '\\') &&
				 (tp[1] >= '0' && tp[1] <= '3') &&
				 (tp[2] >= '0' && tp[2] <= '7') &&
				 (tp[3] >= '0' && tp[3] <= '7'))
			tp += 4;
		else if ((tp[0] == '\\') &&
				 (tp[1] == '\\'))
			tp += 2;
		else
		{
			/*
			 * one backslash, not followed by 0 or ### valid octal
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type bytea")));
		}
	}

	byte += VARHDRSZ;
	result = (bytea *) palloc(byte);
	VARATT_SIZEP(result) = byte;	/* set varlena length */

	tp = inputText;
	rp = VARDATA(result);
	while (*tp != '\0')
	{
		if (tp[0] != '\\')
			*rp++ = *tp++;
		else if ((tp[0] == '\\') &&
				 (tp[1] >= '0' && tp[1] <= '3') &&
				 (tp[2] >= '0' && tp[2] <= '7') &&
				 (tp[3] >= '0' && tp[3] <= '7'))
		{
			byte = VAL(tp[1]);
			byte <<= 3;
			byte += VAL(tp[2]);
			byte <<= 3;
			*rp++ = byte + VAL(tp[3]);
			tp += 4;
		}
		else if ((tp[0] == '\\') &&
				 (tp[1] == '\\'))
		{
			*rp++ = '\\';
			tp += 2;
		}
		else
		{
			/*
			 * We should never get here. The first pass should not allow it.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type bytea")));
		}
	}

	PG_RETURN_BYTEA_P(result);
}

/*
 *		byteaout		- converts to printable representation of byte array
 *
 *		Non-printable characters are inserted as '\nnn' (octal) and '\' as
 *		'\\'.
 *
 *		NULL vlena should be an error--returning string with NULL for now.
 */
Datum
byteaout(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_P(0);
	char	   *result;
	char	   *vp;
	char	   *rp;
	int			val;			/* holds unprintable chars */
	int			i;
	int			len;

	len = 1;					/* empty string has 1 char */
	vp = VARDATA(vlena);
	for (i = VARSIZE(vlena) - VARHDRSZ; i != 0; i--, vp++)
	{
		if (*vp == '\\')
			len += 2;
		else if ((unsigned char) *vp < 0x20 || (unsigned char) *vp > 0x7e)
			len += 4;
		else
			len++;
	}
	rp = result = (char *) palloc(len);
	vp = VARDATA(vlena);
	for (i = VARSIZE(vlena) - VARHDRSZ; i != 0; i--, vp++)
	{
		if (*vp == '\\')
		{
			*rp++ = '\\';
			*rp++ = '\\';
		}
		else if ((unsigned char) *vp < 0x20 || (unsigned char) *vp > 0x7e)
		{
			val = *vp;
			rp[0] = '\\';
			rp[3] = DIG(val & 07);
			val >>= 3;
			rp[2] = DIG(val & 07);
			val >>= 3;
			rp[1] = DIG(val & 03);
			rp += 4;
		}
		else
			*rp++ = *vp;
	}
	*rp = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		bytearecv			- converts external binary format to bytea
 */
Datum
bytearecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	bytea	   *result;
	int			nbytes;

	nbytes = buf->len - buf->cursor;
	result = (bytea *) palloc(nbytes + VARHDRSZ);
	VARATT_SIZEP(result) = nbytes + VARHDRSZ;
	pq_copymsgbytes(buf, VARDATA(result), nbytes);
	PG_RETURN_BYTEA_P(result);
}

/*
 *		byteasend			- converts bytea to binary format
 *
 * This is a special case: just copy the input...
 */
Datum
byteasend(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_P_COPY(0);

	PG_RETURN_BYTEA_P(vlena);
}


/*
 *		textin			- converts "..." to internal representation
 */
Datum
textin(PG_FUNCTION_ARGS)
{
	char	   *inputText = PG_GETARG_CSTRING(0);
	text	   *result;
	int			len;

	len = strlen(inputText);
	result = (text *) palloc(len + VARHDRSZ);
	VARATT_SIZEP(result) = len + VARHDRSZ;

	memcpy(VARDATA(result), inputText, len);

	PG_RETURN_TEXT_P(result);
}

/*
 *		textout			- converts internal representation to "..."
 */
Datum
textout(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_P(0);
	int			len;
	char	   *result;

	len = VARSIZE(t) - VARHDRSZ;
	result = (char *) palloc(len + 1);
	memcpy(result, VARDATA(t), len);
	result[len] = '\0';

	PG_RETURN_CSTRING(result);
}

/*
 *		textrecv			- converts external binary format to text
 */
Datum
textrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	text	   *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	result = (text *) palloc(nbytes + VARHDRSZ);
	VARATT_SIZEP(result) = nbytes + VARHDRSZ;
	memcpy(VARDATA(result), str, nbytes);
	pfree(str);
	PG_RETURN_TEXT_P(result);
}

/*
 *		textsend			- converts text to binary format
 */
Datum
textsend(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, VARDATA(t), VARSIZE(t) - VARHDRSZ);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 *		unknownin			- converts "..." to internal representation
 */
Datum
unknownin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	/* representation is same as cstring */
	PG_RETURN_CSTRING(pstrdup(str));
}

/*
 *		unknownout			- converts internal representation to "..."
 */
Datum
unknownout(PG_FUNCTION_ARGS)
{
	/* representation is same as cstring */
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_CSTRING(pstrdup(str));
}

/*
 *		unknownrecv			- converts external binary format to unknown
 */
Datum
unknownrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	/* representation is same as cstring */
	PG_RETURN_CSTRING(str);
}

/*
 *		unknownsend			- converts unknown to binary format
 */
Datum
unknownsend(PG_FUNCTION_ARGS)
{
	/* representation is same as cstring */
	char	   *str = PG_GETARG_CSTRING(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, str, strlen(str));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* ========== PUBLIC ROUTINES ========== */

/*
 * textlen -
 *	  returns the logical length of a text*
 *	   (which is less than the VARSIZE of the text*)
 */
Datum
textlen(PG_FUNCTION_ARGS)
{
	Datum		str = PG_GETARG_DATUM(0);

	/* try to avoid decompressing argument */
	PG_RETURN_INT32(text_length(str));
}

/*
 * text_length -
 *	Does the real work for textlen()
 *
 *	This is broken out so it can be called directly by other string processing
 *	functions.	Note that the argument is passed as a Datum, to indicate that
 *	it may still be in compressed form.  We can avoid decompressing it at all
 *	in some cases.
 */
static int32
text_length(Datum str)
{
	/* fastpath when max encoding length is one */
	if (pg_database_encoding_max_length() == 1)
		PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);
	else
	{
		text	   *t = DatumGetTextP(str);

		PG_RETURN_INT32(pg_mbstrlen_with_len(VARDATA(t),
											 VARSIZE(t) - VARHDRSZ));
	}
}

/*
 * textoctetlen -
 *	  returns the physical length of a text*
 *	   (which is less than the VARSIZE of the text*)
 */
Datum
textoctetlen(PG_FUNCTION_ARGS)
{
	Datum		str = PG_GETARG_DATUM(0);

	/* We need not detoast the input at all */
	PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);
}

/*
 * textcat -
 *	  takes two text* and returns a text* that is the concatenation of
 *	  the two.
 *
 * Rewritten by Sapa, sapa@hq.icb.chel.su. 8-Jul-96.
 * Updated by Thomas, Thomas.Lockhart@jpl.nasa.gov 1997-07-10.
 * Allocate space for output in all cases.
 * XXX - thomas 1997-07-10
 */
Datum
textcat(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_P(0);
	text	   *t2 = PG_GETARG_TEXT_P(1);
	int			len1,
				len2,
				len;
	text	   *result;
	char	   *ptr;

	len1 = VARSIZE(t1) - VARHDRSZ;
	if (len1 < 0)
		len1 = 0;

	len2 = VARSIZE(t2) - VARHDRSZ;
	if (len2 < 0)
		len2 = 0;

	len = len1 + len2 + VARHDRSZ;
	result = (text *) palloc(len);

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	if (len1 > 0)
		memcpy(ptr, VARDATA(t1), len1);
	if (len2 > 0)
		memcpy(ptr + len1, VARDATA(t2), len2);

	PG_RETURN_TEXT_P(result);
}

/*
 * text_substr()
 * Return a substring starting at the specified position.
 * - thomas 1997-12-31
 *
 * Input:
 *	- string
 *	- starting position (is one-based)
 *	- string length
 *
 * If the starting position is zero or less, then return from the start of the string
 *	adjusting the length to be consistent with the "negative start" per SQL92.
 * If the length is less than zero, return the remaining string.
 *
 * Added multibyte support.
 * - Tatsuo Ishii 1998-4-21
 * Changed behavior if starting position is less than one to conform to SQL92 behavior.
 * Formerly returned the entire string; now returns a portion.
 * - Thomas Lockhart 1998-12-10
 * Now uses faster TOAST-slicing interface
 * - John Gray 2002-02-22
 * Remove "#ifdef MULTIBYTE" and test for encoding_max_length instead. Change
 * behaviors conflicting with SQL92 to meet SQL92 (if E = S + L < S throw
 * error; if E < 1, return '', not entire string). Fixed MB related bug when
 * S > LC and < LC + 4 sometimes garbage characters are returned.
 * - Joe Conway 2002-08-10
 */
Datum
text_substr(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									PG_GETARG_INT32(2),
									false));
}

/*
 * text_substr_no_len -
 *	  Wrapper to avoid opr_sanity failure due to
 *	  one function accepting a different number of args.
 */
Datum
text_substr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									-1, true));
}

/*
 * text_substring -
 *	Does the real work for text_substr() and text_substr_no_len()
 *
 *	This is broken out so it can be called directly by other string processing
 *	functions.	Note that the argument is passed as a Datum, to indicate that
 *	it may still be in compressed/toasted form.  We can avoid detoasting all
 *	of it in some cases.
 */
static text *
text_substring(Datum str, int32 start, int32 length, bool length_not_specified)
{
	int32		eml = pg_database_encoding_max_length();
	int32		S = start;		/* start position */
	int32		S1;				/* adjusted start position */
	int32		L1;				/* adjusted substring length */

	/* life is easy if the encoding max length is 1 */
	if (eml == 1)
	{
		S1 = Max(S, 1);

		if (length_not_specified)		/* special case - get length to end of
										 * string */
			L1 = -1;
		else
		{
			/* end position */
			int			E = S + length;

			/*
			 * A negative value for L is the only way for the end position to
			 * be before the start. SQL99 says to throw an error.
			 */
			if (E < S)
				ereport(ERROR,
						(errcode(ERRCODE_SUBSTRING_ERROR),
						 errmsg("negative substring length not allowed")));

			/*
			 * A zero or negative value for the end position can happen if the
			 * start was negative or one. SQL99 says to return a zero-length
			 * string.
			 */
			if (E < 1)
				return PG_STR_GET_TEXT("");

			L1 = E - S1;
		}

		/*
		 * If the start position is past the end of the string, SQL99 says to
		 * return a zero-length string -- PG_GETARG_TEXT_P_SLICE() will do
		 * that for us. Convert to zero-based starting position
		 */
		return DatumGetTextPSlice(str, S1 - 1, L1);
	}
	else if (eml > 1)
	{
		/*
		 * When encoding max length is > 1, we can't get LC without
		 * detoasting, so we'll grab a conservatively large slice now and go
		 * back later to do the right thing
		 */
		int32		slice_start;
		int32		slice_size;
		int32		slice_strlen;
		text	   *slice;
		int32		E1;
		int32		i;
		char	   *p;
		char	   *s;
		text	   *ret;

		/*
		 * if S is past the end of the string, the tuple toaster will return a
		 * zero-length string to us
		 */
		S1 = Max(S, 1);

		/*
		 * We need to start at position zero because there is no way to know
		 * in advance which byte offset corresponds to the supplied start
		 * position.
		 */
		slice_start = 0;

		if (length_not_specified)		/* special case - get length to end of
										 * string */
			slice_size = L1 = -1;
		else
		{
			int			E = S + length;

			/*
			 * A negative value for L is the only way for the end position to
			 * be before the start. SQL99 says to throw an error.
			 */
			if (E < S)
				ereport(ERROR,
						(errcode(ERRCODE_SUBSTRING_ERROR),
						 errmsg("negative substring length not allowed")));

			/*
			 * A zero or negative value for the end position can happen if the
			 * start was negative or one. SQL99 says to return a zero-length
			 * string.
			 */
			if (E < 1)
				return PG_STR_GET_TEXT("");

			/*
			 * if E is past the end of the string, the tuple toaster will
			 * truncate the length for us
			 */
			L1 = E - S1;

			/*
			 * Total slice size in bytes can't be any longer than the start
			 * position plus substring length times the encoding max length.
			 */
			slice_size = (S1 + L1) * eml;
		}
		slice = DatumGetTextPSlice(str, slice_start, slice_size);

		/* see if we got back an empty string */
		if ((VARSIZE(slice) - VARHDRSZ) == 0)
			return PG_STR_GET_TEXT("");

		/* Now we can get the actual length of the slice in MB characters */
		slice_strlen = pg_mbstrlen_with_len(VARDATA(slice), VARSIZE(slice) - VARHDRSZ);

		/*
		 * Check that the start position wasn't > slice_strlen. If so, SQL99
		 * says to return a zero-length string.
		 */
		if (S1 > slice_strlen)
			return PG_STR_GET_TEXT("");

		/*
		 * Adjust L1 and E1 now that we know the slice string length. Again
		 * remember that S1 is one based, and slice_start is zero based.
		 */
		if (L1 > -1)
			E1 = Min(S1 + L1, slice_start + 1 + slice_strlen);
		else
			E1 = slice_start + 1 + slice_strlen;

		/*
		 * Find the start position in the slice; remember S1 is not zero based
		 */
		p = VARDATA(slice);
		for (i = 0; i < S1 - 1; i++)
			p += pg_mblen(p);

		/* hang onto a pointer to our start position */
		s = p;

		/*
		 * Count the actual bytes used by the substring of the requested
		 * length.
		 */
		for (i = S1; i < E1; i++)
			p += pg_mblen(p);

		ret = (text *) palloc(VARHDRSZ + (p - s));
		VARATT_SIZEP(ret) = VARHDRSZ + (p - s);
		memcpy(VARDATA(ret), s, (p - s));

		return ret;
	}
	else
		elog(ERROR, "invalid backend encoding: encoding max length < 1");

	/* not reached: suppress compiler warning */
	return NULL;
}

/*
 * textpos -
 *	  Return the position of the specified substring.
 *	  Implements the SQL92 POSITION() function.
 *	  Ref: A Guide To The SQL Standard, Date & Darwen, 1997
 * - thomas 1997-07-27
 */
Datum
textpos(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *search_str = PG_GETARG_TEXT_P(1);

	PG_RETURN_INT32(text_position(str, search_str, 1));
}

/*
 * text_position -
 *	Does the real work for textpos()
 *
 * Inputs:
 *		t1 - string to be searched
 *		t2 - pattern to match within t1
 *		matchnum - number of the match to be found (1 is the first match)
 * Result:
 *		Character index of the first matched char, starting from 1,
 *		or 0 if no match.
 *
 *	This is broken out so it can be called directly by other string processing
 *	functions.
 */
static int32
text_position(text *t1, text *t2, int matchnum)
{
	int			match = 0,
				pos = 0,
				p,
				px,
				len1,
				len2;

	if (matchnum <= 0)
		return 0;				/* result for 0th match */

	if (VARSIZE(t2) <= VARHDRSZ)
		return 1;				/* result for empty pattern */

	len1 = VARSIZE(t1) - VARHDRSZ;
	len2 = VARSIZE(t2) - VARHDRSZ;

	if (pg_database_encoding_max_length() == 1)
	{
		/* simple case - single byte encoding */
		char	   *p1,
				   *p2;

		p1 = VARDATA(t1);
		p2 = VARDATA(t2);

		/* no use in searching str past point where search_str will fit */
		px = (len1 - len2);

		for (p = 0; p <= px; p++)
		{
			if ((*p1 == *p2) && (strncmp(p1, p2, len2) == 0))
			{
				if (++match == matchnum)
				{
					pos = p + 1;
					break;
				}
			}
			p1++;
		}
	}
	else
	{
		/* not as simple - multibyte encoding */
		pg_wchar   *p1,
				   *p2,
				   *ps1,
				   *ps2;

		ps1 = p1 = (pg_wchar *) palloc((len1 + 1) * sizeof(pg_wchar));
		(void) pg_mb2wchar_with_len(VARDATA(t1), p1, len1);
		len1 = pg_wchar_strlen(p1);
		ps2 = p2 = (pg_wchar *) palloc((len2 + 1) * sizeof(pg_wchar));
		(void) pg_mb2wchar_with_len(VARDATA(t2), p2, len2);
		len2 = pg_wchar_strlen(p2);

		/* no use in searching str past point where search_str will fit */
		px = (len1 - len2);

		for (p = 0; p <= px; p++)
		{
			if ((*p1 == *p2) && (pg_wchar_strncmp(p1, p2, len2) == 0))
			{
				if (++match == matchnum)
				{
					pos = p + 1;
					break;
				}
			}
			p1++;
		}

		pfree(ps1);
		pfree(ps2);
	}

	return pos;
}

/* varstr_cmp()
 * Comparison function for text strings with given lengths.
 * Includes locale support, but must copy strings to temporary memory
 *	to allow null-termination for inputs to strcoll().
 * Returns -1, 0 or 1
 */
int
varstr_cmp(char *arg1, int len1, char *arg2, int len2)
{
	int			result;

	/*
	 * Unfortunately, there is no strncoll(), so in the non-C locale case we
	 * have to do some memory copying.	This turns out to be significantly
	 * slower, so we optimize the case where LC_COLLATE is C.  We also try to
	 * optimize relatively-short strings by avoiding palloc/pfree overhead.
	 */
	if (lc_collate_is_c())
	{
		result = strncmp(arg1, arg2, Min(len1, len2));
		if ((result == 0) && (len1 != len2))
			result = (len1 < len2) ? -1 : 1;
	}
	else
	{
#define STACKBUFLEN		1024

		char		a1buf[STACKBUFLEN];
		char		a2buf[STACKBUFLEN];
		char	   *a1p,
				   *a2p;

#ifdef WIN32
		/* Win32 does not have UTF-8, so we need to map to UTF-16 */
		if (GetDatabaseEncoding() == PG_UTF8)
		{
			int			a1len;
			int			a2len;
			int			r;

			if (len1 >= STACKBUFLEN / 2)
			{
				a1len = len1 * 2 + 2;
				a1p = palloc(a1len);
			}
			else
			{
				a1len = STACKBUFLEN;
				a1p = a1buf;
			}
			if (len2 >= STACKBUFLEN / 2)
			{
				a2len = len2 * 2 + 2;
				a2p = palloc(a2len);
			}
			else
			{
				a2len = STACKBUFLEN;
				a2p = a2buf;
			}

			/* stupid Microsloth API does not work for zero-length input */
			if (len1 == 0)
				r = 0;
			else
			{
				r = MultiByteToWideChar(CP_UTF8, 0, arg1, len1,
										(LPWSTR) a1p, a1len / 2);
				if (!r)
					ereport(ERROR,
					 (errmsg("could not convert string to UTF-16: error %lu",
							 GetLastError())));
			}
			((LPWSTR) a1p)[r] = 0;

			if (len2 == 0)
				r = 0;
			else
			{
				r = MultiByteToWideChar(CP_UTF8, 0, arg2, len2,
										(LPWSTR) a2p, a2len / 2);
				if (!r)
					ereport(ERROR,
					 (errmsg("could not convert string to UTF-16: error %lu",
							 GetLastError())));
			}
			((LPWSTR) a2p)[r] = 0;

			errno = 0;
			result = wcscoll((LPWSTR) a1p, (LPWSTR) a2p);
			if (result == 2147483647)	/* _NLSCMPERROR; missing from mingw
										 * headers */
				ereport(ERROR,
						(errmsg("could not compare Unicode strings: %m")));

			if (a1p != a1buf)
				pfree(a1p);
			if (a2p != a2buf)
				pfree(a2p);

			return result;
		}
#endif   /* WIN32 */

		if (len1 >= STACKBUFLEN)
			a1p = (char *) palloc(len1 + 1);
		else
			a1p = a1buf;
		if (len2 >= STACKBUFLEN)
			a2p = (char *) palloc(len2 + 1);
		else
			a2p = a2buf;

		memcpy(a1p, arg1, len1);
		a1p[len1] = '\0';
		memcpy(a2p, arg2, len2);
		a2p[len2] = '\0';

		result = strcoll(a1p, a2p);

		/*
		 * In some locales strcoll() can claim that nonidentical strings are
		 * equal.  Believing that would be bad news for a number of reasons,
		 * so we follow Perl's lead and sort "equal" strings according to
		 * strcmp().
		 */
		if (result == 0)
			result = strcmp(a1p, a2p);

		if (a1p != a1buf)
			pfree(a1p);
		if (a2p != a2buf)
			pfree(a2p);
	}

	return result;
}


/* text_cmp()
 * Internal comparison function for text strings.
 * Returns -1, 0 or 1
 */
static int
text_cmp(text *arg1, text *arg2)
{
	char	   *a1p,
			   *a2p;
	int			len1,
				len2;

	a1p = VARDATA(arg1);
	a2p = VARDATA(arg2);

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	return varstr_cmp(a1p, len1, a2p, len2);
}

/*
 * Comparison functions for text strings.
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 */

Datum
texteq(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	bool		result;

	/*
	 * Since we only care about equality or not-equality, we can avoid all
	 * the expense of strcoll() here, and just do bitwise comparison.
	 */
	if (VARSIZE(arg1) != VARSIZE(arg2))
		result = false;
	else
		result = (strncmp(VARDATA(arg1), VARDATA(arg2),
						  VARSIZE(arg1) - VARHDRSZ) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
textne(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	bool		result;

	/*
	 * Since we only care about equality or not-equality, we can avoid all
	 * the expense of strcoll() here, and just do bitwise comparison.
	 */
	if (VARSIZE(arg1) != VARSIZE(arg2))
		result = true;
	else
		result = (strncmp(VARDATA(arg1), VARDATA(arg2),
						  VARSIZE(arg1) - VARHDRSZ) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_lt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	bool		result;

	result = (text_cmp(arg1, arg2) < 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_le(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	bool		result;

	result = (text_cmp(arg1, arg2) <= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_gt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	bool		result;

	result = (text_cmp(arg1, arg2) > 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_ge(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	bool		result;

	result = (text_cmp(arg1, arg2) >= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bttextcmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int32		result;

	result = text_cmp(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}


Datum
text_larger(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	text	   *result;

	result = ((text_cmp(arg1, arg2) > 0) ? arg1 : arg2);

	PG_RETURN_TEXT_P(result);
}

Datum
text_smaller(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	text	   *result;

	result = ((text_cmp(arg1, arg2) < 0) ? arg1 : arg2);

	PG_RETURN_TEXT_P(result);
}


/*
 * The following operators support character-by-character comparison
 * of text data types, to allow building indexes suitable for LIKE
 * clauses.
 */

static int
internal_text_pattern_compare(text *arg1, text *arg2)
{
	int			result;

	result = memcmp(VARDATA(arg1), VARDATA(arg2),
					Min(VARSIZE(arg1), VARSIZE(arg2)) - VARHDRSZ);
	if (result != 0)
		return result;
	else if (VARSIZE(arg1) < VARSIZE(arg2))
		return -1;
	else if (VARSIZE(arg1) > VARSIZE(arg2))
		return 1;
	else
		return 0;
}


Datum
text_pattern_lt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result < 0);
}


Datum
text_pattern_le(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result <= 0);
}


Datum
text_pattern_eq(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	if (VARSIZE(arg1) != VARSIZE(arg2))
		result = 1;
	else
		result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result == 0);
}


Datum
text_pattern_ge(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result >= 0);
}


Datum
text_pattern_gt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result > 0);
}


Datum
text_pattern_ne(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	if (VARSIZE(arg1) != VARSIZE(arg2))
		result = 1;
	else
		result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result != 0);
}


Datum
bttext_pattern_cmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);
	text	   *arg2 = PG_GETARG_TEXT_P(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}


/*-------------------------------------------------------------
 * byteaoctetlen
 *
 * get the number of bytes contained in an instance of type 'bytea'
 *-------------------------------------------------------------
 */
Datum
byteaoctetlen(PG_FUNCTION_ARGS)
{
	Datum		str = PG_GETARG_DATUM(0);

	/* We need not detoast the input at all */
	PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);
}

/*
 * byteacat -
 *	  takes two bytea* and returns a bytea* that is the concatenation of
 *	  the two.
 *
 * Cloned from textcat and modified as required.
 */
Datum
byteacat(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_P(0);
	bytea	   *t2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2,
				len;
	bytea	   *result;
	char	   *ptr;

	len1 = VARSIZE(t1) - VARHDRSZ;
	if (len1 < 0)
		len1 = 0;

	len2 = VARSIZE(t2) - VARHDRSZ;
	if (len2 < 0)
		len2 = 0;

	len = len1 + len2 + VARHDRSZ;
	result = (bytea *) palloc(len);

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	if (len1 > 0)
		memcpy(ptr, VARDATA(t1), len1);
	if (len2 > 0)
		memcpy(ptr + len1, VARDATA(t2), len2);

	PG_RETURN_BYTEA_P(result);
}

#define PG_STR_GET_BYTEA(str_) \
	DatumGetByteaP(DirectFunctionCall1(byteain, CStringGetDatum(str_)))
/*
 * bytea_substr()
 * Return a substring starting at the specified position.
 * Cloned from text_substr and modified as required.
 *
 * Input:
 *	- string
 *	- starting position (is one-based)
 *	- string length (optional)
 *
 * If the starting position is zero or less, then return from the start of the string
 * adjusting the length to be consistent with the "negative start" per SQL92.
 * If the length is less than zero, an ERROR is thrown. If no third argument
 * (length) is provided, the length to the end of the string is assumed.
 */
Datum
bytea_substr(PG_FUNCTION_ARGS)
{
	int			S = PG_GETARG_INT32(1); /* start position */
	int			S1;				/* adjusted start position */
	int			L1;				/* adjusted substring length */

	S1 = Max(S, 1);

	if (fcinfo->nargs == 2)
	{
		/*
		 * Not passed a length - PG_GETARG_BYTEA_P_SLICE() grabs everything to
		 * the end of the string if we pass it a negative value for length.
		 */
		L1 = -1;
	}
	else
	{
		/* end position */
		int			E = S + PG_GETARG_INT32(2);

		/*
		 * A negative value for L is the only way for the end position to be
		 * before the start. SQL99 says to throw an error.
		 */
		if (E < S)
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));

		/*
		 * A zero or negative value for the end position can happen if the
		 * start was negative or one. SQL99 says to return a zero-length
		 * string.
		 */
		if (E < 1)
			PG_RETURN_BYTEA_P(PG_STR_GET_BYTEA(""));

		L1 = E - S1;
	}

	/*
	 * If the start position is past the end of the string, SQL99 says to
	 * return a zero-length string -- PG_GETARG_TEXT_P_SLICE() will do that
	 * for us. Convert to zero-based starting position
	 */
	PG_RETURN_BYTEA_P(PG_GETARG_BYTEA_P_SLICE(0, S1 - 1, L1));
}

/*
 * bytea_substr_no_len -
 *	  Wrapper to avoid opr_sanity failure due to
 *	  one function accepting a different number of args.
 */
Datum
bytea_substr_no_len(PG_FUNCTION_ARGS)
{
	return bytea_substr(fcinfo);
}

/*
 * byteapos -
 *	  Return the position of the specified substring.
 *	  Implements the SQL92 POSITION() function.
 * Cloned from textpos and modified as required.
 */
Datum
byteapos(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_P(0);
	bytea	   *t2 = PG_GETARG_BYTEA_P(1);
	int			pos;
	int			px,
				p;
	int			len1,
				len2;
	char	   *p1,
			   *p2;

	if (VARSIZE(t2) <= VARHDRSZ)
		PG_RETURN_INT32(1);		/* result for empty pattern */

	len1 = VARSIZE(t1) - VARHDRSZ;
	len2 = VARSIZE(t2) - VARHDRSZ;

	p1 = VARDATA(t1);
	p2 = VARDATA(t2);

	pos = 0;
	px = (len1 - len2);
	for (p = 0; p <= px; p++)
	{
		if ((*p2 == *p1) && (memcmp(p1, p2, len2) == 0))
		{
			pos = p + 1;
			break;
		};
		p1++;
	};

	PG_RETURN_INT32(pos);
}

/*-------------------------------------------------------------
 * byteaGetByte
 *
 * this routine treats "bytea" as an array of bytes.
 * It returns the Nth byte (a number between 0 and 255).
 *-------------------------------------------------------------
 */
Datum
byteaGetByte(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_P(0);
	int32		n = PG_GETARG_INT32(1);
	int			len;
	int			byte;

	len = VARSIZE(v) - VARHDRSZ;

	if (n < 0 || n >= len)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len - 1)));

	byte = ((unsigned char *) VARDATA(v))[n];

	PG_RETURN_INT32(byte);
}

/*-------------------------------------------------------------
 * byteaGetBit
 *
 * This routine treats a "bytea" type like an array of bits.
 * It returns the value of the Nth bit (0 or 1).
 *
 *-------------------------------------------------------------
 */
Datum
byteaGetBit(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_P(0);
	int32		n = PG_GETARG_INT32(1);
	int			byteNo,
				bitNo;
	int			len;
	int			byte;

	len = VARSIZE(v) - VARHDRSZ;

	if (n < 0 || n >= len * 8)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len * 8 - 1)));

	byteNo = n / 8;
	bitNo = n % 8;

	byte = ((unsigned char *) VARDATA(v))[byteNo];

	if (byte & (1 << bitNo))
		PG_RETURN_INT32(1);
	else
		PG_RETURN_INT32(0);
}

/*-------------------------------------------------------------
 * byteaSetByte
 *
 * Given an instance of type 'bytea' creates a new one with
 * the Nth byte set to the given value.
 *
 *-------------------------------------------------------------
 */
Datum
byteaSetByte(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_P(0);
	int32		n = PG_GETARG_INT32(1);
	int32		newByte = PG_GETARG_INT32(2);
	int			len;
	bytea	   *res;

	len = VARSIZE(v) - VARHDRSZ;

	if (n < 0 || n >= len)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len - 1)));

	/*
	 * Make a copy of the original varlena.
	 */
	res = (bytea *) palloc(VARSIZE(v));
	memcpy((char *) res, (char *) v, VARSIZE(v));

	/*
	 * Now set the byte.
	 */
	((unsigned char *) VARDATA(res))[n] = newByte;

	PG_RETURN_BYTEA_P(res);
}

/*-------------------------------------------------------------
 * byteaSetBit
 *
 * Given an instance of type 'bytea' creates a new one with
 * the Nth bit set to the given value.
 *
 *-------------------------------------------------------------
 */
Datum
byteaSetBit(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_P(0);
	int32		n = PG_GETARG_INT32(1);
	int32		newBit = PG_GETARG_INT32(2);
	bytea	   *res;
	int			len;
	int			oldByte,
				newByte;
	int			byteNo,
				bitNo;

	len = VARSIZE(v) - VARHDRSZ;

	if (n < 0 || n >= len * 8)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len * 8 - 1)));

	byteNo = n / 8;
	bitNo = n % 8;

	/*
	 * sanity check!
	 */
	if (newBit != 0 && newBit != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("new bit must be 0 or 1")));

	/*
	 * Make a copy of the original varlena.
	 */
	res = (bytea *) palloc(VARSIZE(v));
	memcpy((char *) res, (char *) v, VARSIZE(v));

	/*
	 * Update the byte.
	 */
	oldByte = ((unsigned char *) VARDATA(res))[byteNo];

	if (newBit == 0)
		newByte = oldByte & (~(1 << bitNo));
	else
		newByte = oldByte | (1 << bitNo);

	((unsigned char *) VARDATA(res))[byteNo] = newByte;

	PG_RETURN_BYTEA_P(res);
}


/* text_name()
 * Converts a text type to a Name type.
 */
Datum
text_name(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	Name		result;
	int			len;

	len = VARSIZE(s) - VARHDRSZ;

	/* Truncate oversize input */
	if (len >= NAMEDATALEN)
		len = NAMEDATALEN - 1;

#ifdef STRINGDEBUG
	printf("text- convert string length %d (%d) ->%d\n",
		   VARSIZE(s) - VARHDRSZ, VARSIZE(s), len);
#endif

	result = (Name) palloc(NAMEDATALEN);
	memcpy(NameStr(*result), VARDATA(s), len);

	/* now null pad to full length... */
	while (len < NAMEDATALEN)
	{
		*(NameStr(*result) + len) = '\0';
		len++;
	}

	PG_RETURN_NAME(result);
}

/* name_text()
 * Converts a Name type to a text type.
 */
Datum
name_text(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);
	text	   *result;
	int			len;

	len = strlen(NameStr(*s));

#ifdef STRINGDEBUG
	printf("text- convert string length %d (%d) ->%d\n",
		   VARSIZE(s) - VARHDRSZ, VARSIZE(s), len);
#endif

	result = palloc(VARHDRSZ + len);
	VARATT_SIZEP(result) = VARHDRSZ + len;
	memcpy(VARDATA(result), NameStr(*s), len);

	PG_RETURN_TEXT_P(result);
}


/*
 * textToQualifiedNameList - convert a text object to list of names
 *
 * This implements the input parsing needed by nextval() and other
 * functions that take a text parameter representing a qualified name.
 * We split the name at dots, downcase if not double-quoted, and
 * truncate names if they're too long.
 */
List *
textToQualifiedNameList(text *textval)
{
	char	   *rawname;
	List	   *result = NIL;
	List	   *namelist;
	ListCell   *l;

	/* Convert to C string (handles possible detoasting). */
	/* Note we rely on being able to modify rawname below. */
	rawname = DatumGetCString(DirectFunctionCall1(textout,
												  PointerGetDatum(textval)));

	if (!SplitIdentifierString(rawname, '.', &namelist))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	if (namelist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);

		result = lappend(result, makeString(pstrdup(curname)));
	}

	pfree(rawname);
	list_free(namelist);

	return result;
}

/*
 * SplitIdentifierString --- parse a string containing identifiers
 *
 * This is the guts of textToQualifiedNameList, and is exported for use in
 * other situations such as parsing GUC variables.	In the GUC case, it's
 * important to avoid memory leaks, so the API is designed to minimize the
 * amount of stuff that needs to be allocated and freed.
 *
 * Inputs:
 *	rawstring: the input string; must be overwritable!	On return, it's
 *			   been modified to contain the separated identifiers.
 *	separator: the separator punctuation expected between identifiers
 *			   (typically '.' or ',').	Whitespace may also appear around
 *			   identifiers.
 * Outputs:
 *	namelist: filled with a palloc'd list of pointers to identifiers within
 *			  rawstring.  Caller should list_free() this even on error return.
 *
 * Returns TRUE if okay, FALSE if there is a syntax error in the string.
 *
 * Note that an empty string is considered okay here, though not in
 * textToQualifiedNameList.
 */
bool
SplitIdentifierString(char *rawstring, char separator,
					  List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (isspace((unsigned char) *nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '\"')
		{
			/* Quoted name --- collapse quote-quote pairs, no downcasing */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '\"');
				if (endp == NULL)
					return false;		/* mismatched quotes */
				if (endp[1] != '\"')
					break;		/* found end of quoted name */
				/* Collapse adjacent quotes into one quote, and look again */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp now points at the terminating quote */
			nextp = endp + 1;
		}
		else
		{
			/* Unquoted name --- extends to separator or whitespace */
			char	   *downname;
			int			len;

			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !isspace((unsigned char) *nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* empty unquoted name not allowed */

			/*
			 * Downcase the identifier, using same code as main lexer does.
			 *
			 * XXX because we want to overwrite the input in-place, we cannot
			 * support a downcasing transformation that increases the string
			 * length.	This is not a problem given the current implementation
			 * of downcase_truncate_identifier, but we'll probably have to do
			 * something about this someday.
			 */
			len = endp - curname;
			downname = downcase_truncate_identifier(curname, len, false);
			Assert(strlen(downname) <= len);
			strncpy(curname, downname, len);
			pfree(downname);
		}

		while (isspace((unsigned char) *nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (isspace((unsigned char) *nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Now safe to overwrite separator with a null */
		*endp = '\0';

		/* Truncate name if it's overlength */
		truncate_identifier(curname, strlen(curname), false);

		/*
		 * Finished isolating current name --- add it to list
		 */
		*namelist = lappend(*namelist, curname);

		/* Loop back if we didn't reach end of string */
	} while (!done);

	return true;
}


/*****************************************************************************
 *	Comparison Functions used for bytea
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 *****************************************************************************/

Datum
byteaeq(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	/* fast path for different-length inputs */
	if (len1 != len2)
		result = false;
	else
		result = (memcmp(VARDATA(arg1), VARDATA(arg2), len1) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
byteane(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	bool		result;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	/* fast path for different-length inputs */
	if (len1 != len2)
		result = true;
	else
		result = (memcmp(VARDATA(arg1), VARDATA(arg2), len1) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bytealt(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = memcmp(VARDATA(arg1), VARDATA(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp < 0) || ((cmp == 0) && (len1 < len2)));
}

Datum
byteale(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = memcmp(VARDATA(arg1), VARDATA(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp < 0) || ((cmp == 0) && (len1 <= len2)));
}

Datum
byteagt(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = memcmp(VARDATA(arg1), VARDATA(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp > 0) || ((cmp == 0) && (len1 > len2)));
}

Datum
byteage(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = memcmp(VARDATA(arg1), VARDATA(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp > 0) || ((cmp == 0) && (len1 >= len2)));
}

Datum
byteacmp(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_P(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_P(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE(arg1) - VARHDRSZ;
	len2 = VARSIZE(arg2) - VARHDRSZ;

	cmp = memcmp(VARDATA(arg1), VARDATA(arg2), Min(len1, len2));
	if ((cmp == 0) && (len1 != len2))
		cmp = (len1 < len2) ? -1 : 1;

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}

/*
 * appendStringInfoText
 *
 * Append a text to str.
 * Like appendStringInfoString(str, PG_TEXT_GET_STR(s)) but faster.
 */
static void
appendStringInfoText(StringInfo str, const text *t)
{
	appendBinaryStringInfo(str, VARDATA(t), VARSIZE(t) - VARHDRSZ);
}

/*
 * replace_text
 * replace all occurrences of 'old_sub_str' in 'orig_str'
 * with 'new_sub_str' to form 'new_str'
 *
 * returns 'orig_str' if 'old_sub_str' == '' or 'orig_str' == ''
 * otherwise returns 'new_str'
 */
Datum
replace_text(PG_FUNCTION_ARGS)
{
	text	   *src_text = PG_GETARG_TEXT_P(0);
	text	   *from_sub_text = PG_GETARG_TEXT_P(1);
	text	   *to_sub_text = PG_GETARG_TEXT_P(2);
	int			src_text_len = TEXTLEN(src_text);
	int			from_sub_text_len = TEXTLEN(from_sub_text);
	text	   *left_text;
	text	   *right_text;
	text	   *buf_text;
	text	   *ret_text;
	int			curr_posn;
	StringInfo	str;

	if (src_text_len == 0 || from_sub_text_len == 0)
		PG_RETURN_TEXT_P(src_text);

	curr_posn = TEXTPOS(src_text, from_sub_text);

	/* When the from_sub_text is not found, there is nothing to do. */
	if (curr_posn == 0)
		PG_RETURN_TEXT_P(src_text);

	str = makeStringInfo();
	buf_text = src_text;

	while (curr_posn > 0)
	{
		left_text = text_substring(PointerGetDatum(buf_text),
								   1, curr_posn - 1, false);
		right_text = text_substring(PointerGetDatum(buf_text),
									curr_posn + from_sub_text_len, -1, true);

		appendStringInfoText(str, left_text);
		appendStringInfoText(str, to_sub_text);

		if (buf_text != src_text)
			pfree(buf_text);
		pfree(left_text);
		buf_text = right_text;
		curr_posn = TEXTPOS(buf_text, from_sub_text);
	}

	appendStringInfoText(str, buf_text);
	if (buf_text != src_text)
		pfree(buf_text);

	ret_text = PG_STR_GET_TEXT(str->data);
	pfree(str->data);
	pfree(str);

	PG_RETURN_TEXT_P(ret_text);
}

/*
 * check_replace_text_has_escape_char
 *
 * check whether replace_text contains escape char.
 */
static bool
check_replace_text_has_escape_char(const text *replace_text)
{
	const char *p = VARDATA(replace_text);
	const char *p_end = p + (VARSIZE(replace_text) - VARHDRSZ);

	if (pg_database_encoding_max_length() == 1)
	{
		for (; p < p_end; p++)
		{
			if (*p == '\\')
				return true;
		}
	}
	else
	{
		for (; p < p_end; p += pg_mblen(p))
		{
			if (*p == '\\')
				return true;
		}
	}

	return false;
}

/*
 * appendStringInfoRegexpSubstr
 *
 * Append replace_text to str, substituting regexp back references for
 * \n escapes.
 */
static void
appendStringInfoRegexpSubstr(StringInfo str, text *replace_text,
							 regmatch_t *pmatch, text *src_text)
{
	const char *p = VARDATA(replace_text);
	const char *p_end = p + (VARSIZE(replace_text) - VARHDRSZ);
	int			eml = pg_database_encoding_max_length();

	for (;;)
	{
		const char *chunk_start = p;
		int			so;
		int			eo;

		/* Find next escape char. */
		if (eml == 1)
		{
			for (; p < p_end && *p != '\\'; p++)
				 /* nothing */ ;
		}
		else
		{
			for (; p < p_end && *p != '\\'; p += pg_mblen(p))
				 /* nothing */ ;
		}

		/* Copy the text we just scanned over, if any. */
		if (p > chunk_start)
			appendBinaryStringInfo(str, chunk_start, p - chunk_start);

		/* Done if at end of string, else advance over escape char. */
		if (p >= p_end)
			break;
		p++;

		if (p >= p_end)
		{
			/* Escape at very end of input.  Treat same as unexpected char */
			appendStringInfoChar(str, '\\');
			break;
		}

		if (*p >= '1' && *p <= '9')
		{
			/* Use the back reference of regexp. */
			int			idx = *p - '0';

			so = pmatch[idx].rm_so;
			eo = pmatch[idx].rm_eo;
			p++;
		}
		else if (*p == '&')
		{
			/* Use the entire matched string. */
			so = pmatch[0].rm_so;
			eo = pmatch[0].rm_eo;
			p++;
		}
		else if (*p == '\\')
		{
			/* \\ means transfer one \ to output. */
			appendStringInfoChar(str, '\\');
			p++;
			continue;
		}
		else
		{
			/*
			 * If escape char is not followed by any expected char, just treat
			 * it as ordinary data to copy.  (XXX would it be better to throw
			 * an error?)
			 */
			appendStringInfoChar(str, '\\');
			continue;
		}

		if (so != -1 && eo != -1)
		{
			/*
			 * Copy the text that is back reference of regexp.	Because so and
			 * eo are counted in characters not bytes, it's easiest to use
			 * text_substring to pull out the correct chunk of text.
			 */
			text	   *append_text;

			append_text = text_substring(PointerGetDatum(src_text),
										 so + 1, (eo - so), false);
			appendStringInfoText(str, append_text);
			pfree(append_text);
		}
	}
}

#define REGEXP_REPLACE_BACKREF_CNT		10

/*
 * replace_text_regexp
 *
 * replace text that matches to regexp in src_text to replace_text.
 *
 * Note: to avoid having to include regex.h in builtins.h, we declare
 * the regexp argument as void *, but really it's regex_t *.
 */
text *
replace_text_regexp(text *src_text, void *regexp,
					text *replace_text, bool glob)
{
	text	   *ret_text;
	regex_t    *re = (regex_t *) regexp;
	int			src_text_len = VARSIZE(src_text) - VARHDRSZ;
	StringInfo	str = makeStringInfo();
	int			regexec_result;
	regmatch_t	pmatch[REGEXP_REPLACE_BACKREF_CNT];
	pg_wchar   *data;
	size_t		data_len;
	int			search_start;
	int			data_pos;
	bool		have_escape;

	/* Convert data string to wide characters. */
	data = (pg_wchar *) palloc((src_text_len + 1) * sizeof(pg_wchar));
	data_len = pg_mb2wchar_with_len(VARDATA(src_text), data, src_text_len);

	/* Check whether replace_text has escape char. */
	have_escape = check_replace_text_has_escape_char(replace_text);

	for (search_start = data_pos = 0; search_start <= data_len;)
	{
		regexec_result = pg_regexec(re,
									data,
									data_len,
									search_start,
									NULL,		/* no details */
									REGEXP_REPLACE_BACKREF_CNT,
									pmatch,
									0);

		if (regexec_result != REG_OKAY && regexec_result != REG_NOMATCH)
		{
			char		errMsg[100];

			/* re failed??? */
			pg_regerror(regexec_result, re, errMsg, sizeof(errMsg));
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("regular expression failed: %s", errMsg)));
		}

		if (regexec_result == REG_NOMATCH)
			break;

		/*
		 * Copy the text to the left of the match position.  Because we are
		 * working with character not byte indexes, it's easiest to use
		 * text_substring to pull out the needed data.
		 */
		if (pmatch[0].rm_so - data_pos > 0)
		{
			text	   *left_text;

			left_text = text_substring(PointerGetDatum(src_text),
									   data_pos + 1,
									   pmatch[0].rm_so - data_pos,
									   false);
			appendStringInfoText(str, left_text);
			pfree(left_text);
		}

		/*
		 * Copy the replace_text. Process back references when the
		 * replace_text has escape characters.
		 */
		if (have_escape)
			appendStringInfoRegexpSubstr(str, replace_text, pmatch, src_text);
		else
			appendStringInfoText(str, replace_text);

		search_start = data_pos = pmatch[0].rm_eo;

		/*
		 * When global option is off, replace the first instance only.
		 */
		if (!glob)
			break;

		/*
		 * Search from next character when the matching text is zero width.
		 */
		if (pmatch[0].rm_so == pmatch[0].rm_eo)
			search_start++;
	}

	/*
	 * Copy the text to the right of the last match.
	 */
	if (data_pos < data_len)
	{
		text	   *right_text;

		right_text = text_substring(PointerGetDatum(src_text),
									data_pos + 1, -1, true);
		appendStringInfoText(str, right_text);
		pfree(right_text);
	}

	ret_text = PG_STR_GET_TEXT(str->data);
	pfree(str->data);
	pfree(str);
	pfree(data);

	return ret_text;
}

/*
 * split_text
 * parse input string
 * return ord item (1 based)
 * based on provided field separator
 */
Datum
split_text(PG_FUNCTION_ARGS)
{
	text	   *inputstring = PG_GETARG_TEXT_P(0);
	text	   *fldsep = PG_GETARG_TEXT_P(1);
	int			fldnum = PG_GETARG_INT32(2);
	int			inputstring_len = TEXTLEN(inputstring);
	int			fldsep_len = TEXTLEN(fldsep);
	int			start_posn;
	int			end_posn;
	text	   *result_text;

	/* field number is 1 based */
	if (fldnum < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field position must be greater than zero")));

	/* return empty string for empty input string */
	if (inputstring_len < 1)
		PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));

	/* empty field separator */
	if (fldsep_len < 1)
	{
		/* if first field, return input string, else empty string */
		if (fldnum == 1)
			PG_RETURN_TEXT_P(inputstring);
		else
			PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));
	}

	start_posn = text_position(inputstring, fldsep, fldnum - 1);
	end_posn = text_position(inputstring, fldsep, fldnum);

	if ((start_posn == 0) && (end_posn == 0))	/* fldsep not found */
	{
		/* if first field, return input string, else empty string */
		if (fldnum == 1)
			PG_RETURN_TEXT_P(inputstring);
		else
			PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));
	}
	else if (start_posn == 0)
	{
		/* first field requested */
		result_text = LEFT(inputstring, fldsep);
		PG_RETURN_TEXT_P(result_text);
	}
	else if (end_posn == 0)
	{
		/* last field requested */
		result_text = text_substring(PointerGetDatum(inputstring),
									 start_posn + fldsep_len,
									 -1, true);
		PG_RETURN_TEXT_P(result_text);
	}
	else
	{
		/* interior field requested */
		result_text = text_substring(PointerGetDatum(inputstring),
									 start_posn + fldsep_len,
									 end_posn - start_posn - fldsep_len,
									 false);
		PG_RETURN_TEXT_P(result_text);
	}
}

/*
 * text_to_array
 * parse input string
 * return text array of elements
 * based on provided field separator
 */
Datum
text_to_array(PG_FUNCTION_ARGS)
{
	text	   *inputstring = PG_GETARG_TEXT_P(0);
	text	   *fldsep = PG_GETARG_TEXT_P(1);
	int			inputstring_len = TEXTLEN(inputstring);
	int			fldsep_len = TEXTLEN(fldsep);
	int			fldnum;
	int			start_posn;
	int			end_posn;
	text	   *result_text;
	ArrayBuildState *astate = NULL;

	/* return NULL for empty input string */
	if (inputstring_len < 1)
		PG_RETURN_NULL();

	/*
	 * empty field separator return one element, 1D, array using the input
	 * string
	 */
	if (fldsep_len < 1)
		PG_RETURN_ARRAYTYPE_P(create_singleton_array(fcinfo, TEXTOID,
										   CStringGetDatum(inputstring), 1));

	/* start with end position holding the initial start position */
	end_posn = 0;
	for (fldnum = 1;; fldnum++) /* field number is 1 based */
	{
		Datum		dvalue;
		bool		disnull = false;

		start_posn = end_posn;
		end_posn = text_position(inputstring, fldsep, fldnum);

		if ((start_posn == 0) && (end_posn == 0))		/* fldsep not found */
		{
			if (fldnum == 1)
			{
				/*
				 * first element return one element, 1D, array using the input
				 * string
				 */
				PG_RETURN_ARRAYTYPE_P(create_singleton_array(fcinfo, TEXTOID,
										   CStringGetDatum(inputstring), 1));
			}
			else
			{
				/* otherwise create array and exit */
				PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate,
													  CurrentMemoryContext));
			}
		}
		else if (start_posn == 0)
		{
			/* first field requested */
			result_text = LEFT(inputstring, fldsep);
		}
		else if (end_posn == 0)
		{
			/* last field requested */
			result_text = text_substring(PointerGetDatum(inputstring),
										 start_posn + fldsep_len,
										 -1, true);
		}
		else
		{
			/* interior field requested */
			result_text = text_substring(PointerGetDatum(inputstring),
										 start_posn + fldsep_len,
										 end_posn - start_posn - fldsep_len,
										 false);
		}

		/* stash away current value */
		dvalue = PointerGetDatum(result_text);
		astate = accumArrayResult(astate, dvalue,
								  disnull, TEXTOID,
								  CurrentMemoryContext);
	}

	/* never reached -- keep compiler quiet */
	PG_RETURN_NULL();
}

/*
 * array_to_text
 * concatenate Cstring representation of input array elements
 * using provided field separator
 */
Datum
array_to_text(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	char	   *fldsep = PG_TEXTARG_GET_STR(1);
	int			nitems,
			   *dims,
				ndims;
	char	   *p;
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	StringInfo	result_str = makeStringInfo();
	int			i;
	ArrayMetaState *my_extra;

	p = ARR_DATA_PTR(v);
	ndims = ARR_NDIM(v);
	dims = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndims, dims);

	/* if there are no elements, return an empty string */
	if (nitems == 0)
		PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));

	element_type = ARR_ELEMTYPE(v);

	/*
	 * We arrange to look up info about element type, including its output
	 * conversion proc, only once per series of calls, assuming the element
	 * type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = InvalidOid;
	}

	if (my_extra->element_type != element_type)
	{
		/*
		 * Get info about element type, including its output conversion proc
		 */
		get_type_io_data(element_type, IOFunc_output,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		char	   *value;

		itemvalue = fetch_att(p, typbyval, typlen);

		value = DatumGetCString(FunctionCall1(&my_extra->proc,
											  itemvalue));

		if (i > 0)
			appendStringInfo(result_str, "%s%s", fldsep, value);
		else
			appendStringInfoString(result_str, value);

		p = att_addlength(p, typlen, PointerGetDatum(p));
		p = (char *) att_align(p, typalign);
	}

	PG_RETURN_TEXT_P(PG_STR_GET_TEXT(result_str->data));
}

#define HEXBASE 16
/*
 * Convert a int32 to a string containing a base 16 (hex) representation of
 * the number.
 */
Datum
to_hex32(PG_FUNCTION_ARGS)
{
	uint32		value = (uint32) PG_GETARG_INT32(0);
	text	   *result_text;
	char	   *ptr;
	const char *digits = "0123456789abcdef";
	char		buf[32];		/* bigger than needed, but reasonable */

	ptr = buf + sizeof(buf) - 1;
	*ptr = '\0';

	do
	{
		*--ptr = digits[value % HEXBASE];
		value /= HEXBASE;
	} while (ptr > buf && value);

	result_text = PG_STR_GET_TEXT(ptr);
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Convert a int64 to a string containing a base 16 (hex) representation of
 * the number.
 */
Datum
to_hex64(PG_FUNCTION_ARGS)
{
	uint64		value = (uint64) PG_GETARG_INT64(0);
	text	   *result_text;
	char	   *ptr;
	const char *digits = "0123456789abcdef";
	char		buf[32];		/* bigger than needed, but reasonable */

	ptr = buf + sizeof(buf) - 1;
	*ptr = '\0';

	do
	{
		*--ptr = digits[value % HEXBASE];
		value /= HEXBASE;
	} while (ptr > buf && value);

	result_text = PG_STR_GET_TEXT(ptr);
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Create an md5 hash of a text string and return it as hex
 *
 * md5 produces a 16 byte (128 bit) hash; double it for hex
 */
#define MD5_HASH_LEN  32

Datum
md5_text(PG_FUNCTION_ARGS)
{
	text	   *in_text = PG_GETARG_TEXT_P(0);
	size_t		len;
	char		hexsum[MD5_HASH_LEN + 1];
	text	   *result_text;

	/* Calculate the length of the buffer using varlena metadata */
	len = VARSIZE(in_text) - VARHDRSZ;

	/* get the hash result */
	if (pg_md5_hash(VARDATA(in_text), len, hexsum) == false)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* convert to text and return it */
	result_text = PG_STR_GET_TEXT(hexsum);
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Create an md5 hash of a bytea field and return it as a hex string:
 * 16-byte md5 digest is represented in 32 hex characters.
 */
Datum
md5_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_P(0);
	size_t		len;
	char		hexsum[MD5_HASH_LEN + 1];
	text	   *result_text;

	len = VARSIZE(in) - VARHDRSZ;
	if (pg_md5_hash(VARDATA(in), len, hexsum) == false)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	result_text = PG_STR_GET_TEXT(hexsum);
	PG_RETURN_TEXT_P(result_text);
}

/*
 * Return the size of a datum, possibly compressed
 *
 * Works on any data type
 */
Datum
pg_column_size(PG_FUNCTION_ARGS)
{
	Datum		value = PG_GETARG_DATUM(0);
	int32		result;
	int			typlen;

	/* On first call, get the input type's typlen, and save at *fn_extra */
	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/* Lookup the datatype of the supplied argument */
		Oid			argtypeid = get_fn_expr_argtype(fcinfo->flinfo, 0);

		typlen = get_typlen(argtypeid);
		if (typlen == 0)		/* should not happen */
			elog(ERROR, "cache lookup failed for type %u", argtypeid);

		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(int));
		*((int *) fcinfo->flinfo->fn_extra) = typlen;
	}
	else
		typlen = *((int *) fcinfo->flinfo->fn_extra);

	if (typlen == -1)
	{
		/* varlena type, possibly toasted */
		result = toast_datum_size(value);
	}
	else if (typlen == -2)
	{
		/* cstring */
		result = strlen(DatumGetCString(value)) + 1;
	}
	else
	{
		/* ordinary fixed-width type */
		result = typlen;
	}

	PG_RETURN_INT32(result);
}
