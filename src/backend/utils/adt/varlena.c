/*-------------------------------------------------------------------------
 *
 * varlena.c
 *	  Functions for the variable-length built-in types.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varlena.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "access/detoast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/unicode_norm.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "parser/scansup.h"
#include "port/pg_bswap.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/sortsupport.h"
#include "utils/varlena.h"


/* GUC variable */
int			bytea_output = BYTEA_OUTPUT_HEX;

typedef struct varlena unknown;
typedef struct varlena VarString;

/*
 * State for text_position_* functions.
 */
typedef struct
{
	bool		is_multibyte;	/* T if multibyte encoding */
	bool		is_multibyte_char_in_char;	/* need to check char boundaries? */

	char	   *str1;			/* haystack string */
	char	   *str2;			/* needle string */
	int			len1;			/* string lengths in bytes */
	int			len2;

	/* Skip table for Boyer-Moore-Horspool search algorithm: */
	int			skiptablemask;	/* mask for ANDing with skiptable subscripts */
	int			skiptable[256]; /* skip distance for given mismatched char */

	char	   *last_match;		/* pointer to last match in 'str1' */

	/*
	 * Sometimes we need to convert the byte position of a match to a
	 * character position.  These store the last position that was converted,
	 * so that on the next call, we can continue from that point, rather than
	 * count characters from the very beginning.
	 */
	char	   *refpoint;		/* pointer within original haystack string */
	int			refpos;			/* 0-based character offset of the same point */
} TextPositionState;

typedef struct
{
	char	   *buf1;			/* 1st string, or abbreviation original string
								 * buf */
	char	   *buf2;			/* 2nd string, or abbreviation strxfrm() buf */
	int			buflen1;
	int			buflen2;
	int			last_len1;		/* Length of last buf1 string/strxfrm() input */
	int			last_len2;		/* Length of last buf2 string/strxfrm() blob */
	int			last_returned;	/* Last comparison result (cache) */
	bool		cache_blob;		/* Does buf2 contain strxfrm() blob, etc? */
	bool		collate_c;
	Oid			typid;			/* Actual datatype (text/bpchar/bytea/name) */
	hyperLogLogState abbr_card; /* Abbreviated key cardinality state */
	hyperLogLogState full_card; /* Full key cardinality state */
	double		prop_card;		/* Required cardinality proportion */
	pg_locale_t locale;
} VarStringSortSupport;

/*
 * Output data for split_text(): we output either to an array or a table.
 * tupstore and tupdesc must be set up in advance to output to a table.
 */
typedef struct
{
	ArrayBuildState *astate;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
} SplitTextOutputData;

/*
 * This should be large enough that most strings will fit, but small enough
 * that we feel comfortable putting it on the stack
 */
#define TEXTBUFLEN		1024

#define DatumGetUnknownP(X)			((unknown *) PG_DETOAST_DATUM(X))
#define DatumGetUnknownPCopy(X)		((unknown *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_UNKNOWN_P(n)		DatumGetUnknownP(PG_GETARG_DATUM(n))
#define PG_GETARG_UNKNOWN_P_COPY(n) DatumGetUnknownPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_UNKNOWN_P(x)		PG_RETURN_POINTER(x)

#define DatumGetVarStringP(X)		((VarString *) PG_DETOAST_DATUM(X))
#define DatumGetVarStringPP(X)		((VarString *) PG_DETOAST_DATUM_PACKED(X))

static int	varstrfastcmp_c(Datum x, Datum y, SortSupport ssup);
static int	bpcharfastcmp_c(Datum x, Datum y, SortSupport ssup);
static int	namefastcmp_c(Datum x, Datum y, SortSupport ssup);
static int	varlenafastcmp_locale(Datum x, Datum y, SortSupport ssup);
static int	namefastcmp_locale(Datum x, Datum y, SortSupport ssup);
static int	varstrfastcmp_locale(char *a1p, int len1, char *a2p, int len2, SortSupport ssup);
static int	varstrcmp_abbrev(Datum x, Datum y, SortSupport ssup);
static Datum varstr_abbrev_convert(Datum original, SortSupport ssup);
static bool varstr_abbrev_abort(int memtupcount, SortSupport ssup);
static int32 text_length(Datum str);
static text *text_catenate(text *t1, text *t2);
static text *text_substring(Datum str,
							int32 start,
							int32 length,
							bool length_not_specified);
static text *text_overlay(text *t1, text *t2, int sp, int sl);
static int	text_position(text *t1, text *t2, Oid collid);
static void text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state);
static bool text_position_next(TextPositionState *state);
static char *text_position_next_internal(char *start_ptr, TextPositionState *state);
static char *text_position_get_match_ptr(TextPositionState *state);
static int	text_position_get_match_pos(TextPositionState *state);
static void text_position_cleanup(TextPositionState *state);
static void check_collation_set(Oid collid);
static int	text_cmp(text *arg1, text *arg2, Oid collid);
static bytea *bytea_catenate(bytea *t1, bytea *t2);
static bytea *bytea_substring(Datum str,
							  int S,
							  int L,
							  bool length_not_specified);
static bytea *bytea_overlay(bytea *t1, bytea *t2, int sp, int sl);
static void appendStringInfoText(StringInfo str, const text *t);
static bool split_text(FunctionCallInfo fcinfo, SplitTextOutputData *tstate);
static void split_text_accum_result(SplitTextOutputData *tstate,
									text *field_value,
									text *null_string,
									Oid collation);
static text *array_to_text_internal(FunctionCallInfo fcinfo, ArrayType *v,
									const char *fldsep, const char *null_string);
static StringInfo makeStringAggState(FunctionCallInfo fcinfo);
static bool text_format_parse_digits(const char **ptr, const char *end_ptr,
									 int *value);
static const char *text_format_parse_format(const char *start_ptr,
											const char *end_ptr,
											int *argpos, int *widthpos,
											int *flags, int *width);
static void text_format_string_conversion(StringInfo buf, char conversion,
										  FmgrInfo *typOutputInfo,
										  Datum value, bool isNull,
										  int flags, int width);
static void text_format_append_string(StringInfo buf, const char *str,
									  int flags, int width);


/*****************************************************************************
 *	 CONVERSION ROUTINES EXPORTED FOR USE BY C CODE							 *
 *****************************************************************************/

/*
 * cstring_to_text
 *
 * Create a text value from a null-terminated C string.
 *
 * The new text value is freshly palloc'd with a full-size VARHDR.
 */
text *
cstring_to_text(const char *s)
{
	return cstring_to_text_with_len(s, strlen(s));
}

/*
 * cstring_to_text_with_len
 *
 * Same as cstring_to_text except the caller specifies the string length;
 * the string need not be null_terminated.
 */
text *
cstring_to_text_with_len(const char *s, int len)
{
	text	   *result = (text *) palloc(len + VARHDRSZ);

	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), s, len);

	return result;
}

/*
 * text_to_cstring
 *
 * Create a palloc'd, null-terminated C string from a text value.
 *
 * We support being passed a compressed or toasted text value.
 * This is a bit bogus since such values shouldn't really be referred to as
 * "text *", but it seems useful for robustness.  If we didn't handle that
 * case here, we'd need another routine that did, anyway.
 */
char *
text_to_cstring(const text *t)
{
	/* must cast away the const, unfortunately */
	text	   *tunpacked = pg_detoast_datum_packed(unconstify(text *, t));
	int			len = VARSIZE_ANY_EXHDR(tunpacked);
	char	   *result;

	result = (char *) palloc(len + 1);
	memcpy(result, VARDATA_ANY(tunpacked), len);
	result[len] = '\0';

	if (tunpacked != t)
		pfree(tunpacked);

	return result;
}

/*
 * text_to_cstring_buffer
 *
 * Copy a text value into a caller-supplied buffer of size dst_len.
 *
 * The text string is truncated if necessary to fit.  The result is
 * guaranteed null-terminated (unless dst_len == 0).
 *
 * We support being passed a compressed or toasted text value.
 * This is a bit bogus since such values shouldn't really be referred to as
 * "text *", but it seems useful for robustness.  If we didn't handle that
 * case here, we'd need another routine that did, anyway.
 */
void
text_to_cstring_buffer(const text *src, char *dst, size_t dst_len)
{
	/* must cast away the const, unfortunately */
	text	   *srcunpacked = pg_detoast_datum_packed(unconstify(text *, src));
	size_t		src_len = VARSIZE_ANY_EXHDR(srcunpacked);

	if (dst_len > 0)
	{
		dst_len--;
		if (dst_len >= src_len)
			dst_len = src_len;
		else					/* ensure truncation is encoding-safe */
			dst_len = pg_mbcliplen(VARDATA_ANY(srcunpacked), src_len, dst_len);
		memcpy(dst, VARDATA_ANY(srcunpacked), dst_len);
		dst[dst_len] = '\0';
	}

	if (srcunpacked != src)
		pfree(srcunpacked);
}


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
 *				The input is scanned twice.
 *				The error checking of input is minimal.
 */
Datum
byteain(PG_FUNCTION_ARGS)
{
	char	   *inputText = PG_GETARG_CSTRING(0);
	char	   *tp;
	char	   *rp;
	int			bc;
	bytea	   *result;

	/* Recognize hex input */
	if (inputText[0] == '\\' && inputText[1] == 'x')
	{
		size_t		len = strlen(inputText);

		bc = (len - 2) / 2 + VARHDRSZ;	/* maximum possible length */
		result = palloc(bc);
		bc = hex_decode(inputText + 2, len - 2, VARDATA(result));
		SET_VARSIZE(result, bc + VARHDRSZ); /* actual length */

		PG_RETURN_BYTEA_P(result);
	}

	/* Else, it's the traditional escaped style */
	for (bc = 0, tp = inputText; *tp != '\0'; bc++)
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
			 * one backslash, not followed by another or ### valid octal
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}
	}

	bc += VARHDRSZ;

	result = (bytea *) palloc(bc);
	SET_VARSIZE(result, bc);

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
			bc = VAL(tp[1]);
			bc <<= 3;
			bc += VAL(tp[2]);
			bc <<= 3;
			*rp++ = bc + VAL(tp[3]);

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
					 errmsg("invalid input syntax for type %s", "bytea")));
		}
	}

	PG_RETURN_BYTEA_P(result);
}

/*
 *		byteaout		- converts to printable representation of byte array
 *
 *		In the traditional escaped format, non-printable characters are
 *		printed as '\nnn' (octal) and '\' as '\\'.
 */
Datum
byteaout(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_PP(0);
	char	   *result;
	char	   *rp;

	if (bytea_output == BYTEA_OUTPUT_HEX)
	{
		/* Print hex format */
		rp = result = palloc(VARSIZE_ANY_EXHDR(vlena) * 2 + 2 + 1);
		*rp++ = '\\';
		*rp++ = 'x';
		rp += hex_encode(VARDATA_ANY(vlena), VARSIZE_ANY_EXHDR(vlena), rp);
	}
	else if (bytea_output == BYTEA_OUTPUT_ESCAPE)
	{
		/* Print traditional escaped format */
		char	   *vp;
		uint64		len;
		int			i;

		len = 1;				/* empty string has 1 char */
		vp = VARDATA_ANY(vlena);
		for (i = VARSIZE_ANY_EXHDR(vlena); i != 0; i--, vp++)
		{
			if (*vp == '\\')
				len += 2;
			else if ((unsigned char) *vp < 0x20 || (unsigned char) *vp > 0x7e)
				len += 4;
			else
				len++;
		}

		/*
		 * In principle len can't overflow uint32 if the input fit in 1GB, but
		 * for safety let's check rather than relying on palloc's internal
		 * check.
		 */
		if (len > MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg_internal("result of bytea output conversion is too large")));
		rp = result = (char *) palloc(len);

		vp = VARDATA_ANY(vlena);
		for (i = VARSIZE_ANY_EXHDR(vlena); i != 0; i--, vp++)
		{
			if (*vp == '\\')
			{
				*rp++ = '\\';
				*rp++ = '\\';
			}
			else if ((unsigned char) *vp < 0x20 || (unsigned char) *vp > 0x7e)
			{
				int			val;	/* holds unprintable chars */

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
	}
	else
	{
		elog(ERROR, "unrecognized bytea_output setting: %d",
			 bytea_output);
		rp = result = NULL;		/* keep compiler quiet */
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
	SET_VARSIZE(result, nbytes + VARHDRSZ);
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

Datum
bytea_string_agg_transfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	/* Append the value unless null. */
	if (!PG_ARGISNULL(1))
	{
		bytea	   *value = PG_GETARG_BYTEA_PP(1);

		/* On the first time through, we ignore the delimiter. */
		if (state == NULL)
			state = makeStringAggState(fcinfo);
		else if (!PG_ARGISNULL(2))
		{
			bytea	   *delim = PG_GETARG_BYTEA_PP(2);

			appendBinaryStringInfo(state, VARDATA_ANY(delim), VARSIZE_ANY_EXHDR(delim));
		}

		appendBinaryStringInfo(state, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
	}

	/*
	 * The transition type for string_agg() is declared to be "internal",
	 * which is a pass-by-value type the same size as a pointer.
	 */
	PG_RETURN_POINTER(state);
}

Datum
bytea_string_agg_finalfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	/* cannot be called directly because of internal-type argument */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	if (state != NULL)
	{
		bytea	   *result;

		result = (bytea *) palloc(state->len + VARHDRSZ);
		SET_VARSIZE(result, state->len + VARHDRSZ);
		memcpy(VARDATA(result), state->data, state->len);
		PG_RETURN_BYTEA_P(result);
	}
	else
		PG_RETURN_NULL();
}

/*
 *		textin			- converts "..." to internal representation
 */
Datum
textin(PG_FUNCTION_ARGS)
{
	char	   *inputText = PG_GETARG_CSTRING(0);

	PG_RETURN_TEXT_P(cstring_to_text(inputText));
}

/*
 *		textout			- converts internal representation to "..."
 */
Datum
textout(PG_FUNCTION_ARGS)
{
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
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

	result = cstring_to_text_with_len(str, nbytes);
	pfree(str);
	PG_RETURN_TEXT_P(result);
}

/*
 *		textsend			- converts text to binary format
 */
Datum
textsend(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
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
 *	functions.  Note that the argument is passed as a Datum, to indicate that
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
		text	   *t = DatumGetTextPP(str);

		PG_RETURN_INT32(pg_mbstrlen_with_len(VARDATA_ANY(t),
											 VARSIZE_ANY_EXHDR(t)));
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
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);

	PG_RETURN_TEXT_P(text_catenate(t1, t2));
}

/*
 * text_catenate
 *	Guts of textcat(), broken out so it can be used by other functions
 *
 * Arguments can be in short-header form, but not compressed or out-of-line
 */
static text *
text_catenate(text *t1, text *t2)
{
	text	   *result;
	int			len1,
				len2,
				len;
	char	   *ptr;

	len1 = VARSIZE_ANY_EXHDR(t1);
	len2 = VARSIZE_ANY_EXHDR(t2);

	/* paranoia ... probably should throw error instead? */
	if (len1 < 0)
		len1 = 0;
	if (len2 < 0)
		len2 = 0;

	len = len1 + len2 + VARHDRSZ;
	result = (text *) palloc(len);

	/* Set size of result string... */
	SET_VARSIZE(result, len);

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	if (len1 > 0)
		memcpy(ptr, VARDATA_ANY(t1), len1);
	if (len2 > 0)
		memcpy(ptr + len1, VARDATA_ANY(t2), len2);

	return result;
}

/*
 * charlen_to_bytelen()
 *	Compute the number of bytes occupied by n characters starting at *p
 *
 * It is caller's responsibility that there actually are n characters;
 * the string need not be null-terminated.
 */
static int
charlen_to_bytelen(const char *p, int n)
{
	if (pg_database_encoding_max_length() == 1)
	{
		/* Optimization for single-byte encodings */
		return n;
	}
	else
	{
		const char *s;

		for (s = p; n > 0; n--)
			s += pg_mblen(s);

		return s - p;
	}
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
 *	adjusting the length to be consistent with the "negative start" per SQL.
 * If the length is less than zero, return the remaining string.
 *
 * Added multibyte support.
 * - Tatsuo Ishii 1998-4-21
 * Changed behavior if starting position is less than one to conform to SQL behavior.
 * Formerly returned the entire string; now returns a portion.
 * - Thomas Lockhart 1998-12-10
 * Now uses faster TOAST-slicing interface
 * - John Gray 2002-02-22
 * Remove "#ifdef MULTIBYTE" and test for encoding_max_length instead. Change
 * behaviors conflicting with SQL to meet SQL (if E = S + L < S throw
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
 *	functions.  Note that the argument is passed as a Datum, to indicate that
 *	it may still be in compressed/toasted form.  We can avoid detoasting all
 *	of it in some cases.
 *
 *	The result is always a freshly palloc'd datum.
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

		if (length_not_specified)	/* special case - get length to end of
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
				return cstring_to_text("");

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

		if (length_not_specified)	/* special case - get length to end of
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
				return cstring_to_text("");

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

		/*
		 * If we're working with an untoasted source, no need to do an extra
		 * copying step.
		 */
		if (VARATT_IS_COMPRESSED(DatumGetPointer(str)) ||
			VARATT_IS_EXTERNAL(DatumGetPointer(str)))
			slice = DatumGetTextPSlice(str, slice_start, slice_size);
		else
			slice = (text *) DatumGetPointer(str);

		/* see if we got back an empty string */
		if (VARSIZE_ANY_EXHDR(slice) == 0)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		/* Now we can get the actual length of the slice in MB characters */
		slice_strlen = pg_mbstrlen_with_len(VARDATA_ANY(slice),
											VARSIZE_ANY_EXHDR(slice));

		/*
		 * Check that the start position wasn't > slice_strlen. If so, SQL99
		 * says to return a zero-length string.
		 */
		if (S1 > slice_strlen)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

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
		p = VARDATA_ANY(slice);
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
		SET_VARSIZE(ret, VARHDRSZ + (p - s));
		memcpy(VARDATA(ret), s, (p - s));

		if (slice != (text *) DatumGetPointer(str))
			pfree(slice);

		return ret;
	}
	else
		elog(ERROR, "invalid backend encoding: encoding max length < 1");

	/* not reached: suppress compiler warning */
	return NULL;
}

/*
 * textoverlay
 *	Replace specified substring of first string with second
 *
 * The SQL standard defines OVERLAY() in terms of substring and concatenation.
 * This code is a direct implementation of what the standard says.
 */
Datum
textoverlay(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* substring start position */
	int			sl = PG_GETARG_INT32(3);	/* substring length */

	PG_RETURN_TEXT_P(text_overlay(t1, t2, sp, sl));
}

Datum
textoverlay_no_len(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* substring start position */
	int			sl;

	sl = text_length(PointerGetDatum(t2));	/* defaults to length(t2) */
	PG_RETURN_TEXT_P(text_overlay(t1, t2, sp, sl));
}

static text *
text_overlay(text *t1, text *t2, int sp, int sl)
{
	text	   *result;
	text	   *s1;
	text	   *s2;
	int			sp_pl_sl;

	/*
	 * Check for possible integer-overflow cases.  For negative sp, throw a
	 * "substring length" error because that's what should be expected
	 * according to the spec's definition of OVERLAY().
	 */
	if (sp <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
	if (pg_add_s32_overflow(sp, sl, &sp_pl_sl))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	s1 = text_substring(PointerGetDatum(t1), 1, sp - 1, false);
	s2 = text_substring(PointerGetDatum(t1), sp_pl_sl, -1, true);
	result = text_catenate(s1, t2);
	result = text_catenate(result, s2);

	return result;
}

/*
 * textpos -
 *	  Return the position of the specified substring.
 *	  Implements the SQL POSITION() function.
 *	  Ref: A Guide To The SQL Standard, Date & Darwen, 1997
 * - thomas 1997-07-27
 */
Datum
textpos(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *search_str = PG_GETARG_TEXT_PP(1);

	PG_RETURN_INT32((int32) text_position(str, search_str, PG_GET_COLLATION()));
}

/*
 * text_position -
 *	Does the real work for textpos()
 *
 * Inputs:
 *		t1 - string to be searched
 *		t2 - pattern to match within t1
 * Result:
 *		Character index of the first matched char, starting from 1,
 *		or 0 if no match.
 *
 *	This is broken out so it can be called directly by other string processing
 *	functions.
 */
static int
text_position(text *t1, text *t2, Oid collid)
{
	TextPositionState state;
	int			result;

	/* Empty needle always matches at position 1 */
	if (VARSIZE_ANY_EXHDR(t2) < 1)
		return 1;

	/* Otherwise, can't match if haystack is shorter than needle */
	if (VARSIZE_ANY_EXHDR(t1) < VARSIZE_ANY_EXHDR(t2))
		return 0;

	text_position_setup(t1, t2, collid, &state);
	if (!text_position_next(&state))
		result = 0;
	else
		result = text_position_get_match_pos(&state);
	text_position_cleanup(&state);
	return result;
}


/*
 * text_position_setup, text_position_next, text_position_cleanup -
 *	Component steps of text_position()
 *
 * These are broken out so that a string can be efficiently searched for
 * multiple occurrences of the same pattern.  text_position_next may be
 * called multiple times, and it advances to the next match on each call.
 * text_position_get_match_ptr() and text_position_get_match_pos() return
 * a pointer or 1-based character position of the last match, respectively.
 *
 * The "state" variable is normally just a local variable in the caller.
 *
 * NOTE: text_position_next skips over the matched portion.  For example,
 * searching for "xx" in "xxx" returns only one match, not two.
 */

static void
text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state)
{
	int			len1 = VARSIZE_ANY_EXHDR(t1);
	int			len2 = VARSIZE_ANY_EXHDR(t2);
	pg_locale_t mylocale = 0;

	check_collation_set(collid);

	if (!lc_collate_is_c(collid) && collid != DEFAULT_COLLATION_OID)
		mylocale = pg_newlocale_from_collation(collid);

	if (mylocale && !mylocale->deterministic)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for substring searches")));

	Assert(len1 > 0);
	Assert(len2 > 0);

	/*
	 * Even with a multi-byte encoding, we perform the search using the raw
	 * byte sequence, ignoring multibyte issues.  For UTF-8, that works fine,
	 * because in UTF-8 the byte sequence of one character cannot contain
	 * another character.  For other multi-byte encodings, we do the search
	 * initially as a simple byte search, ignoring multibyte issues, but
	 * verify afterwards that the match we found is at a character boundary,
	 * and continue the search if it was a false match.
	 */
	if (pg_database_encoding_max_length() == 1)
	{
		state->is_multibyte = false;
		state->is_multibyte_char_in_char = false;
	}
	else if (GetDatabaseEncoding() == PG_UTF8)
	{
		state->is_multibyte = true;
		state->is_multibyte_char_in_char = false;
	}
	else
	{
		state->is_multibyte = true;
		state->is_multibyte_char_in_char = true;
	}

	state->str1 = VARDATA_ANY(t1);
	state->str2 = VARDATA_ANY(t2);
	state->len1 = len1;
	state->len2 = len2;
	state->last_match = NULL;
	state->refpoint = state->str1;
	state->refpos = 0;

	/*
	 * Prepare the skip table for Boyer-Moore-Horspool searching.  In these
	 * notes we use the terminology that the "haystack" is the string to be
	 * searched (t1) and the "needle" is the pattern being sought (t2).
	 *
	 * If the needle is empty or bigger than the haystack then there is no
	 * point in wasting cycles initializing the table.  We also choose not to
	 * use B-M-H for needles of length 1, since the skip table can't possibly
	 * save anything in that case.
	 */
	if (len1 >= len2 && len2 > 1)
	{
		int			searchlength = len1 - len2;
		int			skiptablemask;
		int			last;
		int			i;
		const char *str2 = state->str2;

		/*
		 * First we must determine how much of the skip table to use.  The
		 * declaration of TextPositionState allows up to 256 elements, but for
		 * short search problems we don't really want to have to initialize so
		 * many elements --- it would take too long in comparison to the
		 * actual search time.  So we choose a useful skip table size based on
		 * the haystack length minus the needle length.  The closer the needle
		 * length is to the haystack length the less useful skipping becomes.
		 *
		 * Note: since we use bit-masking to select table elements, the skip
		 * table size MUST be a power of 2, and so the mask must be 2^N-1.
		 */
		if (searchlength < 16)
			skiptablemask = 3;
		else if (searchlength < 64)
			skiptablemask = 7;
		else if (searchlength < 128)
			skiptablemask = 15;
		else if (searchlength < 512)
			skiptablemask = 31;
		else if (searchlength < 2048)
			skiptablemask = 63;
		else if (searchlength < 4096)
			skiptablemask = 127;
		else
			skiptablemask = 255;
		state->skiptablemask = skiptablemask;

		/*
		 * Initialize the skip table.  We set all elements to the needle
		 * length, since this is the correct skip distance for any character
		 * not found in the needle.
		 */
		for (i = 0; i <= skiptablemask; i++)
			state->skiptable[i] = len2;

		/*
		 * Now examine the needle.  For each character except the last one,
		 * set the corresponding table element to the appropriate skip
		 * distance.  Note that when two characters share the same skip table
		 * entry, the one later in the needle must determine the skip
		 * distance.
		 */
		last = len2 - 1;

		for (i = 0; i < last; i++)
			state->skiptable[(unsigned char) str2[i] & skiptablemask] = last - i;
	}
}

/*
 * Advance to the next match, starting from the end of the previous match
 * (or the beginning of the string, on first call).  Returns true if a match
 * is found.
 *
 * Note that this refuses to match an empty-string needle.  Most callers
 * will have handled that case specially and we'll never see it here.
 */
static bool
text_position_next(TextPositionState *state)
{
	int			needle_len = state->len2;
	char	   *start_ptr;
	char	   *matchptr;

	if (needle_len <= 0)
		return false;			/* result for empty pattern */

	/* Start from the point right after the previous match. */
	if (state->last_match)
		start_ptr = state->last_match + needle_len;
	else
		start_ptr = state->str1;

retry:
	matchptr = text_position_next_internal(start_ptr, state);

	if (!matchptr)
		return false;

	/*
	 * Found a match for the byte sequence.  If this is a multibyte encoding,
	 * where one character's byte sequence can appear inside a longer
	 * multi-byte character, we need to verify that the match was at a
	 * character boundary, not in the middle of a multi-byte character.
	 */
	if (state->is_multibyte_char_in_char)
	{
		/* Walk one character at a time, until we reach the match. */

		/* the search should never move backwards. */
		Assert(state->refpoint <= matchptr);

		while (state->refpoint < matchptr)
		{
			/* step to next character. */
			state->refpoint += pg_mblen(state->refpoint);
			state->refpos++;

			/*
			 * If we stepped over the match's start position, then it was a
			 * false positive, where the byte sequence appeared in the middle
			 * of a multi-byte character.  Skip it, and continue the search at
			 * the next character boundary.
			 */
			if (state->refpoint > matchptr)
			{
				start_ptr = state->refpoint;
				goto retry;
			}
		}
	}

	state->last_match = matchptr;
	return true;
}

/*
 * Subroutine of text_position_next().  This searches for the raw byte
 * sequence, ignoring any multi-byte encoding issues.  Returns the first
 * match starting at 'start_ptr', or NULL if no match is found.
 */
static char *
text_position_next_internal(char *start_ptr, TextPositionState *state)
{
	int			haystack_len = state->len1;
	int			needle_len = state->len2;
	int			skiptablemask = state->skiptablemask;
	const char *haystack = state->str1;
	const char *needle = state->str2;
	const char *haystack_end = &haystack[haystack_len];
	const char *hptr;

	Assert(start_ptr >= haystack && start_ptr <= haystack_end);

	if (needle_len == 1)
	{
		/* No point in using B-M-H for a one-character needle */
		char		nchar = *needle;

		hptr = start_ptr;
		while (hptr < haystack_end)
		{
			if (*hptr == nchar)
				return (char *) hptr;
			hptr++;
		}
	}
	else
	{
		const char *needle_last = &needle[needle_len - 1];

		/* Start at startpos plus the length of the needle */
		hptr = start_ptr + needle_len - 1;
		while (hptr < haystack_end)
		{
			/* Match the needle scanning *backward* */
			const char *nptr;
			const char *p;

			nptr = needle_last;
			p = hptr;
			while (*nptr == *p)
			{
				/* Matched it all?	If so, return 1-based position */
				if (nptr == needle)
					return (char *) p;
				nptr--, p--;
			}

			/*
			 * No match, so use the haystack char at hptr to decide how far to
			 * advance.  If the needle had any occurrence of that character
			 * (or more precisely, one sharing the same skiptable entry)
			 * before its last character, then we advance far enough to align
			 * the last such needle character with that haystack position.
			 * Otherwise we can advance by the whole needle length.
			 */
			hptr += state->skiptable[(unsigned char) *hptr & skiptablemask];
		}
	}

	return 0;					/* not found */
}

/*
 * Return a pointer to the current match.
 *
 * The returned pointer points into the original haystack string.
 */
static char *
text_position_get_match_ptr(TextPositionState *state)
{
	return state->last_match;
}

/*
 * Return the offset of the current match.
 *
 * The offset is in characters, 1-based.
 */
static int
text_position_get_match_pos(TextPositionState *state)
{
	if (!state->is_multibyte)
		return state->last_match - state->str1 + 1;
	else
	{
		/* Convert the byte position to char position. */
		while (state->refpoint < state->last_match)
		{
			state->refpoint += pg_mblen(state->refpoint);
			state->refpos++;
		}
		Assert(state->refpoint == state->last_match);
		return state->refpos + 1;
	}
}

/*
 * Reset search state to the initial state installed by text_position_setup.
 *
 * The next call to text_position_next will search from the beginning
 * of the string.
 */
static void
text_position_reset(TextPositionState *state)
{
	state->last_match = NULL;
	state->refpoint = state->str1;
	state->refpos = 0;
}

static void
text_position_cleanup(TextPositionState *state)
{
	/* no cleanup needed */
}


static void
check_collation_set(Oid collid)
{
	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

/* varstr_cmp()
 * Comparison function for text strings with given lengths.
 * Includes locale support, but must copy strings to temporary memory
 *	to allow null-termination for inputs to strcoll().
 * Returns an integer less than, equal to, or greater than zero, indicating
 * whether arg1 is less than, equal to, or greater than arg2.
 *
 * Note: many functions that depend on this are marked leakproof; therefore,
 * avoid reporting the actual contents of the input when throwing errors.
 * All errors herein should be things that can't happen except on corrupt
 * data, anyway; otherwise we will have trouble with indexing strings that
 * would cause them.
 */
int
varstr_cmp(const char *arg1, int len1, const char *arg2, int len2, Oid collid)
{
	int			result;

	check_collation_set(collid);

	/*
	 * Unfortunately, there is no strncoll(), so in the non-C locale case we
	 * have to do some memory copying.  This turns out to be significantly
	 * slower, so we optimize the case where LC_COLLATE is C.  We also try to
	 * optimize relatively-short strings by avoiding palloc/pfree overhead.
	 */
	if (lc_collate_is_c(collid))
	{
		result = memcmp(arg1, arg2, Min(len1, len2));
		if ((result == 0) && (len1 != len2))
			result = (len1 < len2) ? -1 : 1;
	}
	else
	{
		char		a1buf[TEXTBUFLEN];
		char		a2buf[TEXTBUFLEN];
		char	   *a1p,
				   *a2p;
		pg_locale_t mylocale = 0;

		if (collid != DEFAULT_COLLATION_OID)
			mylocale = pg_newlocale_from_collation(collid);

		/*
		 * memcmp() can't tell us which of two unequal strings sorts first,
		 * but it's a cheap way to tell if they're equal.  Testing shows that
		 * memcmp() followed by strcoll() is only trivially slower than
		 * strcoll() by itself, so we don't lose much if this doesn't work out
		 * very often, and if it does - for example, because there are many
		 * equal strings in the input - then we win big by avoiding expensive
		 * collation-aware comparisons.
		 */
		if (len1 == len2 && memcmp(arg1, arg2, len1) == 0)
			return 0;

#ifdef WIN32
		/* Win32 does not have UTF-8, so we need to map to UTF-16 */
		if (GetDatabaseEncoding() == PG_UTF8
			&& (!mylocale || mylocale->provider == COLLPROVIDER_LIBC))
		{
			int			a1len;
			int			a2len;
			int			r;

			if (len1 >= TEXTBUFLEN / 2)
			{
				a1len = len1 * 2 + 2;
				a1p = palloc(a1len);
			}
			else
			{
				a1len = TEXTBUFLEN;
				a1p = a1buf;
			}
			if (len2 >= TEXTBUFLEN / 2)
			{
				a2len = len2 * 2 + 2;
				a2p = palloc(a2len);
			}
			else
			{
				a2len = TEXTBUFLEN;
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
							(errmsg("could not convert string to UTF-16: error code %lu",
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
							(errmsg("could not convert string to UTF-16: error code %lu",
									GetLastError())));
			}
			((LPWSTR) a2p)[r] = 0;

			errno = 0;
#ifdef HAVE_LOCALE_T
			if (mylocale)
				result = wcscoll_l((LPWSTR) a1p, (LPWSTR) a2p, mylocale->info.lt);
			else
#endif
				result = wcscoll((LPWSTR) a1p, (LPWSTR) a2p);
			if (result == 2147483647)	/* _NLSCMPERROR; missing from mingw
										 * headers */
				ereport(ERROR,
						(errmsg("could not compare Unicode strings: %m")));

			/* Break tie if necessary. */
			if (result == 0 &&
				(!mylocale || mylocale->deterministic))
			{
				result = memcmp(arg1, arg2, Min(len1, len2));
				if ((result == 0) && (len1 != len2))
					result = (len1 < len2) ? -1 : 1;
			}

			if (a1p != a1buf)
				pfree(a1p);
			if (a2p != a2buf)
				pfree(a2p);

			return result;
		}
#endif							/* WIN32 */

		if (len1 >= TEXTBUFLEN)
			a1p = (char *) palloc(len1 + 1);
		else
			a1p = a1buf;
		if (len2 >= TEXTBUFLEN)
			a2p = (char *) palloc(len2 + 1);
		else
			a2p = a2buf;

		memcpy(a1p, arg1, len1);
		a1p[len1] = '\0';
		memcpy(a2p, arg2, len2);
		a2p[len2] = '\0';

		if (mylocale)
		{
			if (mylocale->provider == COLLPROVIDER_ICU)
			{
#ifdef USE_ICU
#ifdef HAVE_UCOL_STRCOLLUTF8
				if (GetDatabaseEncoding() == PG_UTF8)
				{
					UErrorCode	status;

					status = U_ZERO_ERROR;
					result = ucol_strcollUTF8(mylocale->info.icu.ucol,
											  arg1, len1,
											  arg2, len2,
											  &status);
					if (U_FAILURE(status))
						ereport(ERROR,
								(errmsg("collation failed: %s", u_errorName(status))));
				}
				else
#endif
				{
					int32_t		ulen1,
								ulen2;
					UChar	   *uchar1,
							   *uchar2;

					ulen1 = icu_to_uchar(&uchar1, arg1, len1);
					ulen2 = icu_to_uchar(&uchar2, arg2, len2);

					result = ucol_strcoll(mylocale->info.icu.ucol,
										  uchar1, ulen1,
										  uchar2, ulen2);

					pfree(uchar1);
					pfree(uchar2);
				}
#else							/* not USE_ICU */
				/* shouldn't happen */
				elog(ERROR, "unsupported collprovider: %c", mylocale->provider);
#endif							/* not USE_ICU */
			}
			else
			{
#ifdef HAVE_LOCALE_T
				result = strcoll_l(a1p, a2p, mylocale->info.lt);
#else
				/* shouldn't happen */
				elog(ERROR, "unsupported collprovider: %c", mylocale->provider);
#endif
			}
		}
		else
			result = strcoll(a1p, a2p);

		/* Break tie if necessary. */
		if (result == 0 &&
			(!mylocale || mylocale->deterministic))
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
text_cmp(text *arg1, text *arg2, Oid collid)
{
	char	   *a1p,
			   *a2p;
	int			len1,
				len2;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	return varstr_cmp(a1p, len1, a2p, len2, collid);
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
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (lc_collate_is_c(collid) ||
		collid == DEFAULT_COLLATION_OID ||
		pg_newlocale_from_collation(collid)->deterministic)
	{
		Datum		arg1 = PG_GETARG_DATUM(0);
		Datum		arg2 = PG_GETARG_DATUM(1);
		Size		len1,
					len2;

		/*
		 * Since we only care about equality or not-equality, we can avoid all
		 * the expense of strcoll() here, and just do bitwise comparison.  In
		 * fact, we don't even have to do a bitwise comparison if we can show
		 * the lengths of the strings are unequal; which might save us from
		 * having to detoast one or both values.
		 */
		len1 = toast_raw_datum_size(arg1);
		len2 = toast_raw_datum_size(arg2);
		if (len1 != len2)
			result = false;
		else
		{
			text	   *targ1 = DatumGetTextPP(arg1);
			text	   *targ2 = DatumGetTextPP(arg2);

			result = (memcmp(VARDATA_ANY(targ1), VARDATA_ANY(targ2),
							 len1 - VARHDRSZ) == 0);

			PG_FREE_IF_COPY(targ1, 0);
			PG_FREE_IF_COPY(targ2, 1);
		}
	}
	else
	{
		text	   *arg1 = PG_GETARG_TEXT_PP(0);
		text	   *arg2 = PG_GETARG_TEXT_PP(1);

		result = (text_cmp(arg1, arg2, collid) == 0);

		PG_FREE_IF_COPY(arg1, 0);
		PG_FREE_IF_COPY(arg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
textne(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (lc_collate_is_c(collid) ||
		collid == DEFAULT_COLLATION_OID ||
		pg_newlocale_from_collation(collid)->deterministic)
	{
		Datum		arg1 = PG_GETARG_DATUM(0);
		Datum		arg2 = PG_GETARG_DATUM(1);
		Size		len1,
					len2;

		/* See comment in texteq() */
		len1 = toast_raw_datum_size(arg1);
		len2 = toast_raw_datum_size(arg2);
		if (len1 != len2)
			result = true;
		else
		{
			text	   *targ1 = DatumGetTextPP(arg1);
			text	   *targ2 = DatumGetTextPP(arg2);

			result = (memcmp(VARDATA_ANY(targ1), VARDATA_ANY(targ2),
							 len1 - VARHDRSZ) != 0);

			PG_FREE_IF_COPY(targ1, 0);
			PG_FREE_IF_COPY(targ2, 1);
		}
	}
	else
	{
		text	   *arg1 = PG_GETARG_TEXT_PP(0);
		text	   *arg2 = PG_GETARG_TEXT_PP(1);

		result = (text_cmp(arg1, arg2, collid) != 0);

		PG_FREE_IF_COPY(arg1, 0);
		PG_FREE_IF_COPY(arg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
text_lt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) < 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_le(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) <= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_gt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) > 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_ge(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) >= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_starts_with(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale = 0;
	bool		result;
	Size		len1,
				len2;

	check_collation_set(collid);

	if (!lc_collate_is_c(collid) && collid != DEFAULT_COLLATION_OID)
		mylocale = pg_newlocale_from_collation(collid);

	if (mylocale && !mylocale->deterministic)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for substring searches")));

	len1 = toast_raw_datum_size(arg1);
	len2 = toast_raw_datum_size(arg2);
	if (len2 > len1)
		result = false;
	else
	{
		text	   *targ1 = text_substring(arg1, 1, len2, false);
		text	   *targ2 = DatumGetTextPP(arg2);

		result = (memcmp(VARDATA_ANY(targ1), VARDATA_ANY(targ2),
						 VARSIZE_ANY_EXHDR(targ2)) == 0);

		PG_FREE_IF_COPY(targ1, 0);
		PG_FREE_IF_COPY(targ2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
bttextcmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		result;

	result = text_cmp(arg1, arg2, PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

Datum
bttextsortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	Oid			collid = ssup->ssup_collation;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* Use generic string SortSupport */
	varstr_sortsupport(ssup, TEXTOID, collid);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

/*
 * Generic sortsupport interface for character type's operator classes.
 * Includes locale support, and support for BpChar semantics (i.e. removing
 * trailing spaces before comparison).
 *
 * Relies on the assumption that text, VarChar, BpChar, and bytea all have the
 * same representation.  Callers that always use the C collation (e.g.
 * non-collatable type callers like bytea) may have NUL bytes in their strings;
 * this will not work with any other collation, though.
 */
void
varstr_sortsupport(SortSupport ssup, Oid typid, Oid collid)
{
	bool		abbreviate = ssup->abbreviate;
	bool		collate_c = false;
	VarStringSortSupport *sss;
	pg_locale_t locale = 0;

	check_collation_set(collid);

	/*
	 * If possible, set ssup->comparator to a function which can be used to
	 * directly compare two datums.  If we can do this, we'll avoid the
	 * overhead of a trip through the fmgr layer for every comparison, which
	 * can be substantial.
	 *
	 * Most typically, we'll set the comparator to varlenafastcmp_locale,
	 * which uses strcoll() to perform comparisons.  We use that for the
	 * BpChar case too, but type NAME uses namefastcmp_locale. However, if
	 * LC_COLLATE = C, we can make things quite a bit faster with
	 * varstrfastcmp_c, bpcharfastcmp_c, or namefastcmp_c, all of which use
	 * memcmp() rather than strcoll().
	 */
	if (lc_collate_is_c(collid))
	{
		if (typid == BPCHAROID)
			ssup->comparator = bpcharfastcmp_c;
		else if (typid == NAMEOID)
		{
			ssup->comparator = namefastcmp_c;
			/* Not supporting abbreviation with type NAME, for now */
			abbreviate = false;
		}
		else
			ssup->comparator = varstrfastcmp_c;

		collate_c = true;
	}
	else
	{
		/*
		 * We need a collation-sensitive comparison.  To make things faster,
		 * we'll figure out the collation based on the locale id and cache the
		 * result.
		 */
		if (collid != DEFAULT_COLLATION_OID)
			locale = pg_newlocale_from_collation(collid);

		/*
		 * There is a further exception on Windows.  When the database
		 * encoding is UTF-8 and we are not using the C collation, complex
		 * hacks are required.  We don't currently have a comparator that
		 * handles that case, so we fall back on the slow method of having the
		 * sort code invoke bttextcmp() (in the case of text) via the fmgr
		 * trampoline.  ICU locales work just the same on Windows, however.
		 */
#ifdef WIN32
		if (GetDatabaseEncoding() == PG_UTF8 &&
			!(locale && locale->provider == COLLPROVIDER_ICU))
			return;
#endif

		/*
		 * We use varlenafastcmp_locale except for type NAME.
		 */
		if (typid == NAMEOID)
		{
			ssup->comparator = namefastcmp_locale;
			/* Not supporting abbreviation with type NAME, for now */
			abbreviate = false;
		}
		else
			ssup->comparator = varlenafastcmp_locale;
	}

	/*
	 * Unfortunately, it seems that abbreviation for non-C collations is
	 * broken on many common platforms; testing of multiple versions of glibc
	 * reveals that, for many locales, strcoll() and strxfrm() do not return
	 * consistent results, which is fatal to this optimization.  While no
	 * other libc other than Cygwin has so far been shown to have a problem,
	 * we take the conservative course of action for right now and disable
	 * this categorically.  (Users who are certain this isn't a problem on
	 * their system can define TRUST_STRXFRM.)
	 *
	 * Even apart from the risk of broken locales, it's possible that there
	 * are platforms where the use of abbreviated keys should be disabled at
	 * compile time.  Having only 4 byte datums could make worst-case
	 * performance drastically more likely, for example.  Moreover, macOS's
	 * strxfrm() implementation is known to not effectively concentrate a
	 * significant amount of entropy from the original string in earlier
	 * transformed blobs.  It's possible that other supported platforms are
	 * similarly encumbered.  So, if we ever get past disabling this
	 * categorically, we may still want or need to disable it for particular
	 * platforms.
	 */
#ifndef TRUST_STRXFRM
	if (!collate_c && !(locale && locale->provider == COLLPROVIDER_ICU))
		abbreviate = false;
#endif

	/*
	 * If we're using abbreviated keys, or if we're using a locale-aware
	 * comparison, we need to initialize a VarStringSortSupport object. Both
	 * cases will make use of the temporary buffers we initialize here for
	 * scratch space (and to detect requirement for BpChar semantics from
	 * caller), and the abbreviation case requires additional state.
	 */
	if (abbreviate || !collate_c)
	{
		sss = palloc(sizeof(VarStringSortSupport));
		sss->buf1 = palloc(TEXTBUFLEN);
		sss->buflen1 = TEXTBUFLEN;
		sss->buf2 = palloc(TEXTBUFLEN);
		sss->buflen2 = TEXTBUFLEN;
		/* Start with invalid values */
		sss->last_len1 = -1;
		sss->last_len2 = -1;
		/* Initialize */
		sss->last_returned = 0;
		sss->locale = locale;

		/*
		 * To avoid somehow confusing a strxfrm() blob and an original string,
		 * constantly keep track of the variety of data that buf1 and buf2
		 * currently contain.
		 *
		 * Comparisons may be interleaved with conversion calls.  Frequently,
		 * conversions and comparisons are batched into two distinct phases,
		 * but the correctness of caching cannot hinge upon this.  For
		 * comparison caching, buffer state is only trusted if cache_blob is
		 * found set to false, whereas strxfrm() caching only trusts the state
		 * when cache_blob is found set to true.
		 *
		 * Arbitrarily initialize cache_blob to true.
		 */
		sss->cache_blob = true;
		sss->collate_c = collate_c;
		sss->typid = typid;
		ssup->ssup_extra = sss;

		/*
		 * If possible, plan to use the abbreviated keys optimization.  The
		 * core code may switch back to authoritative comparator should
		 * abbreviation be aborted.
		 */
		if (abbreviate)
		{
			sss->prop_card = 0.20;
			initHyperLogLog(&sss->abbr_card, 10);
			initHyperLogLog(&sss->full_card, 10);
			ssup->abbrev_full_comparator = ssup->comparator;
			ssup->comparator = varstrcmp_abbrev;
			ssup->abbrev_converter = varstr_abbrev_convert;
			ssup->abbrev_abort = varstr_abbrev_abort;
		}
	}
}

/*
 * sortsupport comparison func (for C locale case)
 */
static int
varstrfastcmp_c(Datum x, Datum y, SortSupport ssup)
{
	VarString  *arg1 = DatumGetVarStringPP(x);
	VarString  *arg2 = DatumGetVarStringPP(y);
	char	   *a1p,
			   *a2p;
	int			len1,
				len2,
				result;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	result = memcmp(a1p, a2p, Min(len1, len2));
	if ((result == 0) && (len1 != len2))
		result = (len1 < len2) ? -1 : 1;

	/* We can't afford to leak memory here. */
	if (PointerGetDatum(arg1) != x)
		pfree(arg1);
	if (PointerGetDatum(arg2) != y)
		pfree(arg2);

	return result;
}

/*
 * sortsupport comparison func (for BpChar C locale case)
 *
 * BpChar outsources its sortsupport to this module.  Specialization for the
 * varstr_sortsupport BpChar case, modeled on
 * internal_bpchar_pattern_compare().
 */
static int
bpcharfastcmp_c(Datum x, Datum y, SortSupport ssup)
{
	BpChar	   *arg1 = DatumGetBpCharPP(x);
	BpChar	   *arg2 = DatumGetBpCharPP(y);
	char	   *a1p,
			   *a2p;
	int			len1,
				len2,
				result;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = bpchartruelen(a1p, VARSIZE_ANY_EXHDR(arg1));
	len2 = bpchartruelen(a2p, VARSIZE_ANY_EXHDR(arg2));

	result = memcmp(a1p, a2p, Min(len1, len2));
	if ((result == 0) && (len1 != len2))
		result = (len1 < len2) ? -1 : 1;

	/* We can't afford to leak memory here. */
	if (PointerGetDatum(arg1) != x)
		pfree(arg1);
	if (PointerGetDatum(arg2) != y)
		pfree(arg2);

	return result;
}

/*
 * sortsupport comparison func (for NAME C locale case)
 */
static int
namefastcmp_c(Datum x, Datum y, SortSupport ssup)
{
	Name		arg1 = DatumGetName(x);
	Name		arg2 = DatumGetName(y);

	return strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN);
}

/*
 * sortsupport comparison func (for locale case with all varlena types)
 */
static int
varlenafastcmp_locale(Datum x, Datum y, SortSupport ssup)
{
	VarString  *arg1 = DatumGetVarStringPP(x);
	VarString  *arg2 = DatumGetVarStringPP(y);
	char	   *a1p,
			   *a2p;
	int			len1,
				len2,
				result;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	result = varstrfastcmp_locale(a1p, len1, a2p, len2, ssup);

	/* We can't afford to leak memory here. */
	if (PointerGetDatum(arg1) != x)
		pfree(arg1);
	if (PointerGetDatum(arg2) != y)
		pfree(arg2);

	return result;
}

/*
 * sortsupport comparison func (for locale case with NAME type)
 */
static int
namefastcmp_locale(Datum x, Datum y, SortSupport ssup)
{
	Name		arg1 = DatumGetName(x);
	Name		arg2 = DatumGetName(y);

	return varstrfastcmp_locale(NameStr(*arg1), strlen(NameStr(*arg1)),
								NameStr(*arg2), strlen(NameStr(*arg2)),
								ssup);
}

/*
 * sortsupport comparison func for locale cases
 */
static int
varstrfastcmp_locale(char *a1p, int len1, char *a2p, int len2, SortSupport ssup)
{
	VarStringSortSupport *sss = (VarStringSortSupport *) ssup->ssup_extra;
	int			result;
	bool		arg1_match;

	/* Fast pre-check for equality, as discussed in varstr_cmp() */
	if (len1 == len2 && memcmp(a1p, a2p, len1) == 0)
	{
		/*
		 * No change in buf1 or buf2 contents, so avoid changing last_len1 or
		 * last_len2.  Existing contents of buffers might still be used by
		 * next call.
		 *
		 * It's fine to allow the comparison of BpChar padding bytes here,
		 * even though that implies that the memcmp() will usually be
		 * performed for BpChar callers (though multibyte characters could
		 * still prevent that from occurring).  The memcmp() is still very
		 * cheap, and BpChar's funny semantics have us remove trailing spaces
		 * (not limited to padding), so we need make no distinction between
		 * padding space characters and "real" space characters.
		 */
		return 0;
	}

	if (sss->typid == BPCHAROID)
	{
		/* Get true number of bytes, ignoring trailing spaces */
		len1 = bpchartruelen(a1p, len1);
		len2 = bpchartruelen(a2p, len2);
	}

	if (len1 >= sss->buflen1)
	{
		pfree(sss->buf1);
		sss->buflen1 = Max(len1 + 1, Min(sss->buflen1 * 2, MaxAllocSize));
		sss->buf1 = MemoryContextAlloc(ssup->ssup_cxt, sss->buflen1);
	}
	if (len2 >= sss->buflen2)
	{
		pfree(sss->buf2);
		sss->buflen2 = Max(len2 + 1, Min(sss->buflen2 * 2, MaxAllocSize));
		sss->buf2 = MemoryContextAlloc(ssup->ssup_cxt, sss->buflen2);
	}

	/*
	 * We're likely to be asked to compare the same strings repeatedly, and
	 * memcmp() is so much cheaper than strcoll() that it pays to try to cache
	 * comparisons, even though in general there is no reason to think that
	 * that will work out (every string datum may be unique).  Caching does
	 * not slow things down measurably when it doesn't work out, and can speed
	 * things up by rather a lot when it does.  In part, this is because the
	 * memcmp() compares data from cachelines that are needed in L1 cache even
	 * when the last comparison's result cannot be reused.
	 */
	arg1_match = true;
	if (len1 != sss->last_len1 || memcmp(sss->buf1, a1p, len1) != 0)
	{
		arg1_match = false;
		memcpy(sss->buf1, a1p, len1);
		sss->buf1[len1] = '\0';
		sss->last_len1 = len1;
	}

	/*
	 * If we're comparing the same two strings as last time, we can return the
	 * same answer without calling strcoll() again.  This is more likely than
	 * it seems (at least with moderate to low cardinality sets), because
	 * quicksort compares the same pivot against many values.
	 */
	if (len2 != sss->last_len2 || memcmp(sss->buf2, a2p, len2) != 0)
	{
		memcpy(sss->buf2, a2p, len2);
		sss->buf2[len2] = '\0';
		sss->last_len2 = len2;
	}
	else if (arg1_match && !sss->cache_blob)
	{
		/* Use result cached following last actual strcoll() call */
		return sss->last_returned;
	}

	if (sss->locale)
	{
		if (sss->locale->provider == COLLPROVIDER_ICU)
		{
#ifdef USE_ICU
#ifdef HAVE_UCOL_STRCOLLUTF8
			if (GetDatabaseEncoding() == PG_UTF8)
			{
				UErrorCode	status;

				status = U_ZERO_ERROR;
				result = ucol_strcollUTF8(sss->locale->info.icu.ucol,
										  a1p, len1,
										  a2p, len2,
										  &status);
				if (U_FAILURE(status))
					ereport(ERROR,
							(errmsg("collation failed: %s", u_errorName(status))));
			}
			else
#endif
			{
				int32_t		ulen1,
							ulen2;
				UChar	   *uchar1,
						   *uchar2;

				ulen1 = icu_to_uchar(&uchar1, a1p, len1);
				ulen2 = icu_to_uchar(&uchar2, a2p, len2);

				result = ucol_strcoll(sss->locale->info.icu.ucol,
									  uchar1, ulen1,
									  uchar2, ulen2);

				pfree(uchar1);
				pfree(uchar2);
			}
#else							/* not USE_ICU */
			/* shouldn't happen */
			elog(ERROR, "unsupported collprovider: %c", sss->locale->provider);
#endif							/* not USE_ICU */
		}
		else
		{
#ifdef HAVE_LOCALE_T
			result = strcoll_l(sss->buf1, sss->buf2, sss->locale->info.lt);
#else
			/* shouldn't happen */
			elog(ERROR, "unsupported collprovider: %c", sss->locale->provider);
#endif
		}
	}
	else
		result = strcoll(sss->buf1, sss->buf2);

	/* Break tie if necessary. */
	if (result == 0 &&
		(!sss->locale || sss->locale->deterministic))
		result = strcmp(sss->buf1, sss->buf2);

	/* Cache result, perhaps saving an expensive strcoll() call next time */
	sss->cache_blob = false;
	sss->last_returned = result;
	return result;
}

/*
 * Abbreviated key comparison func
 */
static int
varstrcmp_abbrev(Datum x, Datum y, SortSupport ssup)
{
	/*
	 * When 0 is returned, the core system will call varstrfastcmp_c()
	 * (bpcharfastcmp_c() in BpChar case) or varlenafastcmp_locale().  Even a
	 * strcmp() on two non-truncated strxfrm() blobs cannot indicate *equality*
	 * authoritatively, for the same reason that there is a strcoll()
	 * tie-breaker call to strcmp() in varstr_cmp().
	 */
	if (x > y)
		return 1;
	else if (x == y)
		return 0;
	else
		return -1;
}

/*
 * Conversion routine for sortsupport.  Converts original to abbreviated key
 * representation.  Our encoding strategy is simple -- pack the first 8 bytes
 * of a strxfrm() blob into a Datum (on little-endian machines, the 8 bytes are
 * stored in reverse order), and treat it as an unsigned integer.  When the "C"
 * locale is used, or in case of bytea, just memcpy() from original instead.
 */
static Datum
varstr_abbrev_convert(Datum original, SortSupport ssup)
{
	VarStringSortSupport *sss = (VarStringSortSupport *) ssup->ssup_extra;
	VarString  *authoritative = DatumGetVarStringPP(original);
	char	   *authoritative_data = VARDATA_ANY(authoritative);

	/* working state */
	Datum		res;
	char	   *pres;
	int			len;
	uint32		hash;

	pres = (char *) &res;
	/* memset(), so any non-overwritten bytes are NUL */
	memset(pres, 0, sizeof(Datum));
	len = VARSIZE_ANY_EXHDR(authoritative);

	/* Get number of bytes, ignoring trailing spaces */
	if (sss->typid == BPCHAROID)
		len = bpchartruelen(authoritative_data, len);

	/*
	 * If we're using the C collation, use memcpy(), rather than strxfrm(), to
	 * abbreviate keys.  The full comparator for the C locale is always
	 * memcmp().  It would be incorrect to allow bytea callers (callers that
	 * always force the C collation -- bytea isn't a collatable type, but this
	 * approach is convenient) to use strxfrm().  This is because bytea
	 * strings may contain NUL bytes.  Besides, this should be faster, too.
	 *
	 * More generally, it's okay that bytea callers can have NUL bytes in
	 * strings because varstrcmp_abbrev() need not make a distinction between
	 * terminating NUL bytes, and NUL bytes representing actual NULs in the
	 * authoritative representation.  Hopefully a comparison at or past one
	 * abbreviated key's terminating NUL byte will resolve the comparison
	 * without consulting the authoritative representation; specifically, some
	 * later non-NUL byte in the longer string can resolve the comparison
	 * against a subsequent terminating NUL in the shorter string.  There will
	 * usually be what is effectively a "length-wise" resolution there and
	 * then.
	 *
	 * If that doesn't work out -- if all bytes in the longer string
	 * positioned at or past the offset of the smaller string's (first)
	 * terminating NUL are actually representative of NUL bytes in the
	 * authoritative binary string (perhaps with some *terminating* NUL bytes
	 * towards the end of the longer string iff it happens to still be small)
	 * -- then an authoritative tie-breaker will happen, and do the right
	 * thing: explicitly consider string length.
	 */
	if (sss->collate_c)
		memcpy(pres, authoritative_data, Min(len, sizeof(Datum)));
	else
	{
		Size		bsize;
#ifdef USE_ICU
		int32_t		ulen = -1;
		UChar	   *uchar = NULL;
#endif

		/*
		 * We're not using the C collation, so fall back on strxfrm or ICU
		 * analogs.
		 */

		/* By convention, we use buffer 1 to store and NUL-terminate */
		if (len >= sss->buflen1)
		{
			pfree(sss->buf1);
			sss->buflen1 = Max(len + 1, Min(sss->buflen1 * 2, MaxAllocSize));
			sss->buf1 = palloc(sss->buflen1);
		}

		/* Might be able to reuse strxfrm() blob from last call */
		if (sss->last_len1 == len && sss->cache_blob &&
			memcmp(sss->buf1, authoritative_data, len) == 0)
		{
			memcpy(pres, sss->buf2, Min(sizeof(Datum), sss->last_len2));
			/* No change affecting cardinality, so no hashing required */
			goto done;
		}

		memcpy(sss->buf1, authoritative_data, len);

		/*
		 * Just like strcoll(), strxfrm() expects a NUL-terminated string. Not
		 * necessary for ICU, but doesn't hurt.
		 */
		sss->buf1[len] = '\0';
		sss->last_len1 = len;

#ifdef USE_ICU
		/* When using ICU and not UTF8, convert string to UChar. */
		if (sss->locale && sss->locale->provider == COLLPROVIDER_ICU &&
			GetDatabaseEncoding() != PG_UTF8)
			ulen = icu_to_uchar(&uchar, sss->buf1, len);
#endif

		/*
		 * Loop: Call strxfrm() or ucol_getSortKey(), possibly enlarge buffer,
		 * and try again.  Both of these functions have the result buffer
		 * content undefined if the result did not fit, so we need to retry
		 * until everything fits, even though we only need the first few bytes
		 * in the end.  When using ucol_nextSortKeyPart(), however, we only
		 * ask for as many bytes as we actually need.
		 */
		for (;;)
		{
#ifdef USE_ICU
			if (sss->locale && sss->locale->provider == COLLPROVIDER_ICU)
			{
				/*
				 * When using UTF8, use the iteration interface so we only
				 * need to produce as many bytes as we actually need.
				 */
				if (GetDatabaseEncoding() == PG_UTF8)
				{
					UCharIterator iter;
					uint32_t	state[2];
					UErrorCode	status;

					uiter_setUTF8(&iter, sss->buf1, len);
					state[0] = state[1] = 0;	/* won't need that again */
					status = U_ZERO_ERROR;
					bsize = ucol_nextSortKeyPart(sss->locale->info.icu.ucol,
												 &iter,
												 state,
												 (uint8_t *) sss->buf2,
												 Min(sizeof(Datum), sss->buflen2),
												 &status);
					if (U_FAILURE(status))
						ereport(ERROR,
								(errmsg("sort key generation failed: %s",
										u_errorName(status))));
				}
				else
					bsize = ucol_getSortKey(sss->locale->info.icu.ucol,
											uchar, ulen,
											(uint8_t *) sss->buf2, sss->buflen2);
			}
			else
#endif
#ifdef HAVE_LOCALE_T
			if (sss->locale && sss->locale->provider == COLLPROVIDER_LIBC)
				bsize = strxfrm_l(sss->buf2, sss->buf1,
								  sss->buflen2, sss->locale->info.lt);
			else
#endif
				bsize = strxfrm(sss->buf2, sss->buf1, sss->buflen2);

			sss->last_len2 = bsize;
			if (bsize < sss->buflen2)
				break;

			/*
			 * Grow buffer and retry.
			 */
			pfree(sss->buf2);
			sss->buflen2 = Max(bsize + 1,
							   Min(sss->buflen2 * 2, MaxAllocSize));
			sss->buf2 = palloc(sss->buflen2);
		}

		/*
		 * Every Datum byte is always compared.  This is safe because the
		 * strxfrm() blob is itself NUL terminated, leaving no danger of
		 * misinterpreting any NUL bytes not intended to be interpreted as
		 * logically representing termination.
		 *
		 * (Actually, even if there were NUL bytes in the blob it would be
		 * okay.  See remarks on bytea case above.)
		 */
		memcpy(pres, sss->buf2, Min(sizeof(Datum), bsize));

#ifdef USE_ICU
		if (uchar)
			pfree(uchar);
#endif
	}

	/*
	 * Maintain approximate cardinality of both abbreviated keys and original,
	 * authoritative keys using HyperLogLog.  Used as cheap insurance against
	 * the worst case, where we do many string transformations for no saving
	 * in full strcoll()-based comparisons.  These statistics are used by
	 * varstr_abbrev_abort().
	 *
	 * First, Hash key proper, or a significant fraction of it.  Mix in length
	 * in order to compensate for cases where differences are past
	 * PG_CACHE_LINE_SIZE bytes, so as to limit the overhead of hashing.
	 */
	hash = DatumGetUInt32(hash_any((unsigned char *) authoritative_data,
								   Min(len, PG_CACHE_LINE_SIZE)));

	if (len > PG_CACHE_LINE_SIZE)
		hash ^= DatumGetUInt32(hash_uint32((uint32) len));

	addHyperLogLog(&sss->full_card, hash);

	/* Hash abbreviated key */
#if SIZEOF_DATUM == 8
	{
		uint32		lohalf,
					hihalf;

		lohalf = (uint32) res;
		hihalf = (uint32) (res >> 32);
		hash = DatumGetUInt32(hash_uint32(lohalf ^ hihalf));
	}
#else							/* SIZEOF_DATUM != 8 */
	hash = DatumGetUInt32(hash_uint32((uint32) res));
#endif

	addHyperLogLog(&sss->abbr_card, hash);

	/* Cache result, perhaps saving an expensive strxfrm() call next time */
	sss->cache_blob = true;
done:

	/*
	 * Byteswap on little-endian machines.
	 *
	 * This is needed so that varstrcmp_abbrev() (an unsigned integer 3-way
	 * comparator) works correctly on all platforms.  If we didn't do this,
	 * the comparator would have to call memcmp() with a pair of pointers to
	 * the first byte of each abbreviated key, which is slower.
	 */
	res = DatumBigEndianToNative(res);

	/* Don't leak memory here */
	if (PointerGetDatum(authoritative) != original)
		pfree(authoritative);

	return res;
}

/*
 * Callback for estimating effectiveness of abbreviated key optimization, using
 * heuristic rules.  Returns value indicating if the abbreviation optimization
 * should be aborted, based on its projected effectiveness.
 */
static bool
varstr_abbrev_abort(int memtupcount, SortSupport ssup)
{
	VarStringSortSupport *sss = (VarStringSortSupport *) ssup->ssup_extra;
	double		abbrev_distinct,
				key_distinct;

	Assert(ssup->abbreviate);

	/* Have a little patience */
	if (memtupcount < 100)
		return false;

	abbrev_distinct = estimateHyperLogLog(&sss->abbr_card);
	key_distinct = estimateHyperLogLog(&sss->full_card);

	/*
	 * Clamp cardinality estimates to at least one distinct value.  While
	 * NULLs are generally disregarded, if only NULL values were seen so far,
	 * that might misrepresent costs if we failed to clamp.
	 */
	if (abbrev_distinct <= 1.0)
		abbrev_distinct = 1.0;

	if (key_distinct <= 1.0)
		key_distinct = 1.0;

	/*
	 * In the worst case all abbreviated keys are identical, while at the same
	 * time there are differences within full key strings not captured in
	 * abbreviations.
	 */
#ifdef TRACE_SORT
	if (trace_sort)
	{
		double		norm_abbrev_card = abbrev_distinct / (double) memtupcount;

		elog(LOG, "varstr_abbrev: abbrev_distinct after %d: %f "
			 "(key_distinct: %f, norm_abbrev_card: %f, prop_card: %f)",
			 memtupcount, abbrev_distinct, key_distinct, norm_abbrev_card,
			 sss->prop_card);
	}
#endif

	/*
	 * If the number of distinct abbreviated keys approximately matches the
	 * number of distinct authoritative original keys, that's reason enough to
	 * proceed.  We can win even with a very low cardinality set if most
	 * tie-breakers only memcmp().  This is by far the most important
	 * consideration.
	 *
	 * While comparisons that are resolved at the abbreviated key level are
	 * considerably cheaper than tie-breakers resolved with memcmp(), both of
	 * those two outcomes are so much cheaper than a full strcoll() once
	 * sorting is underway that it doesn't seem worth it to weigh abbreviated
	 * cardinality against the overall size of the set in order to more
	 * accurately model costs.  Assume that an abbreviated comparison, and an
	 * abbreviated comparison with a cheap memcmp()-based authoritative
	 * resolution are equivalent.
	 */
	if (abbrev_distinct > key_distinct * sss->prop_card)
	{
		/*
		 * When we have exceeded 10,000 tuples, decay required cardinality
		 * aggressively for next call.
		 *
		 * This is useful because the number of comparisons required on
		 * average increases at a linearithmic rate, and at roughly 10,000
		 * tuples that factor will start to dominate over the linear costs of
		 * string transformation (this is a conservative estimate).  The decay
		 * rate is chosen to be a little less aggressive than halving -- which
		 * (since we're called at points at which memtupcount has doubled)
		 * would never see the cost model actually abort past the first call
		 * following a decay.  This decay rate is mostly a precaution against
		 * a sudden, violent swing in how well abbreviated cardinality tracks
		 * full key cardinality.  The decay also serves to prevent a marginal
		 * case from being aborted too late, when too much has already been
		 * invested in string transformation.
		 *
		 * It's possible for sets of several million distinct strings with
		 * mere tens of thousands of distinct abbreviated keys to still
		 * benefit very significantly.  This will generally occur provided
		 * each abbreviated key is a proxy for a roughly uniform number of the
		 * set's full keys. If it isn't so, we hope to catch that early and
		 * abort.  If it isn't caught early, by the time the problem is
		 * apparent it's probably not worth aborting.
		 */
		if (memtupcount > 10000)
			sss->prop_card *= 0.65;

		return false;
	}

	/*
	 * Abort abbreviation strategy.
	 *
	 * The worst case, where all abbreviated keys are identical while all
	 * original strings differ will typically only see a regression of about
	 * 10% in execution time for small to medium sized lists of strings.
	 * Whereas on modern CPUs where cache stalls are the dominant cost, we can
	 * often expect very large improvements, particularly with sets of strings
	 * of moderately high to high abbreviated cardinality.  There is little to
	 * lose but much to gain, which our strategy reflects.
	 */
#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "varstr_abbrev: aborted abbreviation at %d "
			 "(abbrev_distinct: %f, key_distinct: %f, prop_card: %f)",
			 memtupcount, abbrev_distinct, key_distinct, sss->prop_card);
#endif

	return true;
}

/*
 * Generic equalimage support function for character type's operator classes.
 * Disables the use of deduplication with nondeterministic collations.
 */
Datum
btvarstrequalimage(PG_FUNCTION_ARGS)
{
	/* Oid		opcintype = PG_GETARG_OID(0); */
	Oid			collid = PG_GET_COLLATION();

	check_collation_set(collid);

	if (lc_collate_is_c(collid) ||
		collid == DEFAULT_COLLATION_OID ||
		get_collation_isdeterministic(collid))
		PG_RETURN_BOOL(true);
	else
		PG_RETURN_BOOL(false);
}

Datum
text_larger(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	text	   *result;

	result = ((text_cmp(arg1, arg2, PG_GET_COLLATION()) > 0) ? arg1 : arg2);

	PG_RETURN_TEXT_P(result);
}

Datum
text_smaller(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	text	   *result;

	result = ((text_cmp(arg1, arg2, PG_GET_COLLATION()) < 0) ? arg1 : arg2);

	PG_RETURN_TEXT_P(result);
}


/*
 * Cross-type comparison functions for types text and name.
 */

Datum
nameeqtext(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	size_t		len1 = strlen(NameStr(*arg1));
	size_t		len2 = VARSIZE_ANY_EXHDR(arg2);
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = (len1 == len2 &&
				  memcmp(NameStr(*arg1), VARDATA_ANY(arg2), len1) == 0);
	else
		result = (varstr_cmp(NameStr(*arg1), len1,
							 VARDATA_ANY(arg2), len2,
							 collid) == 0);

	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
texteqname(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	Name		arg2 = PG_GETARG_NAME(1);
	size_t		len1 = VARSIZE_ANY_EXHDR(arg1);
	size_t		len2 = strlen(NameStr(*arg2));
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = (len1 == len2 &&
				  memcmp(VARDATA_ANY(arg1), NameStr(*arg2), len1) == 0);
	else
		result = (varstr_cmp(VARDATA_ANY(arg1), len1,
							 NameStr(*arg2), len2,
							 collid) == 0);

	PG_FREE_IF_COPY(arg1, 0);

	PG_RETURN_BOOL(result);
}

Datum
namenetext(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	size_t		len1 = strlen(NameStr(*arg1));
	size_t		len2 = VARSIZE_ANY_EXHDR(arg2);
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = !(len1 == len2 &&
				   memcmp(NameStr(*arg1), VARDATA_ANY(arg2), len1) == 0);
	else
		result = !(varstr_cmp(NameStr(*arg1), len1,
							  VARDATA_ANY(arg2), len2,
							  collid) == 0);

	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
textnename(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	Name		arg2 = PG_GETARG_NAME(1);
	size_t		len1 = VARSIZE_ANY_EXHDR(arg1);
	size_t		len2 = strlen(NameStr(*arg2));
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = !(len1 == len2 &&
				   memcmp(VARDATA_ANY(arg1), NameStr(*arg2), len1) == 0);
	else
		result = !(varstr_cmp(VARDATA_ANY(arg1), len1,
							  NameStr(*arg2), len2,
							  collid) == 0);

	PG_FREE_IF_COPY(arg1, 0);

	PG_RETURN_BOOL(result);
}

Datum
btnametextcmp(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		result;

	result = varstr_cmp(NameStr(*arg1), strlen(NameStr(*arg1)),
						VARDATA_ANY(arg2), VARSIZE_ANY_EXHDR(arg2),
						PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

Datum
bttextnamecmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	Name		arg2 = PG_GETARG_NAME(1);
	int32		result;

	result = varstr_cmp(VARDATA_ANY(arg1), VARSIZE_ANY_EXHDR(arg1),
						NameStr(*arg2), strlen(NameStr(*arg2)),
						PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);

	PG_RETURN_INT32(result);
}

#define CmpCall(cmpfunc) \
	DatumGetInt32(DirectFunctionCall2Coll(cmpfunc, \
										  PG_GET_COLLATION(), \
										  PG_GETARG_DATUM(0), \
										  PG_GETARG_DATUM(1)))

Datum
namelttext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) < 0);
}

Datum
nameletext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) <= 0);
}

Datum
namegttext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) > 0);
}

Datum
namegetext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) >= 0);
}

Datum
textltname(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) < 0);
}

Datum
textlename(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) <= 0);
}

Datum
textgtname(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) > 0);
}

Datum
textgename(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) >= 0);
}

#undef CmpCall


/*
 * The following operators support character-by-character comparison
 * of text datums, to allow building indexes suitable for LIKE clauses.
 * Note that the regular texteq/textne comparison operators, and regular
 * support functions 1 and 2 with "C" collation are assumed to be
 * compatible with these!
 */

static int
internal_text_pattern_compare(text *arg1, text *arg2)
{
	int			result;
	int			len1,
				len2;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	result = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));
	if (result != 0)
		return result;
	else if (len1 < len2)
		return -1;
	else if (len1 > len2)
		return 1;
	else
		return 0;
}


Datum
text_pattern_lt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result < 0);
}


Datum
text_pattern_le(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result <= 0);
}


Datum
text_pattern_ge(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result >= 0);
}


Datum
text_pattern_gt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result > 0);
}


Datum
bttext_pattern_cmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}


Datum
bttext_pattern_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* Use generic string SortSupport, forcing "C" collation */
	varstr_sortsupport(ssup, TEXTOID, C_COLLATION_OID);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
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
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);

	PG_RETURN_BYTEA_P(bytea_catenate(t1, t2));
}

/*
 * bytea_catenate
 *	Guts of byteacat(), broken out so it can be used by other functions
 *
 * Arguments can be in short-header form, but not compressed or out-of-line
 */
static bytea *
bytea_catenate(bytea *t1, bytea *t2)
{
	bytea	   *result;
	int			len1,
				len2,
				len;
	char	   *ptr;

	len1 = VARSIZE_ANY_EXHDR(t1);
	len2 = VARSIZE_ANY_EXHDR(t2);

	/* paranoia ... probably should throw error instead? */
	if (len1 < 0)
		len1 = 0;
	if (len2 < 0)
		len2 = 0;

	len = len1 + len2 + VARHDRSZ;
	result = (bytea *) palloc(len);

	/* Set size of result string... */
	SET_VARSIZE(result, len);

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	if (len1 > 0)
		memcpy(ptr, VARDATA_ANY(t1), len1);
	if (len2 > 0)
		memcpy(ptr + len1, VARDATA_ANY(t2), len2);

	return result;
}

#define PG_STR_GET_BYTEA(str_) \
	DatumGetByteaPP(DirectFunctionCall1(byteain, CStringGetDatum(str_)))

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
 * adjusting the length to be consistent with the "negative start" per SQL.
 * If the length is less than zero, an ERROR is thrown. If no third argument
 * (length) is provided, the length to the end of the string is assumed.
 */
Datum
bytea_substr(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(bytea_substring(PG_GETARG_DATUM(0),
									  PG_GETARG_INT32(1),
									  PG_GETARG_INT32(2),
									  false));
}

/*
 * bytea_substr_no_len -
 *	  Wrapper to avoid opr_sanity failure due to
 *	  one function accepting a different number of args.
 */
Datum
bytea_substr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(bytea_substring(PG_GETARG_DATUM(0),
									  PG_GETARG_INT32(1),
									  -1,
									  true));
}

static bytea *
bytea_substring(Datum str,
				int S,
				int L,
				bool length_not_specified)
{
	int			S1;				/* adjusted start position */
	int			L1;				/* adjusted substring length */

	S1 = Max(S, 1);

	if (length_not_specified)
	{
		/*
		 * Not passed a length - DatumGetByteaPSlice() grabs everything to the
		 * end of the string if we pass it a negative value for length.
		 */
		L1 = -1;
	}
	else
	{
		/* end position */
		int			E = S + L;

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
			return PG_STR_GET_BYTEA("");

		L1 = E - S1;
	}

	/*
	 * If the start position is past the end of the string, SQL99 says to
	 * return a zero-length string -- DatumGetByteaPSlice() will do that for
	 * us. Convert to zero-based starting position
	 */
	return DatumGetByteaPSlice(str, S1 - 1, L1);
}

/*
 * byteaoverlay
 *	Replace specified substring of first string with second
 *
 * The SQL standard defines OVERLAY() in terms of substring and concatenation.
 * This code is a direct implementation of what the standard says.
 */
Datum
byteaoverlay(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* substring start position */
	int			sl = PG_GETARG_INT32(3);	/* substring length */

	PG_RETURN_BYTEA_P(bytea_overlay(t1, t2, sp, sl));
}

Datum
byteaoverlay_no_len(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* substring start position */
	int			sl;

	sl = VARSIZE_ANY_EXHDR(t2); /* defaults to length(t2) */
	PG_RETURN_BYTEA_P(bytea_overlay(t1, t2, sp, sl));
}

static bytea *
bytea_overlay(bytea *t1, bytea *t2, int sp, int sl)
{
	bytea	   *result;
	bytea	   *s1;
	bytea	   *s2;
	int			sp_pl_sl;

	/*
	 * Check for possible integer-overflow cases.  For negative sp, throw a
	 * "substring length" error because that's what should be expected
	 * according to the spec's definition of OVERLAY().
	 */
	if (sp <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
	if (pg_add_s32_overflow(sp, sl, &sp_pl_sl))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	s1 = bytea_substring(PointerGetDatum(t1), 1, sp - 1, false);
	s2 = bytea_substring(PointerGetDatum(t1), sp_pl_sl, -1, true);
	result = bytea_catenate(s1, t2);
	result = bytea_catenate(result, s2);

	return result;
}

/*
 * byteapos -
 *	  Return the position of the specified substring.
 *	  Implements the SQL POSITION() function.
 * Cloned from textpos and modified as required.
 */
Datum
byteapos(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);
	int			pos;
	int			px,
				p;
	int			len1,
				len2;
	char	   *p1,
			   *p2;

	len1 = VARSIZE_ANY_EXHDR(t1);
	len2 = VARSIZE_ANY_EXHDR(t2);

	if (len2 <= 0)
		PG_RETURN_INT32(1);		/* result for empty pattern */

	p1 = VARDATA_ANY(t1);
	p2 = VARDATA_ANY(t2);

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
	bytea	   *v = PG_GETARG_BYTEA_PP(0);
	int32		n = PG_GETARG_INT32(1);
	int			len;
	int			byte;

	len = VARSIZE_ANY_EXHDR(v);

	if (n < 0 || n >= len)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len - 1)));

	byte = ((unsigned char *) VARDATA_ANY(v))[n];

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
	bytea	   *v = PG_GETARG_BYTEA_PP(0);
	int64		n = PG_GETARG_INT64(1);
	int			byteNo,
				bitNo;
	int			len;
	int			byte;

	len = VARSIZE_ANY_EXHDR(v);

	if (n < 0 || n >= (int64) len * 8)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %lld out of valid range, 0..%lld",
						(long long) n, (long long) len * 8 - 1)));

	/* n/8 is now known < len, so safe to cast to int */
	byteNo = (int) (n / 8);
	bitNo = (int) (n % 8);

	byte = ((unsigned char *) VARDATA_ANY(v))[byteNo];

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
	bytea	   *res = PG_GETARG_BYTEA_P_COPY(0);
	int32		n = PG_GETARG_INT32(1);
	int32		newByte = PG_GETARG_INT32(2);
	int			len;

	len = VARSIZE(res) - VARHDRSZ;

	if (n < 0 || n >= len)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len - 1)));

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
	bytea	   *res = PG_GETARG_BYTEA_P_COPY(0);
	int64		n = PG_GETARG_INT64(1);
	int32		newBit = PG_GETARG_INT32(2);
	int			len;
	int			oldByte,
				newByte;
	int			byteNo,
				bitNo;

	len = VARSIZE(res) - VARHDRSZ;

	if (n < 0 || n >= (int64) len * 8)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %lld out of valid range, 0..%lld",
						(long long) n, (long long) len * 8 - 1)));

	/* n/8 is now known < len, so safe to cast to int */
	byteNo = (int) (n / 8);
	bitNo = (int) (n % 8);

	/*
	 * sanity check!
	 */
	if (newBit != 0 && newBit != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("new bit must be 0 or 1")));

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
	text	   *s = PG_GETARG_TEXT_PP(0);
	Name		result;
	int			len;

	len = VARSIZE_ANY_EXHDR(s);

	/* Truncate oversize input */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(VARDATA_ANY(s), len, NAMEDATALEN - 1);

	/* We use palloc0 here to ensure result is zero-padded */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), VARDATA_ANY(s), len);

	PG_RETURN_NAME(result);
}

/* name_text()
 * Converts a Name type to a text type.
 */
Datum
name_text(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_TEXT_P(cstring_to_text(NameStr(*s)));
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
	rawname = text_to_cstring(textval);

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
 * other situations such as parsing GUC variables.  In the GUC case, it's
 * important to avoid memory leaks, so the API is designed to minimize the
 * amount of stuff that needs to be allocated and freed.
 *
 * Inputs:
 *	rawstring: the input string; must be overwritable!	On return, it's
 *			   been modified to contain the separated identifiers.
 *	separator: the separator punctuation expected between identifiers
 *			   (typically '.' or ',').  Whitespace may also appear around
 *			   identifiers.
 * Outputs:
 *	namelist: filled with a palloc'd list of pointers to identifiers within
 *			  rawstring.  Caller should list_free() this even on error return.
 *
 * Returns true if okay, false if there is a syntax error in the string.
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

	while (scanner_isspace(*nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* Quoted name --- collapse quote-quote pairs, no downcasing */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* mismatched quotes */
				if (endp[1] != '"')
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
				   !scanner_isspace(*nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* empty unquoted name not allowed */

			/*
			 * Downcase the identifier, using same code as main lexer does.
			 *
			 * XXX because we want to overwrite the input in-place, we cannot
			 * support a downcasing transformation that increases the string
			 * length.  This is not a problem given the current implementation
			 * of downcase_truncate_identifier, but we'll probably have to do
			 * something about this someday.
			 */
			len = endp - curname;
			downname = downcase_truncate_identifier(curname, len, false);
			Assert(strlen(downname) <= len);
			strncpy(curname, downname, len);	/* strncpy is required here */
			pfree(downname);
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
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


/*
 * SplitDirectoriesString --- parse a string containing file/directory names
 *
 * This works fine on file names too; the function name is historical.
 *
 * This is similar to SplitIdentifierString, except that the parsing
 * rules are meant to handle pathnames instead of identifiers: there is
 * no downcasing, embedded spaces are allowed, the max length is MAXPGPATH-1,
 * and we apply canonicalize_path() to each extracted string.  Because of the
 * last, the returned strings are separately palloc'd rather than being
 * pointers into rawstring --- but we still scribble on rawstring.
 *
 * Inputs:
 *	rawstring: the input string; must be modifiable!
 *	separator: the separator punctuation expected between directories
 *			   (typically ',' or ';').  Whitespace may also appear around
 *			   directories.
 * Outputs:
 *	namelist: filled with a palloc'd list of directory names.
 *			  Caller should list_free_deep() this even on error return.
 *
 * Returns true if okay, false if there is a syntax error in the string.
 *
 * Note that an empty string is considered okay here.
 */
bool
SplitDirectoriesString(char *rawstring, char separator,
					   List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (scanner_isspace(*nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new directory. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* Quoted name --- collapse quote-quote pairs */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* mismatched quotes */
				if (endp[1] != '"')
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
			/* Unquoted name --- extends to separator or end of string */
			curname = endp = nextp;
			while (*nextp && *nextp != separator)
			{
				/* trailing whitespace should not be included in name */
				if (!scanner_isspace(*nextp))
					endp = nextp + 1;
				nextp++;
			}
			if (curname == endp)
				return false;	/* empty unquoted name not allowed */
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Now safe to overwrite separator with a null */
		*endp = '\0';

		/* Truncate path if it's overlength */
		if (strlen(curname) >= MAXPGPATH)
			curname[MAXPGPATH - 1] = '\0';

		/*
		 * Finished isolating current name --- add it to list
		 */
		curname = pstrdup(curname);
		canonicalize_path(curname);
		*namelist = lappend(*namelist, curname);

		/* Loop back if we didn't reach end of string */
	} while (!done);

	return true;
}


/*
 * SplitGUCList --- parse a string containing identifiers or file names
 *
 * This is used to split the value of a GUC_LIST_QUOTE GUC variable, without
 * presuming whether the elements will be taken as identifiers or file names.
 * We assume the input has already been through flatten_set_variable_args(),
 * so that we need never downcase (if appropriate, that was done already).
 * Nor do we ever truncate, since we don't know the correct max length.
 * We disallow embedded whitespace for simplicity (it shouldn't matter,
 * because any embedded whitespace should have led to double-quoting).
 * Otherwise the API is identical to SplitIdentifierString.
 *
 * XXX it's annoying to have so many copies of this string-splitting logic.
 * However, it's not clear that having one function with a bunch of option
 * flags would be much better.
 *
 * XXX there is a version of this function in src/bin/pg_dump/dumputils.c.
 * Be sure to update that if you have to change this.
 *
 * Inputs:
 *	rawstring: the input string; must be overwritable!	On return, it's
 *			   been modified to contain the separated identifiers.
 *	separator: the separator punctuation expected between identifiers
 *			   (typically '.' or ',').  Whitespace may also appear around
 *			   identifiers.
 * Outputs:
 *	namelist: filled with a palloc'd list of pointers to identifiers within
 *			  rawstring.  Caller should list_free() this even on error return.
 *
 * Returns true if okay, false if there is a syntax error in the string.
 */
bool
SplitGUCList(char *rawstring, char separator,
			 List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (scanner_isspace(*nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* Quoted name --- collapse quote-quote pairs */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* mismatched quotes */
				if (endp[1] != '"')
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
			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !scanner_isspace(*nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* empty unquoted name not allowed */
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Now safe to overwrite separator with a null */
		*endp = '\0';

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
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	bool		result;
	Size		len1,
				len2;

	/*
	 * We can use a fast path for unequal lengths, which might save us from
	 * having to detoast one or both values.
	 */
	len1 = toast_raw_datum_size(arg1);
	len2 = toast_raw_datum_size(arg2);
	if (len1 != len2)
		result = false;
	else
	{
		bytea	   *barg1 = DatumGetByteaPP(arg1);
		bytea	   *barg2 = DatumGetByteaPP(arg2);

		result = (memcmp(VARDATA_ANY(barg1), VARDATA_ANY(barg2),
						 len1 - VARHDRSZ) == 0);

		PG_FREE_IF_COPY(barg1, 0);
		PG_FREE_IF_COPY(barg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
byteane(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	bool		result;
	Size		len1,
				len2;

	/*
	 * We can use a fast path for unequal lengths, which might save us from
	 * having to detoast one or both values.
	 */
	len1 = toast_raw_datum_size(arg1);
	len2 = toast_raw_datum_size(arg2);
	if (len1 != len2)
		result = true;
	else
	{
		bytea	   *barg1 = DatumGetByteaPP(arg1);
		bytea	   *barg2 = DatumGetByteaPP(arg2);

		result = (memcmp(VARDATA_ANY(barg1), VARDATA_ANY(barg2),
						 len1 - VARHDRSZ) != 0);

		PG_FREE_IF_COPY(barg1, 0);
		PG_FREE_IF_COPY(barg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
bytealt(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp < 0) || ((cmp == 0) && (len1 < len2)));
}

Datum
byteale(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp < 0) || ((cmp == 0) && (len1 <= len2)));
}

Datum
byteagt(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp > 0) || ((cmp == 0) && (len1 > len2)));
}

Datum
byteage(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp > 0) || ((cmp == 0) && (len1 >= len2)));
}

Datum
byteacmp(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));
	if ((cmp == 0) && (len1 != len2))
		cmp = (len1 < len2) ? -1 : 1;

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}

Datum
bytea_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* Use generic string SortSupport, forcing "C" collation */
	varstr_sortsupport(ssup, BYTEAOID, C_COLLATION_OID);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

/*
 * appendStringInfoText
 *
 * Append a text to str.
 * Like appendStringInfoString(str, text_to_cstring(t)) but faster.
 */
static void
appendStringInfoText(StringInfo str, const text *t)
{
	appendBinaryStringInfo(str, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
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
	text	   *src_text = PG_GETARG_TEXT_PP(0);
	text	   *from_sub_text = PG_GETARG_TEXT_PP(1);
	text	   *to_sub_text = PG_GETARG_TEXT_PP(2);
	int			src_text_len;
	int			from_sub_text_len;
	TextPositionState state;
	text	   *ret_text;
	int			chunk_len;
	char	   *curr_ptr;
	char	   *start_ptr;
	StringInfoData str;
	bool		found;

	src_text_len = VARSIZE_ANY_EXHDR(src_text);
	from_sub_text_len = VARSIZE_ANY_EXHDR(from_sub_text);

	/* Return unmodified source string if empty source or pattern */
	if (src_text_len < 1 || from_sub_text_len < 1)
	{
		PG_RETURN_TEXT_P(src_text);
	}

	text_position_setup(src_text, from_sub_text, PG_GET_COLLATION(), &state);

	found = text_position_next(&state);

	/* When the from_sub_text is not found, there is nothing to do. */
	if (!found)
	{
		text_position_cleanup(&state);
		PG_RETURN_TEXT_P(src_text);
	}
	curr_ptr = text_position_get_match_ptr(&state);
	start_ptr = VARDATA_ANY(src_text);

	initStringInfo(&str);

	do
	{
		CHECK_FOR_INTERRUPTS();

		/* copy the data skipped over by last text_position_next() */
		chunk_len = curr_ptr - start_ptr;
		appendBinaryStringInfo(&str, start_ptr, chunk_len);

		appendStringInfoText(&str, to_sub_text);

		start_ptr = curr_ptr + from_sub_text_len;

		found = text_position_next(&state);
		if (found)
			curr_ptr = text_position_get_match_ptr(&state);
	}
	while (found);

	/* copy trailing data */
	chunk_len = ((char *) src_text + VARSIZE_ANY(src_text)) - start_ptr;
	appendBinaryStringInfo(&str, start_ptr, chunk_len);

	text_position_cleanup(&state);

	ret_text = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

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
	const char *p = VARDATA_ANY(replace_text);
	const char *p_end = p + VARSIZE_ANY_EXHDR(replace_text);

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
 * \n escapes.  start_ptr is the start of the match in the source string,
 * at logical character position data_pos.
 */
static void
appendStringInfoRegexpSubstr(StringInfo str, text *replace_text,
							 regmatch_t *pmatch,
							 char *start_ptr, int data_pos)
{
	const char *p = VARDATA_ANY(replace_text);
	const char *p_end = p + VARSIZE_ANY_EXHDR(replace_text);
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
			 * Copy the text that is back reference of regexp.  Note so and eo
			 * are counted in characters not bytes.
			 */
			char	   *chunk_start;
			int			chunk_len;

			Assert(so >= data_pos);
			chunk_start = start_ptr;
			chunk_start += charlen_to_bytelen(chunk_start, so - data_pos);
			chunk_len = charlen_to_bytelen(chunk_start, eo - so);
			appendBinaryStringInfo(str, chunk_start, chunk_len);
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
	int			src_text_len = VARSIZE_ANY_EXHDR(src_text);
	StringInfoData buf;
	regmatch_t	pmatch[REGEXP_REPLACE_BACKREF_CNT];
	pg_wchar   *data;
	size_t		data_len;
	int			search_start;
	int			data_pos;
	char	   *start_ptr;
	bool		have_escape;

	initStringInfo(&buf);

	/* Convert data string to wide characters. */
	data = (pg_wchar *) palloc((src_text_len + 1) * sizeof(pg_wchar));
	data_len = pg_mb2wchar_with_len(VARDATA_ANY(src_text), data, src_text_len);

	/* Check whether replace_text has escape char. */
	have_escape = check_replace_text_has_escape_char(replace_text);

	/* start_ptr points to the data_pos'th character of src_text */
	start_ptr = (char *) VARDATA_ANY(src_text);
	data_pos = 0;

	search_start = 0;
	while (search_start <= data_len)
	{
		int			regexec_result;

		CHECK_FOR_INTERRUPTS();

		regexec_result = pg_regexec(re,
									data,
									data_len,
									search_start,
									NULL,	/* no details */
									REGEXP_REPLACE_BACKREF_CNT,
									pmatch,
									0);

		if (regexec_result == REG_NOMATCH)
			break;

		if (regexec_result != REG_OKAY)
		{
			char		errMsg[100];

			CHECK_FOR_INTERRUPTS();
			pg_regerror(regexec_result, re, errMsg, sizeof(errMsg));
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("regular expression failed: %s", errMsg)));
		}

		/*
		 * Copy the text to the left of the match position.  Note we are given
		 * character not byte indexes.
		 */
		if (pmatch[0].rm_so - data_pos > 0)
		{
			int			chunk_len;

			chunk_len = charlen_to_bytelen(start_ptr,
										   pmatch[0].rm_so - data_pos);
			appendBinaryStringInfo(&buf, start_ptr, chunk_len);

			/*
			 * Advance start_ptr over that text, to avoid multiple rescans of
			 * it if the replace_text contains multiple back-references.
			 */
			start_ptr += chunk_len;
			data_pos = pmatch[0].rm_so;
		}

		/*
		 * Copy the replace_text. Process back references when the
		 * replace_text has escape characters.
		 */
		if (have_escape)
			appendStringInfoRegexpSubstr(&buf, replace_text, pmatch,
										 start_ptr, data_pos);
		else
			appendStringInfoText(&buf, replace_text);

		/* Advance start_ptr and data_pos over the matched text. */
		start_ptr += charlen_to_bytelen(start_ptr,
										pmatch[0].rm_eo - data_pos);
		data_pos = pmatch[0].rm_eo;

		/*
		 * When global option is off, replace the first instance only.
		 */
		if (!glob)
			break;

		/*
		 * Advance search position.  Normally we start the next search at the
		 * end of the previous match; but if the match was of zero length, we
		 * have to advance by one character, or we'd just find the same match
		 * again.
		 */
		search_start = data_pos;
		if (pmatch[0].rm_so == pmatch[0].rm_eo)
			search_start++;
	}

	/*
	 * Copy the text to the right of the last match.
	 */
	if (data_pos < data_len)
	{
		int			chunk_len;

		chunk_len = ((char *) src_text + VARSIZE_ANY(src_text)) - start_ptr;
		appendBinaryStringInfo(&buf, start_ptr, chunk_len);
	}

	ret_text = cstring_to_text_with_len(buf.data, buf.len);
	pfree(buf.data);
	pfree(data);

	return ret_text;
}

/*
 * split_part
 * parse input string based on provided field separator
 * return N'th item (1 based, negative counts from end)
 */
Datum
split_part(PG_FUNCTION_ARGS)
{
	text	   *inputstring = PG_GETARG_TEXT_PP(0);
	text	   *fldsep = PG_GETARG_TEXT_PP(1);
	int			fldnum = PG_GETARG_INT32(2);
	int			inputstring_len;
	int			fldsep_len;
	TextPositionState state;
	char	   *start_ptr;
	char	   *end_ptr;
	text	   *result_text;
	bool		found;

	/* field number is 1 based */
	if (fldnum == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field position must not be zero")));

	inputstring_len = VARSIZE_ANY_EXHDR(inputstring);
	fldsep_len = VARSIZE_ANY_EXHDR(fldsep);

	/* return empty string for empty input string */
	if (inputstring_len < 1)
		PG_RETURN_TEXT_P(cstring_to_text(""));

	/* handle empty field separator */
	if (fldsep_len < 1)
	{
		/* if first or last field, return input string, else empty string */
		if (fldnum == 1 || fldnum == -1)
			PG_RETURN_TEXT_P(inputstring);
		else
			PG_RETURN_TEXT_P(cstring_to_text(""));
	}

	/* find the first field separator */
	text_position_setup(inputstring, fldsep, PG_GET_COLLATION(), &state);

	found = text_position_next(&state);

	/* special case if fldsep not found at all */
	if (!found)
	{
		text_position_cleanup(&state);
		/* if first or last field, return input string, else empty string */
		if (fldnum == 1 || fldnum == -1)
			PG_RETURN_TEXT_P(inputstring);
		else
			PG_RETURN_TEXT_P(cstring_to_text(""));
	}

	/*
	 * take care of a negative field number (i.e. count from the right) by
	 * converting to a positive field number; we need total number of fields
	 */
	if (fldnum < 0)
	{
		/* we found a fldsep, so there are at least two fields */
		int			numfields = 2;

		while (text_position_next(&state))
			numfields++;

		/* special case of last field does not require an extra pass */
		if (fldnum == -1)
		{
			start_ptr = text_position_get_match_ptr(&state) + fldsep_len;
			end_ptr = VARDATA_ANY(inputstring) + inputstring_len;
			text_position_cleanup(&state);
			PG_RETURN_TEXT_P(cstring_to_text_with_len(start_ptr,
													  end_ptr - start_ptr));
		}

		/* else, convert fldnum to positive notation */
		fldnum += numfields + 1;

		/* if nonexistent field, return empty string */
		if (fldnum <= 0)
		{
			text_position_cleanup(&state);
			PG_RETURN_TEXT_P(cstring_to_text(""));
		}

		/* reset to pointing at first match, but now with positive fldnum */
		text_position_reset(&state);
		found = text_position_next(&state);
		Assert(found);
	}

	/* identify bounds of first field */
	start_ptr = VARDATA_ANY(inputstring);
	end_ptr = text_position_get_match_ptr(&state);

	while (found && --fldnum > 0)
	{
		/* identify bounds of next field */
		start_ptr = end_ptr + fldsep_len;
		found = text_position_next(&state);
		if (found)
			end_ptr = text_position_get_match_ptr(&state);
	}

	text_position_cleanup(&state);

	if (fldnum > 0)
	{
		/* N'th field separator not found */
		/* if last field requested, return it, else empty string */
		if (fldnum == 1)
		{
			int			last_len = start_ptr - VARDATA_ANY(inputstring);

			result_text = cstring_to_text_with_len(start_ptr,
												   inputstring_len - last_len);
		}
		else
			result_text = cstring_to_text("");
	}
	else
	{
		/* non-last field requested */
		result_text = cstring_to_text_with_len(start_ptr, end_ptr - start_ptr);
	}

	PG_RETURN_TEXT_P(result_text);
}

/*
 * Convenience function to return true when two text params are equal.
 */
static bool
text_isequal(text *txt1, text *txt2, Oid collid)
{
	return DatumGetBool(DirectFunctionCall2Coll(texteq,
												collid,
												PointerGetDatum(txt1),
												PointerGetDatum(txt2)));
}

/*
 * text_to_array
 * parse input string and return text array of elements,
 * based on provided field separator
 */
Datum
text_to_array(PG_FUNCTION_ARGS)
{
	SplitTextOutputData tstate;

	/* For array output, tstate should start as all zeroes */
	memset(&tstate, 0, sizeof(tstate));

	if (!split_text(fcinfo, &tstate))
		PG_RETURN_NULL();

	if (tstate.astate == NULL)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));

	PG_RETURN_ARRAYTYPE_P(makeArrayResult(tstate.astate,
										  CurrentMemoryContext));
}

/*
 * text_to_array_null
 * parse input string and return text array of elements,
 * based on provided field separator and null string
 *
 * This is a separate entry point only to prevent the regression tests from
 * complaining about different argument sets for the same internal function.
 */
Datum
text_to_array_null(PG_FUNCTION_ARGS)
{
	return text_to_array(fcinfo);
}

/*
 * text_to_table
 * parse input string and return table of elements,
 * based on provided field separator
 */
Datum
text_to_table(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SplitTextOutputData tstate;
	MemoryContext old_cxt;

	/* check to see if caller supports us returning a tuplestore */
	if (rsi == NULL || !IsA(rsi, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsi->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* OK, prepare tuplestore in per-query memory */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	tstate.astate = NULL;
	tstate.tupdesc = CreateTupleDescCopy(rsi->expectedDesc);
	tstate.tupstore = tuplestore_begin_heap(true, false, work_mem);

	MemoryContextSwitchTo(old_cxt);

	(void) split_text(fcinfo, &tstate);

	tuplestore_donestoring(tstate.tupstore);

	rsi->returnMode = SFRM_Materialize;
	rsi->setResult = tstate.tupstore;
	rsi->setDesc = tstate.tupdesc;

	return (Datum) 0;
}

/*
 * text_to_table_null
 * parse input string and return table of elements,
 * based on provided field separator and null string
 *
 * This is a separate entry point only to prevent the regression tests from
 * complaining about different argument sets for the same internal function.
 */
Datum
text_to_table_null(PG_FUNCTION_ARGS)
{
	return text_to_table(fcinfo);
}

/*
 * Common code for text_to_array, text_to_array_null, text_to_table
 * and text_to_table_null functions.
 *
 * These are not strict so we have to test for null inputs explicitly.
 * Returns false if result is to be null, else returns true.
 *
 * Note that if the result is valid but empty (zero elements), we return
 * without changing *tstate --- caller must handle that case, too.
 */
static bool
split_text(FunctionCallInfo fcinfo, SplitTextOutputData *tstate)
{
	text	   *inputstring;
	text	   *fldsep;
	text	   *null_string;
	Oid			collation = PG_GET_COLLATION();
	int			inputstring_len;
	int			fldsep_len;
	char	   *start_ptr;
	text	   *result_text;

	/* when input string is NULL, then result is NULL too */
	if (PG_ARGISNULL(0))
		return false;

	inputstring = PG_GETARG_TEXT_PP(0);

	/* fldsep can be NULL */
	if (!PG_ARGISNULL(1))
		fldsep = PG_GETARG_TEXT_PP(1);
	else
		fldsep = NULL;

	/* null_string can be NULL or omitted */
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		null_string = PG_GETARG_TEXT_PP(2);
	else
		null_string = NULL;

	if (fldsep != NULL)
	{
		/*
		 * Normal case with non-null fldsep.  Use the text_position machinery
		 * to search for occurrences of fldsep.
		 */
		TextPositionState state;

		inputstring_len = VARSIZE_ANY_EXHDR(inputstring);
		fldsep_len = VARSIZE_ANY_EXHDR(fldsep);

		/* return empty set for empty input string */
		if (inputstring_len < 1)
			return true;

		/* empty field separator: return input string as a one-element set */
		if (fldsep_len < 1)
		{
			split_text_accum_result(tstate, inputstring,
									null_string, collation);
			return true;
		}

		text_position_setup(inputstring, fldsep, collation, &state);

		start_ptr = VARDATA_ANY(inputstring);

		for (;;)
		{
			bool		found;
			char	   *end_ptr;
			int			chunk_len;

			CHECK_FOR_INTERRUPTS();

			found = text_position_next(&state);
			if (!found)
			{
				/* fetch last field */
				chunk_len = ((char *) inputstring + VARSIZE_ANY(inputstring)) - start_ptr;
				end_ptr = NULL; /* not used, but some compilers complain */
			}
			else
			{
				/* fetch non-last field */
				end_ptr = text_position_get_match_ptr(&state);
				chunk_len = end_ptr - start_ptr;
			}

			/* build a temp text datum to pass to split_text_accum_result */
			result_text = cstring_to_text_with_len(start_ptr, chunk_len);

			/* stash away this field */
			split_text_accum_result(tstate, result_text,
									null_string, collation);

			pfree(result_text);

			if (!found)
				break;

			start_ptr = end_ptr + fldsep_len;
		}

		text_position_cleanup(&state);
	}
	else
	{
		/*
		 * When fldsep is NULL, each character in the input string becomes a
		 * separate element in the result set.  The separator is effectively
		 * the space between characters.
		 */
		inputstring_len = VARSIZE_ANY_EXHDR(inputstring);

		start_ptr = VARDATA_ANY(inputstring);

		while (inputstring_len > 0)
		{
			int			chunk_len = pg_mblen(start_ptr);

			CHECK_FOR_INTERRUPTS();

			/* build a temp text datum to pass to split_text_accum_result */
			result_text = cstring_to_text_with_len(start_ptr, chunk_len);

			/* stash away this field */
			split_text_accum_result(tstate, result_text,
									null_string, collation);

			pfree(result_text);

			start_ptr += chunk_len;
			inputstring_len -= chunk_len;
		}
	}

	return true;
}

/*
 * Add text item to result set (table or array).
 *
 * This is also responsible for checking to see if the item matches
 * the null_string, in which case we should emit NULL instead.
 */
static void
split_text_accum_result(SplitTextOutputData *tstate,
						text *field_value,
						text *null_string,
						Oid collation)
{
	bool		is_null = false;

	if (null_string && text_isequal(field_value, null_string, collation))
		is_null = true;

	if (tstate->tupstore)
	{
		Datum		values[1];
		bool		nulls[1];

		values[0] = PointerGetDatum(field_value);
		nulls[0] = is_null;

		tuplestore_putvalues(tstate->tupstore,
							 tstate->tupdesc,
							 values,
							 nulls);
	}
	else
	{
		tstate->astate = accumArrayResult(tstate->astate,
										  PointerGetDatum(field_value),
										  is_null,
										  TEXTOID,
										  CurrentMemoryContext);
	}
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
	char	   *fldsep = text_to_cstring(PG_GETARG_TEXT_PP(1));

	PG_RETURN_TEXT_P(array_to_text_internal(fcinfo, v, fldsep, NULL));
}

/*
 * array_to_text_null
 * concatenate Cstring representation of input array elements
 * using provided field separator and null string
 *
 * This version is not strict so we have to test for null inputs explicitly.
 */
Datum
array_to_text_null(PG_FUNCTION_ARGS)
{
	ArrayType  *v;
	char	   *fldsep;
	char	   *null_string;

	/* returns NULL when first or second parameter is NULL */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	v = PG_GETARG_ARRAYTYPE_P(0);
	fldsep = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* NULL null string is passed through as a null pointer */
	if (!PG_ARGISNULL(2))
		null_string = text_to_cstring(PG_GETARG_TEXT_PP(2));
	else
		null_string = NULL;

	PG_RETURN_TEXT_P(array_to_text_internal(fcinfo, v, fldsep, null_string));
}

/*
 * common code for array_to_text and array_to_text_null functions
 */
static text *
array_to_text_internal(FunctionCallInfo fcinfo, ArrayType *v,
					   const char *fldsep, const char *null_string)
{
	text	   *result;
	int			nitems,
			   *dims,
				ndims;
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	StringInfoData buf;
	bool		printed = false;
	char	   *p;
	bits8	   *bitmap;
	int			bitmask;
	int			i;
	ArrayMetaState *my_extra;

	ndims = ARR_NDIM(v);
	dims = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndims, dims);

	/* if there are no elements, return an empty string */
	if (nitems == 0)
		return cstring_to_text_with_len("", 0);

	element_type = ARR_ELEMTYPE(v);
	initStringInfo(&buf);

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
		my_extra->element_type = ~element_type;
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

	p = ARR_DATA_PTR(v);
	bitmap = ARR_NULLBITMAP(v);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		char	   *value;

		/* Get source element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			/* if null_string is NULL, we just ignore null elements */
			if (null_string != NULL)
			{
				if (printed)
					appendStringInfo(&buf, "%s%s", fldsep, null_string);
				else
					appendStringInfoString(&buf, null_string);
				printed = true;
			}
		}
		else
		{
			itemvalue = fetch_att(p, typbyval, typlen);

			value = OutputFunctionCall(&my_extra->proc, itemvalue);

			if (printed)
				appendStringInfo(&buf, "%s%s", fldsep, value);
			else
				appendStringInfoString(&buf, value);
			printed = true;

			p = att_addlength_pointer(p, typlen, p);
			p = (char *) att_align_nominal(p, typalign);
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	result = cstring_to_text_with_len(buf.data, buf.len);
	pfree(buf.data);

	return result;
}

#define HEXBASE 16
/*
 * Convert an int32 to a string containing a base 16 (hex) representation of
 * the number.
 */
Datum
to_hex32(PG_FUNCTION_ARGS)
{
	uint32		value = (uint32) PG_GETARG_INT32(0);
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

	PG_RETURN_TEXT_P(cstring_to_text(ptr));
}

/*
 * Convert an int64 to a string containing a base 16 (hex) representation of
 * the number.
 */
Datum
to_hex64(PG_FUNCTION_ARGS)
{
	uint64		value = (uint64) PG_GETARG_INT64(0);
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

	PG_RETURN_TEXT_P(cstring_to_text(ptr));
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

/*
 * string_agg - Concatenates values and returns string.
 *
 * Syntax: string_agg(value text, delimiter text) RETURNS text
 *
 * Note: Any NULL values are ignored. The first-call delimiter isn't
 * actually used at all, and on subsequent calls the delimiter precedes
 * the associated value.
 */

/* subroutine to initialize state */
static StringInfo
makeStringAggState(FunctionCallInfo fcinfo)
{
	StringInfo	state;
	MemoryContext aggcontext;
	MemoryContext oldcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "string_agg_transfn called in non-aggregate context");
	}

	/*
	 * Create state in aggregate context.  It'll stay there across subsequent
	 * calls.
	 */
	oldcontext = MemoryContextSwitchTo(aggcontext);
	state = makeStringInfo();
	MemoryContextSwitchTo(oldcontext);

	return state;
}

Datum
string_agg_transfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	/* Append the value unless null. */
	if (!PG_ARGISNULL(1))
	{
		/* On the first time through, we ignore the delimiter. */
		if (state == NULL)
			state = makeStringAggState(fcinfo);
		else if (!PG_ARGISNULL(2))
			appendStringInfoText(state, PG_GETARG_TEXT_PP(2));	/* delimiter */

		appendStringInfoText(state, PG_GETARG_TEXT_PP(1));	/* value */
	}

	/*
	 * The transition type for string_agg() is declared to be "internal",
	 * which is a pass-by-value type the same size as a pointer.
	 */
	PG_RETURN_POINTER(state);
}

Datum
string_agg_finalfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	/* cannot be called directly because of internal-type argument */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	if (state != NULL)
		PG_RETURN_TEXT_P(cstring_to_text_with_len(state->data, state->len));
	else
		PG_RETURN_NULL();
}

/*
 * Prepare cache with fmgr info for the output functions of the datatypes of
 * the arguments of a concat-like function, beginning with argument "argidx".
 * (Arguments before that will have corresponding slots in the resulting
 * FmgrInfo array, but we don't fill those slots.)
 */
static FmgrInfo *
build_concat_foutcache(FunctionCallInfo fcinfo, int argidx)
{
	FmgrInfo   *foutcache;
	int			i;

	/* We keep the info in fn_mcxt so it survives across calls */
	foutcache = (FmgrInfo *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												PG_NARGS() * sizeof(FmgrInfo));

	for (i = argidx; i < PG_NARGS(); i++)
	{
		Oid			valtype;
		Oid			typOutput;
		bool		typIsVarlena;

		valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		if (!OidIsValid(valtype))
			elog(ERROR, "could not determine data type of concat() input");

		getTypeOutputInfo(valtype, &typOutput, &typIsVarlena);
		fmgr_info_cxt(typOutput, &foutcache[i], fcinfo->flinfo->fn_mcxt);
	}

	fcinfo->flinfo->fn_extra = foutcache;

	return foutcache;
}

/*
 * Implementation of both concat() and concat_ws().
 *
 * sepstr is the separator string to place between values.
 * argidx identifies the first argument to concatenate (counting from zero);
 * note that this must be constant across any one series of calls.
 *
 * Returns NULL if result should be NULL, else text value.
 */
static text *
concat_internal(const char *sepstr, int argidx,
				FunctionCallInfo fcinfo)
{
	text	   *result;
	StringInfoData str;
	FmgrInfo   *foutcache;
	bool		first_arg = true;
	int			i;

	/*
	 * concat(VARIADIC some-array) is essentially equivalent to
	 * array_to_text(), ie concat the array elements with the given separator.
	 * So we just pass the case off to that code.
	 */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;

		/* Should have just the one argument */
		Assert(argidx == PG_NARGS() - 1);

		/* concat(VARIADIC NULL) is defined as NULL */
		if (PG_ARGISNULL(argidx))
			return NULL;

		/*
		 * Non-null argument had better be an array.  We assume that any call
		 * context that could let get_fn_expr_variadic return true will have
		 * checked that a VARIADIC-labeled parameter actually is an array.  So
		 * it should be okay to just Assert that it's an array rather than
		 * doing a full-fledged error check.
		 */
		Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, argidx))));

		/* OK, safe to fetch the array value */
		arr = PG_GETARG_ARRAYTYPE_P(argidx);

		/*
		 * And serialize the array.  We tell array_to_text to ignore null
		 * elements, which matches the behavior of the loop below.
		 */
		return array_to_text_internal(fcinfo, arr, sepstr, NULL);
	}

	/* Normal case without explicit VARIADIC marker */
	initStringInfo(&str);

	/* Get output function info, building it if first time through */
	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, argidx);

	for (i = argidx; i < PG_NARGS(); i++)
	{
		if (!PG_ARGISNULL(i))
		{
			Datum		value = PG_GETARG_DATUM(i);

			/* add separator if appropriate */
			if (first_arg)
				first_arg = false;
			else
				appendStringInfoString(&str, sepstr);

			/* call the appropriate type output function, append the result */
			appendStringInfoString(&str,
								   OutputFunctionCall(&foutcache[i], value));
		}
	}

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	return result;
}

/*
 * Concatenate all arguments. NULL arguments are ignored.
 */
Datum
text_concat(PG_FUNCTION_ARGS)
{
	text	   *result;

	result = concat_internal("", 0, fcinfo);
	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

/*
 * Concatenate all but first argument value with separators. The first
 * parameter is used as the separator. NULL arguments are ignored.
 */
Datum
text_concat_ws(PG_FUNCTION_ARGS)
{
	char	   *sep;
	text	   *result;

	/* return NULL when separator is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	sep = text_to_cstring(PG_GETARG_TEXT_PP(0));

	result = concat_internal(sep, 1, fcinfo);
	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

/*
 * Return first n characters in the string. When n is negative,
 * return all but last |n| characters.
 */
Datum
text_left(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(1);

	if (n < 0)
	{
		text	   *str = PG_GETARG_TEXT_PP(0);
		const char *p = VARDATA_ANY(str);
		int			len = VARSIZE_ANY_EXHDR(str);
		int			rlen;

		n = pg_mbstrlen_with_len(p, len) + n;
		rlen = pg_mbcharcliplen(p, len, n);
		PG_RETURN_TEXT_P(cstring_to_text_with_len(p, rlen));
	}
	else
		PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0), 1, n, false));
}

/*
 * Return last n characters in the string. When n is negative,
 * return all but first |n| characters.
 */
Datum
text_right(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	const char *p = VARDATA_ANY(str);
	int			len = VARSIZE_ANY_EXHDR(str);
	int			n = PG_GETARG_INT32(1);
	int			off;

	if (n < 0)
		n = -n;
	else
		n = pg_mbstrlen_with_len(p, len) - n;
	off = pg_mbcharcliplen(p, len, n);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(p + off, len - off));
}

/*
 * Return reversed string
 */
Datum
text_reverse(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	const char *p = VARDATA_ANY(str);
	int			len = VARSIZE_ANY_EXHDR(str);
	const char *endp = p + len;
	text	   *result;
	char	   *dst;

	result = palloc(len + VARHDRSZ);
	dst = (char *) VARDATA(result) + len;
	SET_VARSIZE(result, len + VARHDRSZ);

	if (pg_database_encoding_max_length() > 1)
	{
		/* multibyte version */
		while (p < endp)
		{
			int			sz;

			sz = pg_mblen(p);
			dst -= sz;
			memcpy(dst, p, sz);
			p += sz;
		}
	}
	else
	{
		/* single byte version */
		while (p < endp)
			*(--dst) = *p++;
	}

	PG_RETURN_TEXT_P(result);
}


/*
 * Support macros for text_format()
 */
#define TEXT_FORMAT_FLAG_MINUS	0x0001	/* is minus flag present? */

#define ADVANCE_PARSE_POINTER(ptr,end_ptr) \
	do { \
		if (++(ptr) >= (end_ptr)) \
			ereport(ERROR, \
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
					 errmsg("unterminated format() type specifier"), \
					 errhint("For a single \"%%\" use \"%%%%\"."))); \
	} while (0)

/*
 * Returns a formatted string
 */
Datum
text_format(PG_FUNCTION_ARGS)
{
	text	   *fmt;
	StringInfoData str;
	const char *cp;
	const char *start_ptr;
	const char *end_ptr;
	text	   *result;
	int			arg;
	bool		funcvariadic;
	int			nargs;
	Datum	   *elements = NULL;
	bool	   *nulls = NULL;
	Oid			element_type = InvalidOid;
	Oid			prev_type = InvalidOid;
	Oid			prev_width_type = InvalidOid;
	FmgrInfo	typoutputfinfo;
	FmgrInfo	typoutputinfo_width;

	/* When format string is null, immediately return null */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	/* If argument is marked VARIADIC, expand array into elements */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			nitems;

		/* Should have just the one argument */
		Assert(PG_NARGS() == 2);

		/* If argument is NULL, we treat it as zero-length array */
		if (PG_ARGISNULL(1))
			nitems = 0;
		else
		{
			/*
			 * Non-null argument had better be an array.  We assume that any
			 * call context that could let get_fn_expr_variadic return true
			 * will have checked that a VARIADIC-labeled parameter actually is
			 * an array.  So it should be okay to just Assert that it's an
			 * array rather than doing a full-fledged error check.
			 */
			Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, 1))));

			/* OK, safe to fetch the array value */
			arr = PG_GETARG_ARRAYTYPE_P(1);

			/* Get info about array element type */
			element_type = ARR_ELEMTYPE(arr);
			get_typlenbyvalalign(element_type,
								 &elmlen, &elmbyval, &elmalign);

			/* Extract all array elements */
			deconstruct_array(arr, element_type, elmlen, elmbyval, elmalign,
							  &elements, &nulls, &nitems);
		}

		nargs = nitems + 1;
		funcvariadic = true;
	}
	else
	{
		/* Non-variadic case, we'll process the arguments individually */
		nargs = PG_NARGS();
		funcvariadic = false;
	}

	/* Setup for main loop. */
	fmt = PG_GETARG_TEXT_PP(0);
	start_ptr = VARDATA_ANY(fmt);
	end_ptr = start_ptr + VARSIZE_ANY_EXHDR(fmt);
	initStringInfo(&str);
	arg = 1;					/* next argument position to print */

	/* Scan format string, looking for conversion specifiers. */
	for (cp = start_ptr; cp < end_ptr; cp++)
	{
		int			argpos;
		int			widthpos;
		int			flags;
		int			width;
		Datum		value;
		bool		isNull;
		Oid			typid;

		/*
		 * If it's not the start of a conversion specifier, just copy it to
		 * the output buffer.
		 */
		if (*cp != '%')
		{
			appendStringInfoCharMacro(&str, *cp);
			continue;
		}

		ADVANCE_PARSE_POINTER(cp, end_ptr);

		/* Easy case: %% outputs a single % */
		if (*cp == '%')
		{
			appendStringInfoCharMacro(&str, *cp);
			continue;
		}

		/* Parse the optional portions of the format specifier */
		cp = text_format_parse_format(cp, end_ptr,
									  &argpos, &widthpos,
									  &flags, &width);

		/*
		 * Next we should see the main conversion specifier.  Whether or not
		 * an argument position was present, it's known that at least one
		 * character remains in the string at this point.  Experience suggests
		 * that it's worth checking that that character is one of the expected
		 * ones before we try to fetch arguments, so as to produce the least
		 * confusing response to a mis-formatted specifier.
		 */
		if (strchr("sIL", *cp) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized format() type specifier \"%.*s\"",
							pg_mblen(cp), cp),
					 errhint("For a single \"%%\" use \"%%%%\".")));

		/* If indirect width was specified, get its value */
		if (widthpos >= 0)
		{
			/* Collect the specified or next argument position */
			if (widthpos > 0)
				arg = widthpos;
			if (arg >= nargs)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("too few arguments for format()")));

			/* Get the value and type of the selected argument */
			if (!funcvariadic)
			{
				value = PG_GETARG_DATUM(arg);
				isNull = PG_ARGISNULL(arg);
				typid = get_fn_expr_argtype(fcinfo->flinfo, arg);
			}
			else
			{
				value = elements[arg - 1];
				isNull = nulls[arg - 1];
				typid = element_type;
			}
			if (!OidIsValid(typid))
				elog(ERROR, "could not determine data type of format() input");

			arg++;

			/* We can treat NULL width the same as zero */
			if (isNull)
				width = 0;
			else if (typid == INT4OID)
				width = DatumGetInt32(value);
			else if (typid == INT2OID)
				width = DatumGetInt16(value);
			else
			{
				/* For less-usual datatypes, convert to text then to int */
				char	   *str;

				if (typid != prev_width_type)
				{
					Oid			typoutputfunc;
					bool		typIsVarlena;

					getTypeOutputInfo(typid, &typoutputfunc, &typIsVarlena);
					fmgr_info(typoutputfunc, &typoutputinfo_width);
					prev_width_type = typid;
				}

				str = OutputFunctionCall(&typoutputinfo_width, value);

				/* pg_strtoint32 will complain about bad data or overflow */
				width = pg_strtoint32(str);

				pfree(str);
			}
		}

		/* Collect the specified or next argument position */
		if (argpos > 0)
			arg = argpos;
		if (arg >= nargs)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("too few arguments for format()")));

		/* Get the value and type of the selected argument */
		if (!funcvariadic)
		{
			value = PG_GETARG_DATUM(arg);
			isNull = PG_ARGISNULL(arg);
			typid = get_fn_expr_argtype(fcinfo->flinfo, arg);
		}
		else
		{
			value = elements[arg - 1];
			isNull = nulls[arg - 1];
			typid = element_type;
		}
		if (!OidIsValid(typid))
			elog(ERROR, "could not determine data type of format() input");

		arg++;

		/*
		 * Get the appropriate typOutput function, reusing previous one if
		 * same type as previous argument.  That's particularly useful in the
		 * variadic-array case, but often saves work even for ordinary calls.
		 */
		if (typid != prev_type)
		{
			Oid			typoutputfunc;
			bool		typIsVarlena;

			getTypeOutputInfo(typid, &typoutputfunc, &typIsVarlena);
			fmgr_info(typoutputfunc, &typoutputfinfo);
			prev_type = typid;
		}

		/*
		 * And now we can format the value.
		 */
		switch (*cp)
		{
			case 's':
			case 'I':
			case 'L':
				text_format_string_conversion(&str, *cp, &typoutputfinfo,
											  value, isNull,
											  flags, width);
				break;
			default:
				/* should not get here, because of previous check */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized format() type specifier \"%.*s\"",
								pg_mblen(cp), cp),
						 errhint("For a single \"%%\" use \"%%%%\".")));
				break;
		}
	}

	/* Don't need deconstruct_array results anymore. */
	if (elements != NULL)
		pfree(elements);
	if (nulls != NULL)
		pfree(nulls);

	/* Generate results. */
	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	PG_RETURN_TEXT_P(result);
}

/*
 * Parse contiguous digits as a decimal number.
 *
 * Returns true if some digits could be parsed.
 * The value is returned into *value, and *ptr is advanced to the next
 * character to be parsed.
 *
 * Note parsing invariant: at least one character is known available before
 * string end (end_ptr) at entry, and this is still true at exit.
 */
static bool
text_format_parse_digits(const char **ptr, const char *end_ptr, int *value)
{
	bool		found = false;
	const char *cp = *ptr;
	int			val = 0;

	while (*cp >= '0' && *cp <= '9')
	{
		int8		digit = (*cp - '0');

		if (unlikely(pg_mul_s32_overflow(val, 10, &val)) ||
			unlikely(pg_add_s32_overflow(val, digit, &val)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("number is out of range")));
		ADVANCE_PARSE_POINTER(cp, end_ptr);
		found = true;
	}

	*ptr = cp;
	*value = val;

	return found;
}

/*
 * Parse a format specifier (generally following the SUS printf spec).
 *
 * We have already advanced over the initial '%', and we are looking for
 * [argpos][flags][width]type (but the type character is not consumed here).
 *
 * Inputs are start_ptr (the position after '%') and end_ptr (string end + 1).
 * Output parameters:
 *	argpos: argument position for value to be printed.  -1 means unspecified.
 *	widthpos: argument position for width.  Zero means the argument position
 *			was unspecified (ie, take the next arg) and -1 means no width
 *			argument (width was omitted or specified as a constant).
 *	flags: bitmask of flags.
 *	width: directly-specified width value.  Zero means the width was omitted
 *			(note it's not necessary to distinguish this case from an explicit
 *			zero width value).
 *
 * The function result is the next character position to be parsed, ie, the
 * location where the type character is/should be.
 *
 * Note parsing invariant: at least one character is known available before
 * string end (end_ptr) at entry, and this is still true at exit.
 */
static const char *
text_format_parse_format(const char *start_ptr, const char *end_ptr,
						 int *argpos, int *widthpos,
						 int *flags, int *width)
{
	const char *cp = start_ptr;
	int			n;

	/* set defaults for output parameters */
	*argpos = -1;
	*widthpos = -1;
	*flags = 0;
	*width = 0;

	/* try to identify first number */
	if (text_format_parse_digits(&cp, end_ptr, &n))
	{
		if (*cp != '$')
		{
			/* Must be just a width and a type, so we're done */
			*width = n;
			return cp;
		}
		/* The number was argument position */
		*argpos = n;
		/* Explicit 0 for argument index is immediately refused */
		if (n == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("format specifies argument 0, but arguments are numbered from 1")));
		ADVANCE_PARSE_POINTER(cp, end_ptr);
	}

	/* Handle flags (only minus is supported now) */
	while (*cp == '-')
	{
		*flags |= TEXT_FORMAT_FLAG_MINUS;
		ADVANCE_PARSE_POINTER(cp, end_ptr);
	}

	if (*cp == '*')
	{
		/* Handle indirect width */
		ADVANCE_PARSE_POINTER(cp, end_ptr);
		if (text_format_parse_digits(&cp, end_ptr, &n))
		{
			/* number in this position must be closed by $ */
			if (*cp != '$')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("width argument position must be ended by \"$\"")));
			/* The number was width argument position */
			*widthpos = n;
			/* Explicit 0 for argument index is immediately refused */
			if (n == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("format specifies argument 0, but arguments are numbered from 1")));
			ADVANCE_PARSE_POINTER(cp, end_ptr);
		}
		else
			*widthpos = 0;		/* width's argument position is unspecified */
	}
	else
	{
		/* Check for direct width specification */
		if (text_format_parse_digits(&cp, end_ptr, &n))
			*width = n;
	}

	/* cp should now be pointing at type character */
	return cp;
}

/*
 * Format a %s, %I, or %L conversion
 */
static void
text_format_string_conversion(StringInfo buf, char conversion,
							  FmgrInfo *typOutputInfo,
							  Datum value, bool isNull,
							  int flags, int width)
{
	char	   *str;

	/* Handle NULL arguments before trying to stringify the value. */
	if (isNull)
	{
		if (conversion == 's')
			text_format_append_string(buf, "", flags, width);
		else if (conversion == 'L')
			text_format_append_string(buf, "NULL", flags, width);
		else if (conversion == 'I')
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null values cannot be formatted as an SQL identifier")));
		return;
	}

	/* Stringify. */
	str = OutputFunctionCall(typOutputInfo, value);

	/* Escape. */
	if (conversion == 'I')
	{
		/* quote_identifier may or may not allocate a new string. */
		text_format_append_string(buf, quote_identifier(str), flags, width);
	}
	else if (conversion == 'L')
	{
		char	   *qstr = quote_literal_cstr(str);

		text_format_append_string(buf, qstr, flags, width);
		/* quote_literal_cstr() always allocates a new string */
		pfree(qstr);
	}
	else
		text_format_append_string(buf, str, flags, width);

	/* Cleanup. */
	pfree(str);
}

/*
 * Append str to buf, padding as directed by flags/width
 */
static void
text_format_append_string(StringInfo buf, const char *str,
						  int flags, int width)
{
	bool		align_to_left = false;
	int			len;

	/* fast path for typical easy case */
	if (width == 0)
	{
		appendStringInfoString(buf, str);
		return;
	}

	if (width < 0)
	{
		/* Negative width: implicit '-' flag, then take absolute value */
		align_to_left = true;
		/* -INT_MIN is undefined */
		if (width <= INT_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("number is out of range")));
		width = -width;
	}
	else if (flags & TEXT_FORMAT_FLAG_MINUS)
		align_to_left = true;

	len = pg_mbstrlen(str);
	if (align_to_left)
	{
		/* left justify */
		appendStringInfoString(buf, str);
		if (len < width)
			appendStringInfoSpaces(buf, width - len);
	}
	else
	{
		/* right justify */
		if (len < width)
			appendStringInfoSpaces(buf, width - len);
		appendStringInfoString(buf, str);
	}
}

/*
 * text_format_nv - nonvariadic wrapper for text_format function.
 *
 * note: this wrapper is necessary to pass the sanity check in opr_sanity,
 * which checks that all built-in functions that share the implementing C
 * function take the same number of arguments.
 */
Datum
text_format_nv(PG_FUNCTION_ARGS)
{
	return text_format(fcinfo);
}

/*
 * Helper function for Levenshtein distance functions. Faster than memcmp(),
 * for this use case.
 */
static inline bool
rest_of_char_same(const char *s1, const char *s2, int len)
{
	while (len > 0)
	{
		len--;
		if (s1[len] != s2[len])
			return false;
	}
	return true;
}

/* Expand each Levenshtein distance variant */
#include "levenshtein.c"
#define LEVENSHTEIN_LESS_EQUAL
#include "levenshtein.c"


/*
 * Unicode support
 */

static UnicodeNormalizationForm
unicode_norm_form_from_string(const char *formstr)
{
	UnicodeNormalizationForm form = -1;

	/*
	 * Might as well check this while we're here.
	 */
	if (GetDatabaseEncoding() != PG_UTF8)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Unicode normalization can only be performed if server encoding is UTF8")));

	if (pg_strcasecmp(formstr, "NFC") == 0)
		form = UNICODE_NFC;
	else if (pg_strcasecmp(formstr, "NFD") == 0)
		form = UNICODE_NFD;
	else if (pg_strcasecmp(formstr, "NFKC") == 0)
		form = UNICODE_NFKC;
	else if (pg_strcasecmp(formstr, "NFKD") == 0)
		form = UNICODE_NFKD;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid normalization form: %s", formstr)));

	return form;
}

Datum
unicode_normalize_func(PG_FUNCTION_ARGS)
{
	text	   *input = PG_GETARG_TEXT_PP(0);
	char	   *formstr = text_to_cstring(PG_GETARG_TEXT_PP(1));
	UnicodeNormalizationForm form;
	int			size;
	pg_wchar   *input_chars;
	pg_wchar   *output_chars;
	unsigned char *p;
	text	   *result;
	int			i;

	form = unicode_norm_form_from_string(formstr);

	/* convert to pg_wchar */
	size = pg_mbstrlen_with_len(VARDATA_ANY(input), VARSIZE_ANY_EXHDR(input));
	input_chars = palloc((size + 1) * sizeof(pg_wchar));
	p = (unsigned char *) VARDATA_ANY(input);
	for (i = 0; i < size; i++)
	{
		input_chars[i] = utf8_to_unicode(p);
		p += pg_utf_mblen(p);
	}
	input_chars[i] = (pg_wchar) '\0';
	Assert((char *) p == VARDATA_ANY(input) + VARSIZE_ANY_EXHDR(input));

	/* action */
	output_chars = unicode_normalize(form, input_chars);

	/* convert back to UTF-8 string */
	size = 0;
	for (pg_wchar *wp = output_chars; *wp; wp++)
	{
		unsigned char buf[4];

		unicode_to_utf8(*wp, buf);
		size += pg_utf_mblen(buf);
	}

	result = palloc(size + VARHDRSZ);
	SET_VARSIZE(result, size + VARHDRSZ);

	p = (unsigned char *) VARDATA_ANY(result);
	for (pg_wchar *wp = output_chars; *wp; wp++)
	{
		unicode_to_utf8(*wp, p);
		p += pg_utf_mblen(p);
	}
	Assert((char *) p == (char *) result + size + VARHDRSZ);

	PG_RETURN_TEXT_P(result);
}

/*
 * Check whether the string is in the specified Unicode normalization form.
 *
 * This is done by converting the string to the specified normal form and then
 * comparing that to the original string.  To speed that up, we also apply the
 * "quick check" algorithm specified in UAX #15, which can give a yes or no
 * answer for many strings by just scanning the string once.
 *
 * This function should generally be optimized for the case where the string
 * is in fact normalized.  In that case, we'll end up looking at the entire
 * string, so it's probably not worth doing any incremental conversion etc.
 */
Datum
unicode_is_normalized(PG_FUNCTION_ARGS)
{
	text	   *input = PG_GETARG_TEXT_PP(0);
	char	   *formstr = text_to_cstring(PG_GETARG_TEXT_PP(1));
	UnicodeNormalizationForm form;
	int			size;
	pg_wchar   *input_chars;
	pg_wchar   *output_chars;
	unsigned char *p;
	int			i;
	UnicodeNormalizationQC quickcheck;
	int			output_size;
	bool		result;

	form = unicode_norm_form_from_string(formstr);

	/* convert to pg_wchar */
	size = pg_mbstrlen_with_len(VARDATA_ANY(input), VARSIZE_ANY_EXHDR(input));
	input_chars = palloc((size + 1) * sizeof(pg_wchar));
	p = (unsigned char *) VARDATA_ANY(input);
	for (i = 0; i < size; i++)
	{
		input_chars[i] = utf8_to_unicode(p);
		p += pg_utf_mblen(p);
	}
	input_chars[i] = (pg_wchar) '\0';
	Assert((char *) p == VARDATA_ANY(input) + VARSIZE_ANY_EXHDR(input));

	/* quick check (see UAX #15) */
	quickcheck = unicode_is_normalized_quickcheck(form, input_chars);
	if (quickcheck == UNICODE_NORM_QC_YES)
		PG_RETURN_BOOL(true);
	else if (quickcheck == UNICODE_NORM_QC_NO)
		PG_RETURN_BOOL(false);

	/* normalize and compare with original */
	output_chars = unicode_normalize(form, input_chars);

	output_size = 0;
	for (pg_wchar *wp = output_chars; *wp; wp++)
		output_size++;

	result = (size == output_size) &&
		(memcmp(input_chars, output_chars, size * sizeof(pg_wchar)) == 0);

	PG_RETURN_BOOL(result);
}
