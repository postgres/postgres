/*
 * $PostgreSQL: pgsql/contrib/pg_trgm/trgm_op.c,v 1.12 2009/06/11 14:48:51 momjian Exp $
 */
#include "trgm.h"
#include <ctype.h>
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "tsearch/ts_locale.h"

PG_MODULE_MAGIC;

float4		trgm_limit = 0.3f;

PG_FUNCTION_INFO_V1(set_limit);
Datum		set_limit(PG_FUNCTION_ARGS);
Datum
set_limit(PG_FUNCTION_ARGS)
{
	float4		nlimit = PG_GETARG_FLOAT4(0);

	if (nlimit < 0 || nlimit > 1.0)
		elog(ERROR, "wrong limit, should be between 0 and 1");
	trgm_limit = nlimit;
	PG_RETURN_FLOAT4(trgm_limit);
}

PG_FUNCTION_INFO_V1(show_limit);
Datum		show_limit(PG_FUNCTION_ARGS);
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

#ifdef KEEPONLYALNUM
#define iswordchr(c)	(t_isalpha(c) || t_isdigit(c))
#else
#define iswordchr(c)	(!t_isspace(c))
#endif

/*
 * Finds first word in string, returns pointer to the word,
 * endword points to the character after word
 */
static char *
find_word(char *str, int lenstr, char **endword, int *charlen)
{
	char	   *beginword = str;

	while (beginword - str < lenstr && !iswordchr(beginword))
		beginword += pg_mblen(beginword);

	if (beginword - str >= lenstr)
		return NULL;

	*endword = beginword;
	*charlen = 0;
	while (*endword - str < lenstr && iswordchr(*endword))
	{
		*endword += pg_mblen(*endword);
		(*charlen)++;
	}

	return beginword;
}

#ifdef USE_WIDE_UPPER_LOWER
static void
cnt_trigram(trgm *tptr, char *str, int bytelen)
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
#endif

/*
 * Adds trigramm from words (already padded).
 */
static trgm *
make_trigrams(trgm *tptr, char *str, int bytelen, int charlen)
{
	char	   *ptr = str;

	if (charlen < 3)
		return tptr;

#ifdef USE_WIDE_UPPER_LOWER
	if (pg_database_encoding_max_length() > 1)
	{
		int			lenfirst = pg_mblen(str),
					lenmiddle = pg_mblen(str + lenfirst),
					lenlast = pg_mblen(str + lenfirst + lenmiddle);

		while ((ptr - str) + lenfirst + lenmiddle + lenlast <= bytelen)
		{
			cnt_trigram(tptr, ptr, lenfirst + lenmiddle + lenlast);

			ptr += lenfirst;
			tptr++;

			lenfirst = lenmiddle;
			lenmiddle = lenlast;
			lenlast = pg_mblen(ptr + lenfirst + lenmiddle);
		}
	}
	else
#endif
	{
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

	trg = (TRGM *) palloc(TRGMHDRSIZE + sizeof(trgm) * (slen / 2 + 1) *3);
	trg->flag = ARRKEY;
	SET_VARSIZE(trg, TRGMHDRSIZE);

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return trg;

	tptr = GETARR(trg);

	buf = palloc(sizeof(char) * (slen + 4));

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

	if (len > 0)
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

PG_FUNCTION_INFO_V1(show_trgm);
Datum		show_trgm(PG_FUNCTION_ARGS);
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
	return ((((float4) count) / ((float4) (len1 + len2 - count))));
#else
	return (((float) count) / ((float) ((len1 > len2) ? len1 : len2)));
#endif

}

PG_FUNCTION_INFO_V1(similarity);
Datum		similarity(PG_FUNCTION_ARGS);
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

PG_FUNCTION_INFO_V1(similarity_op);
Datum		similarity_op(PG_FUNCTION_ARGS);
Datum
similarity_op(PG_FUNCTION_ARGS)
{
	float4		res = DatumGetFloat4(DirectFunctionCall2(
														 similarity,
														 PG_GETARG_DATUM(0),
														 PG_GETARG_DATUM(1)
														 ));

	PG_RETURN_BOOL(res >= trgm_limit);
}
