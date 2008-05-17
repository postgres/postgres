/*
 * $PostgreSQL: pgsql/contrib/pg_trgm/trgm_op.c,v 1.10 2008/05/17 01:28:21 adunstan Exp $ 
 */
#include "trgm.h"
#include <ctype.h>
#include "utils/array.h"
#include "catalog/pg_type.h"

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

#define WORDWAIT		0
#define INWORD			1

static int
comp_trgm(const void *a, const void *b)
{
	return CMPTRGM(a, b);
}

static int
unique_array(trgm * a, int len)
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


TRGM *
generate_trgm(char *str, int slen)
{
	TRGM	   *trg;
	char	   *buf,
			   *sptr,
			   *bufptr;
	trgm	   *tptr;
	int			state = WORDWAIT;
	int			wl,
				len;

	trg = (TRGM *) palloc(TRGMHDRSIZE + sizeof(trgm) * (slen / 2 + 1) * 3);
	trg->flag = ARRKEY;
	SET_VARSIZE(trg, TRGMHDRSIZE);

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return trg;

	tptr = GETARR(trg);

	buf = palloc(sizeof(char) * (slen + 4));
	sptr = str;

	if (LPADDING > 0)
	{
		*buf = ' ';
		if (LPADDING > 1)
			*(buf + 1) = ' ';
	}

	bufptr = buf + LPADDING;
	while (sptr - str < slen)
	{
		if (state == WORDWAIT)
		{
			if (
#ifdef	KEEPONLYALNUM
				isalnum((unsigned char) *sptr)
#else
				!isspace((unsigned char) *sptr)
#endif
				)
			{
				*bufptr = *sptr;	/* start put word in buffer */
				bufptr++;
				state = INWORD;
				if (sptr - str == slen - 1 /* last char */ )
					goto gettrg;
			}
		}
		else
		{
			if (
#ifdef	KEEPONLYALNUM
				!isalnum((unsigned char) *sptr)
#else
				isspace((unsigned char) *sptr)
#endif
				)
			{
		gettrg:
				/* word in buffer, so count trigrams */
				*bufptr = ' ';
				*(bufptr + 1) = ' ';
				wl = bufptr - (buf + LPADDING) - 2 + LPADDING + RPADDING;
				if (wl <= 0)
				{
					bufptr = buf + LPADDING;
					state = WORDWAIT;
					sptr++;
					continue;
				}

#ifdef IGNORECASE
				do
				{				/* lower word */
					int			wwl = bufptr - buf;

					bufptr = buf + LPADDING;
					while (bufptr - buf < wwl)
					{
						*bufptr = tolower((unsigned char) *bufptr);
						bufptr++;
					}
				} while (0);
#endif
				bufptr = buf;
				/* set trigrams */
				while (bufptr - buf < wl)
				{
					CPTRGM(tptr, bufptr);
					bufptr++;
					tptr++;
				}
				bufptr = buf + LPADDING;
				state = WORDWAIT;
			}
			else
			{
				*bufptr = *sptr;	/* put in buffer */
				bufptr++;
				if (sptr - str == slen - 1)
					goto gettrg;
			}
		}
		sptr++;
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
		text	   *item = (text *) palloc(VARHDRSZ + 3);

		SET_VARSIZE(item, VARHDRSZ + 3);
		CPTRGM(VARDATA(item), ptr);
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
cnt_sml(TRGM * trg1, TRGM * trg2)
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
