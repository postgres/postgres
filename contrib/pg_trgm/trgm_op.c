/*
 * contrib/pg_trgm/trgm_op.c
 */
#include "postgres.h"

#include <ctype.h>

#include "trgm.h"

#include "catalog/pg_type.h"
#include "tsearch/ts_locale.h"
#include "utils/memutils.h"


PG_MODULE_MAGIC;

float4		trgm_limit = 0.3f;

PG_FUNCTION_INFO_V1(set_limit);
PG_FUNCTION_INFO_V1(show_limit);
PG_FUNCTION_INFO_V1(show_trgm);
PG_FUNCTION_INFO_V1(similarity);
PG_FUNCTION_INFO_V1(similarity_dist);
PG_FUNCTION_INFO_V1(similarity_op);


Datum
set_limit(PG_FUNCTION_ARGS)
{
	float4		nlimit = PG_GETARG_FLOAT4(0);

	if (nlimit < 0 || nlimit > 1.0)
		elog(ERROR, "wrong limit, should be between 0 and 1");
	trgm_limit = nlimit;
	PG_RETURN_FLOAT4(trgm_limit);
}

Datum
show_limit(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT4(trgm_limit);
}

static int
comp_trgm(const void *a, const void *b)
{
	return CMPTRGM(a, b);
}

static int
unique_array(trgm *a, int len)
{
	trgm	   *curend,
			   *tmp;

	curend = tmp = a;
	while (tmp - a < len)
		if (CMPTRGM(tmp, curend))
		{
			curend++;
			CPTRGM(curend, tmp);
			tmp++;
		}
		else
			tmp++;

	return curend + 1 - a;
}

/*
 * Finds first word in string, returns pointer to the word,
 * endword points to the character after word
 */
static char *
find_word(char *str, int lenstr, char **endword, int *charlen)
{
	char	   *beginword = str;

	while (beginword - str < lenstr && !ISWORDCHR(beginword))
		beginword += pg_mblen(beginword);

	if (beginword - str >= lenstr)
		return NULL;

	*endword = beginword;
	*charlen = 0;
	while (*endword - str < lenstr && ISWORDCHR(*endword))
	{
		*endword += pg_mblen(*endword);
		(*charlen)++;
	}

	return beginword;
}

/*
 * Reduce a trigram (three possibly multi-byte characters) to a trgm,
 * which is always exactly three bytes.  If we have three single-byte
 * characters, we just use them as-is; otherwise we form a hash value.
 */
void
compact_trigram(trgm *tptr, char *str, int bytelen)
{
	if (bytelen == 3)
	{
		CPTRGM(tptr, str);
	}
	else
	{
		pg_crc32	crc;

		INIT_CRC32(crc);
		COMP_CRC32(crc, str, bytelen);
		FIN_CRC32(crc);

		/*
		 * use only 3 upper bytes from crc, hope, it's good enough hashing
		 */
		CPTRGM(tptr, &crc);
	}
}

/*
 * Adds trigrams from words (already padded).
 */
static trgm *
make_trigrams(trgm *tptr, char *str, int bytelen, int charlen)
{
	char	   *ptr = str;

	if (charlen < 3)
		return tptr;

	if (bytelen > charlen)
	{
		/* Find multibyte character boundaries and apply compact_trigram */
		int			lenfirst = pg_mblen(str),
					lenmiddle = pg_mblen(str + lenfirst),
					lenlast = pg_mblen(str + lenfirst + lenmiddle);

		while ((ptr - str) + lenfirst + lenmiddle + lenlast <= bytelen)
		{
			compact_trigram(tptr, ptr, lenfirst + lenmiddle + lenlast);

			ptr += lenfirst;
			tptr++;

			lenfirst = lenmiddle;
			lenmiddle = lenlast;
			lenlast = pg_mblen(ptr + lenfirst + lenmiddle);
		}
	}
	else
	{
		/* Fast path when there are no multibyte characters */
		Assert(bytelen == charlen);

		while (ptr - str < bytelen - 2 /* number of trigrams = strlen - 2 */ )
		{
			CPTRGM(tptr, ptr);
			ptr++;
			tptr++;
		}
	}

	return tptr;
}

TRGM *
generate_trgm(char *str, int slen)
{
	TRGM	   *trg;
	char	   *buf;
	trgm	   *tptr;
	int			len,
				charlen,
				bytelen;
	char	   *bword,
			   *eword;

	/*
	 * Guard against possible overflow in the palloc requests below.  (We
	 * don't worry about the additive constants, since palloc can detect
	 * requests that are a little above MaxAllocSize --- we just need to
	 * prevent integer overflow in the multiplications.)
	 */
	if ((Size) (slen / 2) >= (MaxAllocSize / (sizeof(trgm) * 3)) ||
		(Size) slen >= (MaxAllocSize / pg_database_encoding_max_length()))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory")));

	trg = (TRGM *) palloc(TRGMHDRSIZE + sizeof(trgm) * (slen / 2 + 1) *3);
	trg->flag = ARRKEY;
	SET_VARSIZE(trg, TRGMHDRSIZE);

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return trg;

	tptr = GETARR(trg);

	/* Allocate a buffer for case-folded, blank-padded words */
	buf = (char *) palloc(slen * pg_database_encoding_max_length() + 4);

	if (LPADDING > 0)
	{
		*buf = ' ';
		if (LPADDING > 1)
			*(buf + 1) = ' ';
	}

	eword = str;
	while ((bword = find_word(eword, slen - (eword - str), &eword, &charlen)) != NULL)
	{
#ifdef IGNORECASE
		bword = lowerstr_with_len(bword, eword - bword);
		bytelen = strlen(bword);
#else
		bytelen = eword - bword;
#endif

		memcpy(buf + LPADDING, bword, bytelen);

#ifdef IGNORECASE
		pfree(bword);
#endif

		buf[LPADDING + bytelen] = ' ';
		buf[LPADDING + bytelen + 1] = ' ';

		/*
		 * count trigrams
		 */
		tptr = make_trigrams(tptr, buf, bytelen + LPADDING + RPADDING,
							 charlen + LPADDING + RPADDING);
	}

	pfree(buf);

	if ((len = tptr - GETARR(trg)) == 0)
		return trg;

	/*
	 * Make trigrams unique.
	 */
	if (len > 1)
	{
		qsort((void *) GETARR(trg), len, sizeof(trgm), comp_trgm);
		len = unique_array(GETARR(trg), len);
	}

	SET_VARSIZE(trg, CALCGTSIZE(ARRKEY, len));

	return trg;
}

/*
 * Extract the next non-wildcard part of a search string, ie, a word bounded
 * by '_' or '%' meta-characters, non-word characters or string end.
 *
 * str: source string, of length lenstr bytes (need not be null-terminated)
 * buf: where to return the substring (must be long enough)
 * *bytelen: receives byte length of the found substring
 * *charlen: receives character length of the found substring
 *
 * Returns pointer to end+1 of the found substring in the source string.
 * Returns NULL if no word found (in which case buf, bytelen, charlen not set)
 *
 * If the found word is bounded by non-word characters or string boundaries
 * then this function will include corresponding padding spaces into buf.
 */
static const char *
get_wildcard_part(const char *str, int lenstr,
				  char *buf, int *bytelen, int *charlen)
{
	const char *beginword = str;
	const char *endword;
	char	   *s = buf;
	bool		in_leading_wildcard_meta = false;
	bool		in_trailing_wildcard_meta = false;
	bool		in_escape = false;
	int			clen;

	/*
	 * Find the first word character, remembering whether preceding character
	 * was wildcard meta-character.  Note that the in_escape state persists
	 * from this loop to the next one, since we may exit at a word character
	 * that is in_escape.
	 */
	while (beginword - str < lenstr)
	{
		if (in_escape)
		{
			if (ISWORDCHR(beginword))
				break;
			in_escape = false;
			in_leading_wildcard_meta = false;
		}
		else
		{
			if (ISESCAPECHAR(beginword))
				in_escape = true;
			else if (ISWILDCARDCHAR(beginword))
				in_leading_wildcard_meta = true;
			else if (ISWORDCHR(beginword))
				break;
			else
				in_leading_wildcard_meta = false;
		}
		beginword += pg_mblen(beginword);
	}

	/*
	 * Handle string end.
	 */
	if (beginword - str >= lenstr)
		return NULL;

	/*
	 * Add left padding spaces if preceding character wasn't wildcard
	 * meta-character.
	 */
	*charlen = 0;
	if (!in_leading_wildcard_meta)
	{
		if (LPADDING > 0)
		{
			*s++ = ' ';
			(*charlen)++;
			if (LPADDING > 1)
			{
				*s++ = ' ';
				(*charlen)++;
			}
		}
	}

	/*
	 * Copy data into buf until wildcard meta-character, non-word character or
	 * string boundary.  Strip escapes during copy.
	 */
	endword = beginword;
	while (endword - str < lenstr)
	{
		clen = pg_mblen(endword);
		if (in_escape)
		{
			if (ISWORDCHR(endword))
			{
				memcpy(s, endword, clen);
				(*charlen)++;
				s += clen;
			}
			else
			{
				/*
				 * Back up endword to the escape character when stopping at an
				 * escaped char, so that subsequent get_wildcard_part will
				 * restart from the escape character.  We assume here that
				 * escape chars are single-byte.
				 */
				endword--;
				break;
			}
			in_escape = false;
		}
		else
		{
			if (ISESCAPECHAR(endword))
				in_escape = true;
			else if (ISWILDCARDCHAR(endword))
			{
				in_trailing_wildcard_meta = true;
				break;
			}
			else if (ISWORDCHR(endword))
			{
				memcpy(s, endword, clen);
				(*charlen)++;
				s += clen;
			}
			else
				break;
		}
		endword += clen;
	}

	/*
	 * Add right padding spaces if next character isn't wildcard
	 * meta-character.
	 */
	if (!in_trailing_wildcard_meta)
	{
		if (RPADDING > 0)
		{
			*s++ = ' ';
			(*charlen)++;
			if (RPADDING > 1)
			{
				*s++ = ' ';
				(*charlen)++;
			}
		}
	}

	*bytelen = s - buf;
	return endword;
}

/*
 * Generates trigrams for wildcard search string.
 *
 * Returns array of trigrams that must occur in any string that matches the
 * wildcard string.  For example, given pattern "a%bcd%" the trigrams
 * " a", "bcd" would be extracted.
 */
TRGM *
generate_wildcard_trgm(const char *str, int slen)
{
	TRGM	   *trg;
	char	   *buf,
			   *buf2;
	trgm	   *tptr;
	int			len,
				charlen,
				bytelen;
	const char *eword;

	/*
	 * Guard against possible overflow in the palloc requests below.  (We
	 * don't worry about the additive constants, since palloc can detect
	 * requests that are a little above MaxAllocSize --- we just need to
	 * prevent integer overflow in the multiplications.)
	 */
	if ((Size) (slen / 2) >= (MaxAllocSize / (sizeof(trgm) * 3)) ||
		(Size) slen >= (MaxAllocSize / pg_database_encoding_max_length()))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory")));

	trg = (TRGM *) palloc(TRGMHDRSIZE + sizeof(trgm) * (slen / 2 + 1) *3);
	trg->flag = ARRKEY;
	SET_VARSIZE(trg, TRGMHDRSIZE);

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return trg;

	tptr = GETARR(trg);

	/* Allocate a buffer for blank-padded, but not yet case-folded, words */
	buf = palloc(sizeof(char) * (slen + 4));

	/*
	 * Extract trigrams from each substring extracted by get_wildcard_part.
	 */
	eword = str;
	while ((eword = get_wildcard_part(eword, slen - (eword - str),
									  buf, &bytelen, &charlen)) != NULL)
	{
#ifdef IGNORECASE
		buf2 = lowerstr_with_len(buf, bytelen);
		bytelen = strlen(buf2);
#else
		buf2 = buf;
#endif

		/*
		 * count trigrams
		 */
		tptr = make_trigrams(tptr, buf2, bytelen, charlen);

#ifdef IGNORECASE
		pfree(buf2);
#endif
	}

	pfree(buf);

	if ((len = tptr - GETARR(trg)) == 0)
		return trg;

	/*
	 * Make trigrams unique.
	 */
	if (len > 1)
	{
		qsort((void *) GETARR(trg), len, sizeof(trgm), comp_trgm);
		len = unique_array(GETARR(trg), len);
	}

	SET_VARSIZE(trg, CALCGTSIZE(ARRKEY, len));

	return trg;
}

uint32
trgm2int(trgm *ptr)
{
	uint32		val = 0;

	val |= *(((unsigned char *) ptr));
	val <<= 8;
	val |= *(((unsigned char *) ptr) + 1);
	val <<= 8;
	val |= *(((unsigned char *) ptr) + 2);

	return val;
}

Datum
show_trgm(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(0);
	TRGM	   *trg;
	Datum	   *d;
	ArrayType  *a;
	trgm	   *ptr;
	int			i;

	trg = generate_trgm(VARDATA(in), VARSIZE(in) - VARHDRSZ);
	d = (Datum *) palloc(sizeof(Datum) * (1 + ARRNELEM(trg)));

	for (i = 0, ptr = GETARR(trg); i < ARRNELEM(trg); i++, ptr++)
	{
		text	   *item = (text *) palloc(VARHDRSZ + Max(12, pg_database_encoding_max_length() * 3));

		if (pg_database_encoding_max_length() > 1 && !ISPRINTABLETRGM(ptr))
		{
			snprintf(VARDATA(item), 12, "0x%06x", trgm2int(ptr));
			SET_VARSIZE(item, VARHDRSZ + strlen(VARDATA(item)));
		}
		else
		{
			SET_VARSIZE(item, VARHDRSZ + 3);
			CPTRGM(VARDATA(item), ptr);
		}
		d[i] = PointerGetDatum(item);
	}

	a = construct_array(
						d,
						ARRNELEM(trg),
						TEXTOID,
						-1,
						false,
						'i'
		);

	for (i = 0; i < ARRNELEM(trg); i++)
		pfree(DatumGetPointer(d[i]));

	pfree(d);
	pfree(trg);
	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_POINTER(a);
}

float4
cnt_sml(TRGM *trg1, TRGM *trg2)
{
	trgm	   *ptr1,
			   *ptr2;
	int			count = 0;
	int			len1,
				len2;

	ptr1 = GETARR(trg1);
	ptr2 = GETARR(trg2);

	len1 = ARRNELEM(trg1);
	len2 = ARRNELEM(trg2);

	/* explicit test is needed to avoid 0/0 division when both lengths are 0 */
	if (len1 <= 0 || len2 <= 0)
		return (float4) 0.0;

	while (ptr1 - GETARR(trg1) < len1 && ptr2 - GETARR(trg2) < len2)
	{
		int			res = CMPTRGM(ptr1, ptr2);

		if (res < 0)
			ptr1++;
		else if (res > 0)
			ptr2++;
		else
		{
			ptr1++;
			ptr2++;
			count++;
		}
	}

#ifdef DIVUNION
	return ((float4) count) / ((float4) (len1 + len2 - count));
#else
	return ((float4) count) / ((float4) ((len1 > len2) ? len1 : len2));
#endif

}

/*
 * Returns whether trg2 contains all trigrams in trg1.
 * This relies on the trigram arrays being sorted.
 */
bool
trgm_contained_by(TRGM *trg1, TRGM *trg2)
{
	trgm	   *ptr1,
			   *ptr2;
	int			len1,
				len2;

	ptr1 = GETARR(trg1);
	ptr2 = GETARR(trg2);

	len1 = ARRNELEM(trg1);
	len2 = ARRNELEM(trg2);

	while (ptr1 - GETARR(trg1) < len1 && ptr2 - GETARR(trg2) < len2)
	{
		int			res = CMPTRGM(ptr1, ptr2);

		if (res < 0)
			return false;
		else if (res > 0)
			ptr2++;
		else
		{
			ptr1++;
			ptr2++;
		}
	}
	if (ptr1 - GETARR(trg1) < len1)
		return false;
	else
		return true;
}

/*
 * Return a palloc'd boolean array showing, for each trigram in "query",
 * whether it is present in the trigram array "key".
 * This relies on the "key" array being sorted, but "query" need not be.
 */
bool *
trgm_presence_map(TRGM *query, TRGM *key)
{
	bool	   *result;
	trgm	   *ptrq = GETARR(query),
			   *ptrk = GETARR(key);
	int			lenq = ARRNELEM(query),
				lenk = ARRNELEM(key),
				i;

	result = (bool *) palloc0(lenq * sizeof(bool));

	/* for each query trigram, do a binary search in the key array */
	for (i = 0; i < lenq; i++)
	{
		int			lo = 0;
		int			hi = lenk;

		while (lo < hi)
		{
			int			mid = (lo + hi) / 2;
			int			res = CMPTRGM(ptrq, ptrk + mid);

			if (res < 0)
				hi = mid;
			else if (res > 0)
				lo = mid + 1;
			else
			{
				result[i] = true;
				break;
			}
		}
		ptrq++;
	}

	return result;
}

Datum
similarity(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_P(0);
	text	   *in2 = PG_GETARG_TEXT_P(1);
	TRGM	   *trg1,
			   *trg2;
	float4		res;

	trg1 = generate_trgm(VARDATA(in1), VARSIZE(in1) - VARHDRSZ);
	trg2 = generate_trgm(VARDATA(in2), VARSIZE(in2) - VARHDRSZ);

	res = cnt_sml(trg1, trg2);

	pfree(trg1);
	pfree(trg2);
	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);

	PG_RETURN_FLOAT4(res);
}

Datum
similarity_dist(PG_FUNCTION_ARGS)
{
	float4		res = DatumGetFloat4(DirectFunctionCall2(similarity,
														 PG_GETARG_DATUM(0),
														 PG_GETARG_DATUM(1)));

	PG_RETURN_FLOAT4(1.0 - res);
}

Datum
similarity_op(PG_FUNCTION_ARGS)
{
	float4		res = DatumGetFloat4(DirectFunctionCall2(similarity,
														 PG_GETARG_DATUM(0),
														 PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res >= trgm_limit);
}
