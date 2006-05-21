/*-------------------------------------------------------------------------
 *
 * varlena.c
 *	  Functions for the variable-length built-in types.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/varlena.c,v 1.106.2.6 2006/05/21 20:06:43 tgl Exp $
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
	text_position(PointerGetDatum(buf_text), PointerGetDatum(from_sub_text), 1)
#define TEXTDUP(textp) \
	DatumGetTextPCopy(PointerGetDatum(textp))
#define LEFT(buf_text, from_sub_text) \
	text_substring(PointerGetDatum(buf_text), \
					1, \
					TEXTPOS(buf_text, from_sub_text) - 1, false)
#define RIGHT(buf_text, from_sub_text, from_sub_text_len) \
	text_substring(PointerGetDatum(buf_text), \
					TEXTPOS(buf_text, from_sub_text) + from_sub_text_len, \
					-1, true)

static int	text_cmp(text *arg1, text *arg2);
static int32 text_length(Datum str);
static int32 text_position(Datum str, Datum search_str, int matchnum);
static text *text_substring(Datum str,
			   int32 start,
			   int32 length,
			   bool length_not_specified);


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
			 * We should never get here. The first pass should not allow
			 * it.
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
	char	   *inputStr = PG_GETARG_CSTRING(0);
	unknown    *result;
	int			len;

	len = strlen(inputStr) + VARHDRSZ;

	result = (unknown *) palloc(len);
	VARATT_SIZEP(result) = len;

	memcpy(VARDATA(result), inputStr, len - VARHDRSZ);

	PG_RETURN_UNKNOWN_P(result);
}

/*
 *		unknownout			- converts internal representation to "..."
 */
Datum
unknownout(PG_FUNCTION_ARGS)
{
	unknown    *t = PG_GETARG_UNKNOWN_P(0);
	int			len;
	char	   *result;

	len = VARSIZE(t) - VARHDRSZ;
	result = (char *) palloc(len + 1);
	memcpy(result, VARDATA(t), len);
	result[len] = '\0';

	PG_RETURN_CSTRING(result);
}

/*
 *		unknownrecv			- converts external binary format to unknown
 */
Datum
unknownrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	unknown    *result;
	int			nbytes;

	nbytes = buf->len - buf->cursor;
	result = (unknown *) palloc(nbytes + VARHDRSZ);
	VARATT_SIZEP(result) = nbytes + VARHDRSZ;
	pq_copymsgbytes(buf, VARDATA(result), nbytes);
	PG_RETURN_UNKNOWN_P(result);
}

/*
 *		unknownsend			- converts unknown to binary format
 *
 * This is a special case: just copy the input, since it's
 * effectively the same format as bytea
 */
Datum
unknownsend(PG_FUNCTION_ARGS)
{
	unknown    *vlena = PG_GETARG_UNKNOWN_P_COPY(0);

	PG_RETURN_UNKNOWN_P(vlena);
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
	PG_RETURN_INT32(text_length(PG_GETARG_DATUM(0)));
}

/*
 * text_length -
 *	Does the real work for textlen()
 *	This is broken out so it can be called directly by other string processing
 *	functions.
 */
static int32
text_length(Datum str)
{
	/* fastpath when max encoding length is one */
	if (pg_database_encoding_max_length() == 1)
		PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);

	if (pg_database_encoding_max_length() > 1)
	{
		text	   *t = DatumGetTextP(str);

		PG_RETURN_INT32(pg_mbstrlen_with_len(VARDATA(t),
											 VARSIZE(t) - VARHDRSZ));
	}

	/* should never get here */
	elog(ERROR, "invalid backend encoding: encoding max length < 1");

	/* not reached: suppress compiler warning */
	return 0;
}

/*
 * textoctetlen -
 *	  returns the physical length of a text*
 *	   (which is less than the VARSIZE of the text*)
 */
Datum
textoctetlen(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(toast_raw_datum_size(PG_GETARG_DATUM(0)) - VARHDRSZ);
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

	len1 = (VARSIZE(t1) - VARHDRSZ);
	if (len1 < 0)
		len1 = 0;

	len2 = (VARSIZE(t2) - VARHDRSZ);
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
 * Note that the arguments operate on octet length,
 *	so not aware of multibyte character sets.
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
 *	This is broken out so it can be called directly by other string processing
 *	functions.
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

		if (length_not_specified)		/* special case - get length to
										 * end of string */
			L1 = -1;
		else
		{
			/* end position */
			int			E = S + length;

			/*
			 * A negative value for L is the only way for the end position
			 * to be before the start. SQL99 says to throw an error.
			 */
			if (E < S)
				ereport(ERROR,
						(errcode(ERRCODE_SUBSTRING_ERROR),
					   errmsg("negative substring length not allowed")));

			/*
			 * A zero or negative value for the end position can happen if
			 * the start was negative or one. SQL99 says to return a
			 * zero-length string.
			 */
			if (E < 1)
				return PG_STR_GET_TEXT("");

			L1 = E - S1;
		}

		/*
		 * If the start position is past the end of the string, SQL99 says
		 * to return a zero-length string -- PG_GETARG_TEXT_P_SLICE() will
		 * do that for us. Convert to zero-based starting position
		 */
		return DatumGetTextPSlice(str, S1 - 1, L1);
	}
	else if (eml > 1)
	{
		/*
		 * When encoding max length is > 1, we can't get LC without
		 * detoasting, so we'll grab a conservatively large slice now and
		 * go back later to do the right thing
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
		 * if S is past the end of the string, the tuple toaster will
		 * return a zero-length string to us
		 */
		S1 = Max(S, 1);

		/*
		 * We need to start at position zero because there is no way to
		 * know in advance which byte offset corresponds to the supplied
		 * start position.
		 */
		slice_start = 0;

		if (length_not_specified)		/* special case - get length to
										 * end of string */
			slice_size = L1 = -1;
		else
		{
			int			E = S + length;

			/*
			 * A negative value for L is the only way for the end position
			 * to be before the start. SQL99 says to throw an error.
			 */
			if (E < S)
				ereport(ERROR,
						(errcode(ERRCODE_SUBSTRING_ERROR),
					   errmsg("negative substring length not allowed")));

			/*
			 * A zero or negative value for the end position can happen if
			 * the start was negative or one. SQL99 says to return a
			 * zero-length string.
			 */
			if (E < 1)
				return PG_STR_GET_TEXT("");

			/*
			 * if E is past the end of the string, the tuple toaster will
			 * truncate the length for us
			 */
			L1 = E - S1;

			/*
			 * Total slice size in bytes can't be any longer than the
			 * start position plus substring length times the encoding max
			 * length.
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
		 * Check that the start position wasn't > slice_strlen. If so,
		 * SQL99 says to return a zero-length string.
		 */
		if (S1 > slice_strlen)
			return PG_STR_GET_TEXT("");

		/*
		 * Adjust L1 and E1 now that we know the slice string length.
		 * Again remember that S1 is one based, and slice_start is zero
		 * based.
		 */
		if (L1 > -1)
			E1 = Min(S1 + L1, slice_start + 1 + slice_strlen);
		else
			E1 = slice_start + 1 + slice_strlen;

		/*
		 * Find the start position in the slice; remember S1 is not zero
		 * based
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
	return PG_STR_GET_TEXT("");
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
	PG_RETURN_INT32(text_position(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), 1));
}

/*
 * text_position -
 *	Does the real work for textpos()
 *	This is broken out so it can be called directly by other string processing
 *	functions.
 */
static int32
text_position(Datum str, Datum search_str, int matchnum)
{
	int			eml = pg_database_encoding_max_length();
	text	   *t1 = DatumGetTextP(str);
	text	   *t2 = DatumGetTextP(search_str);
	int			match = 0,
				pos = 0,
				p = 0,
				px,
				len1,
				len2;

	if (matchnum == 0)
		return 0;				/* result for 0th match */

	if (VARSIZE(t2) <= VARHDRSZ)
		PG_RETURN_INT32(1);		/* result for empty pattern */

	len1 = (VARSIZE(t1) - VARHDRSZ);
	len2 = (VARSIZE(t2) - VARHDRSZ);

	if (eml == 1)				/* simple case - single byte encoding */
	{
		char	   *p1,
				   *p2;

		p1 = VARDATA(t1);
		p2 = VARDATA(t2);

		/* no use in searching str past point where search_str will fit */
		px = (len1 - len2);

		for (p = 0; p <= px; p++)
		{
			if ((*p2 == *p1) && (strncmp(p1, p2, len2) == 0))
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
	else if (eml > 1)			/* not as simple - multibyte encoding */
	{
		pg_wchar   *p1,
				   *p2,
				   *ps1,
				   *ps2;

		ps1 = p1 = (pg_wchar *) palloc((len1 + 1) * sizeof(pg_wchar));
		(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(t1), p1, len1);
		len1 = pg_wchar_strlen(p1);
		ps2 = p2 = (pg_wchar *) palloc((len2 + 1) * sizeof(pg_wchar));
		(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(t2), p2, len2);
		len2 = pg_wchar_strlen(p2);

		/* no use in searching str past point where search_str will fit */
		px = (len1 - len2);

		for (p = 0; p <= px; p++)
		{
			if ((*p2 == *p1) && (pg_wchar_strncmp(p1, p2, len2) == 0))
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
	else
		elog(ERROR, "invalid backend encoding: encoding max length < 1");

	PG_RETURN_INT32(pos);
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
	 * Unfortunately, there is no strncoll(), so in the non-C locale case
	 * we have to do some memory copying.  This turns out to be
	 * significantly slower, so we optimize the case where LC_COLLATE is
	 * C.  We also try to optimize relatively-short strings by avoiding
	 * palloc/pfree overhead.
	 */
#define STACKBUFLEN		1024

	if (!lc_collate_is_c())
	{
		char		a1buf[STACKBUFLEN];
		char		a2buf[STACKBUFLEN];
		char	   *a1p,
				   *a2p;

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

		if (len1 >= STACKBUFLEN)
			pfree(a1p);
		if (len2 >= STACKBUFLEN)
			pfree(a2p);
	}
	else
	{
		result = strncmp(arg1, arg2, Min(len1, len2));
		if ((result == 0) && (len1 != len2))
			result = (len1 < len2) ? -1 : 1;
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
	PG_RETURN_INT32(toast_raw_datum_size(PG_GETARG_DATUM(0)) - VARHDRSZ);
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

	len1 = (VARSIZE(t1) - VARHDRSZ);
	if (len1 < 0)
		len1 = 0;

	len2 = (VARSIZE(t2) - VARHDRSZ);
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
		 * Not passed a length - PG_GETARG_BYTEA_P_SLICE() grabs
		 * everything to the end of the string if we pass it a negative
		 * value for length.
		 */
		L1 = -1;
	}
	else
	{
		/* end position */
		int			E = S + PG_GETARG_INT32(2);

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
			PG_RETURN_BYTEA_P(PG_STR_GET_BYTEA(""));

		L1 = E - S1;
	}

	/*
	 * If the start position is past the end of the string, SQL99 says to
	 * return a zero-length string -- PG_GETARG_TEXT_P_SLICE() will do
	 * that for us. Convert to zero-based starting position
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

	len1 = (VARSIZE(t1) - VARHDRSZ);
	len2 = (VARSIZE(t2) - VARHDRSZ);

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
textToQualifiedNameList(text *textval, const char *caller)
{
	char	   *rawname;
	List	   *result = NIL;
	List	   *namelist;
	List	   *l;

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
	freeList(namelist);

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
 *			  rawstring.  Caller should freeList() this even on error return.
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
			 * support a downcasing transformation that increases the
			 * string length.  This is not a problem given the current
			 * implementation of downcase_truncate_identifier, but we'll
			 * probably have to do something about this someday.
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
	text	   *left_text;
	text	   *right_text;
	text	   *buf_text;
	text	   *ret_text;
	int			curr_posn;
	text	   *src_text = PG_GETARG_TEXT_P(0);
	int			src_text_len = TEXTLEN(src_text);
	text	   *from_sub_text = PG_GETARG_TEXT_P(1);
	int			from_sub_text_len = TEXTLEN(from_sub_text);
	text	   *to_sub_text = PG_GETARG_TEXT_P(2);
	char	   *to_sub_str = PG_TEXT_GET_STR(to_sub_text);
	StringInfo	str = makeStringInfo();

	if (src_text_len == 0 || from_sub_text_len == 0)
		PG_RETURN_TEXT_P(src_text);

	buf_text = TEXTDUP(src_text);
	curr_posn = TEXTPOS(buf_text, from_sub_text);

	while (curr_posn > 0)
	{
		left_text = LEFT(buf_text, from_sub_text);
		right_text = RIGHT(buf_text, from_sub_text, from_sub_text_len);

		appendStringInfoString(str, PG_TEXT_GET_STR(left_text));
		appendStringInfoString(str, to_sub_str);

		pfree(buf_text);
		pfree(left_text);
		buf_text = right_text;
		curr_posn = TEXTPOS(buf_text, from_sub_text);
	}

	appendStringInfoString(str, PG_TEXT_GET_STR(buf_text));
	pfree(buf_text);

	ret_text = PG_STR_GET_TEXT(str->data);
	pfree(str->data);
	pfree(str);

	PG_RETURN_TEXT_P(ret_text);
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
	int			inputstring_len = TEXTLEN(inputstring);
	text	   *fldsep = PG_GETARG_TEXT_P(1);
	int			fldsep_len = TEXTLEN(fldsep);
	int			fldnum = PG_GETARG_INT32(2);
	int			start_posn = 0;
	int			end_posn = 0;
	text	   *result_text;

	/* return empty string for empty input string */
	if (inputstring_len < 1)
		PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));

	/* empty field separator */
	if (fldsep_len < 1)
	{
		if (fldnum == 1)		/* first field - just return the input
								 * string */
			PG_RETURN_TEXT_P(inputstring);
		else
/* otherwise return an empty string */
			PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));
	}

	/* field number is 1 based */
	if (fldnum < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field position must be greater than zero")));

	start_posn = text_position(PointerGetDatum(inputstring),
							   PointerGetDatum(fldsep),
							   fldnum - 1);
	end_posn = text_position(PointerGetDatum(inputstring),
							 PointerGetDatum(fldsep),
							 fldnum);

	if ((start_posn == 0) && (end_posn == 0))	/* fldsep not found */
	{
		if (fldnum == 1)		/* first field - just return the input
								 * string */
			PG_RETURN_TEXT_P(inputstring);
		else
/* otherwise return an empty string */
			PG_RETURN_TEXT_P(PG_STR_GET_TEXT(""));
	}
	else if ((start_posn != 0) && (end_posn == 0))
	{
		/* last field requested */
		result_text = text_substring(PointerGetDatum(inputstring), start_posn + fldsep_len, -1, true);
		PG_RETURN_TEXT_P(result_text);
	}
	else if ((start_posn == 0) && (end_posn != 0))
	{
		/* first field requested */
		result_text = LEFT(inputstring, fldsep);
		PG_RETURN_TEXT_P(result_text);
	}
	else
	{
		/* prior to last field requested */
		result_text = text_substring(PointerGetDatum(inputstring), start_posn + fldsep_len, end_posn - start_posn - fldsep_len, false);
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
	int			inputstring_len = TEXTLEN(inputstring);
	text	   *fldsep = PG_GETARG_TEXT_P(1);
	int			fldsep_len = TEXTLEN(fldsep);
	int			fldnum;
	int			start_posn = 0;
	int			end_posn = 0;
	text	   *result_text = NULL;
	ArrayBuildState *astate = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;

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
		end_posn = text_position(PointerGetDatum(inputstring),
								 PointerGetDatum(fldsep),
								 fldnum);

		if ((start_posn == 0) && (end_posn == 0))		/* fldsep not found */
		{
			if (fldnum == 1)
			{
				/*
				 * first element return one element, 1D, array using the
				 * input string
				 */
				PG_RETURN_ARRAYTYPE_P(create_singleton_array(fcinfo, TEXTOID,
									   CStringGetDatum(inputstring), 1));
			}
			else
			{
				/* otherwise create array and exit */
				PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate, oldcontext));
			}
		}
		else if ((start_posn != 0) && (end_posn == 0))
		{
			/* last field requested */
			result_text = text_substring(PointerGetDatum(inputstring), start_posn + fldsep_len, -1, true);
		}
		else if ((start_posn == 0) && (end_posn != 0))
		{
			/* first field requested */
			result_text = LEFT(inputstring, fldsep);
		}
		else
		{
			/* prior to last field requested */
			result_text = text_substring(PointerGetDatum(inputstring), start_posn + fldsep_len, end_posn - start_posn - fldsep_len, false);
		}

		/* stash away current value */
		dvalue = PointerGetDatum(result_text);
		astate = accumArrayResult(astate, dvalue,
								  disnull, TEXTOID, oldcontext);

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
	Oid			typelem;
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
	 * conversion proc, only once per series of calls, assuming the
	 * element type doesn't change underneath us.
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
		 * Get info about element type, including its output conversion
		 * proc
		 */
		get_type_io_data(element_type, IOFunc_output,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typelem, &my_extra->typiofunc);
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typelem = my_extra->typelem;

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		char	   *value;

		itemvalue = fetch_att(p, typbyval, typlen);

		value = DatumGetCString(FunctionCall3(&my_extra->proc,
											  itemvalue,
											  ObjectIdGetDatum(typelem),
											  Int32GetDatum(-1)));

		if (i > 0)
			appendStringInfo(result_str, "%s%s", fldsep, value);
		else
			appendStringInfo(result_str, "%s", value);

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
	char	   *buff = PG_TEXT_GET_STR(PG_GETARG_TEXT_P(0));
	size_t		len = strlen(buff);
	char	   *hexsum;
	text	   *result_text;

	/* leave room for the terminating '\0' */
	hexsum = (char *) palloc(MD5_HASH_LEN + 1);

	/* get the hash result */
	md5_hash((void *) buff, len, hexsum);

	/* convert to text and return it */
	result_text = PG_STR_GET_TEXT(hexsum);
	PG_RETURN_TEXT_P(result_text);
}
