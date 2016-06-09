/*-------------------------------------------------------------------------
 *
 * to_tsany.c
 *		to_ts* function definitions
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/to_tsany.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "tsearch/ts_cache.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


typedef struct MorphOpaque
{
	Oid			cfg_id;
	int			qoperator;		/* query operator */
} MorphOpaque;


Datum
get_current_ts_config(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(getTSCurrentConfig(true));
}

/*
 * to_tsvector
 */
static int
compareWORD(const void *a, const void *b)
{
	int			res;

	res = tsCompareString(
			   ((const ParsedWord *) a)->word, ((const ParsedWord *) a)->len,
			   ((const ParsedWord *) b)->word, ((const ParsedWord *) b)->len,
						  false);

	if (res == 0)
	{
		if (((const ParsedWord *) a)->pos.pos == ((const ParsedWord *) b)->pos.pos)
			return 0;

		res = (((const ParsedWord *) a)->pos.pos > ((const ParsedWord *) b)->pos.pos) ? 1 : -1;
	}

	return res;
}

static int
uniqueWORD(ParsedWord *a, int32 l)
{
	ParsedWord *ptr,
			   *res;
	int			tmppos;

	if (l == 1)
	{
		tmppos = LIMITPOS(a->pos.pos);
		a->alen = 2;
		a->pos.apos = (uint16 *) palloc(sizeof(uint16) * a->alen);
		a->pos.apos[0] = 1;
		a->pos.apos[1] = tmppos;
		return l;
	}

	res = a;
	ptr = a + 1;

	/*
	 * Sort words with its positions
	 */
	qsort((void *) a, l, sizeof(ParsedWord), compareWORD);

	/*
	 * Initialize first word and its first position
	 */
	tmppos = LIMITPOS(a->pos.pos);
	a->alen = 2;
	a->pos.apos = (uint16 *) palloc(sizeof(uint16) * a->alen);
	a->pos.apos[0] = 1;
	a->pos.apos[1] = tmppos;

	/*
	 * Summarize position information for each word
	 */
	while (ptr - a < l)
	{
		if (!(ptr->len == res->len &&
			  strncmp(ptr->word, res->word, res->len) == 0))
		{
			/*
			 * Got a new word, so put it in result
			 */
			res++;
			res->len = ptr->len;
			res->word = ptr->word;
			tmppos = LIMITPOS(ptr->pos.pos);
			res->alen = 2;
			res->pos.apos = (uint16 *) palloc(sizeof(uint16) * res->alen);
			res->pos.apos[0] = 1;
			res->pos.apos[1] = tmppos;
		}
		else
		{
			/*
			 * The word already exists, so adjust position information. But
			 * before we should check size of position's array, max allowed
			 * value for position and uniqueness of position
			 */
			pfree(ptr->word);
			if (res->pos.apos[0] < MAXNUMPOS - 1 && res->pos.apos[res->pos.apos[0]] != MAXENTRYPOS - 1 &&
				res->pos.apos[res->pos.apos[0]] != LIMITPOS(ptr->pos.pos))
			{
				if (res->pos.apos[0] + 1 >= res->alen)
				{
					res->alen *= 2;
					res->pos.apos = (uint16 *) repalloc(res->pos.apos, sizeof(uint16) * res->alen);
				}
				if (res->pos.apos[0] == 0 || res->pos.apos[res->pos.apos[0]] != LIMITPOS(ptr->pos.pos))
				{
					res->pos.apos[res->pos.apos[0] + 1] = LIMITPOS(ptr->pos.pos);
					res->pos.apos[0]++;
				}
			}
		}
		ptr++;
	}

	return res + 1 - a;
}

/*
 * make value of tsvector, given parsed text
 */
TSVector
make_tsvector(ParsedText *prs)
{
	int			i,
				j,
				lenstr = 0,
				totallen;
	TSVector	in;
	WordEntry  *ptr;
	char	   *str;
	int			stroff;

	prs->curwords = uniqueWORD(prs->words, prs->curwords);
	for (i = 0; i < prs->curwords; i++)
	{
		lenstr += prs->words[i].len;
		if (prs->words[i].alen)
		{
			lenstr = SHORTALIGN(lenstr);
			lenstr += sizeof(uint16) + prs->words[i].pos.apos[0] * sizeof(WordEntryPos);
		}
	}

	if (lenstr > MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", lenstr, MAXSTRPOS)));

	totallen = CALCDATASIZE(prs->curwords, lenstr);
	in = (TSVector) palloc0(totallen);
	SET_VARSIZE(in, totallen);
	in->size = prs->curwords;

	ptr = ARRPTR(in);
	str = STRPTR(in);
	stroff = 0;
	for (i = 0; i < prs->curwords; i++)
	{
		ptr->len = prs->words[i].len;
		ptr->pos = stroff;
		memcpy(str + stroff, prs->words[i].word, prs->words[i].len);
		stroff += prs->words[i].len;
		pfree(prs->words[i].word);
		if (prs->words[i].alen)
		{
			int			k = prs->words[i].pos.apos[0];
			WordEntryPos *wptr;

			if (k > 0xFFFF)
				elog(ERROR, "positions array too long");

			ptr->haspos = 1;
			stroff = SHORTALIGN(stroff);
			*(uint16 *) (str + stroff) = (uint16) k;
			wptr = POSDATAPTR(in, ptr);
			for (j = 0; j < k; j++)
			{
				WEP_SETWEIGHT(wptr[j], 0);
				WEP_SETPOS(wptr[j], prs->words[i].pos.apos[j + 1]);
			}
			stroff += sizeof(uint16) + k * sizeof(WordEntryPos);
			pfree(prs->words[i].pos.apos);
		}
		else
			ptr->haspos = 0;
		ptr++;
	}
	pfree(prs->words);
	return in;
}

Datum
to_tsvector_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *in = PG_GETARG_TEXT_P(1);
	ParsedText	prs;
	TSVector	out;

	prs.lenwords = (VARSIZE(in) - VARHDRSZ) / 6;		/* just estimation of
														 * word's number */
	if (prs.lenwords == 0)
		prs.lenwords = 2;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	parsetext(cfgId, &prs, VARDATA(in), VARSIZE(in) - VARHDRSZ);
	PG_FREE_IF_COPY(in, 1);

	if (prs.curwords)
		out = make_tsvector(&prs);
	else
	{
		pfree(prs.words);
		out = palloc(CALCDATASIZE(0, 0));
		SET_VARSIZE(out, CALCDATASIZE(0, 0));
		out->size = 0;
	}

	PG_RETURN_POINTER(out);
}

Datum
to_tsvector(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(to_tsvector_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}

/*
 * to_tsquery
 */


/*
 * This function is used for morph parsing.
 *
 * The value is passed to parsetext which will call the right dictionary to
 * lexize the word. If it turns out to be a stopword, we push a QI_VALSTOP
 * to the stack.
 *
 * All words belonging to the same variant are pushed as an ANDed list,
 * and different variants are ORed together.
 */
static void
pushval_morph(Datum opaque, TSQueryParserState state, char *strval, int lenval, int16 weight, bool prefix)
{
	int32		count = 0;
	ParsedText	prs;
	uint32		variant,
				pos = 0,
				cntvar = 0,
				cntpos = 0,
				cnt = 0;
	MorphOpaque *data = (MorphOpaque *) DatumGetPointer(opaque);

	prs.lenwords = 4;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	parsetext(data->cfg_id, &prs, strval, lenval);

	if (prs.curwords > 0)
	{
		while (count < prs.curwords)
		{
			/*
			 * Were any stop words removed? If so, fill empty positions with
			 * placeholders linked by an appropriate operator.
			 */
			if (pos > 0 && pos + 1 < prs.words[count].pos.pos)
			{
				while (pos + 1 < prs.words[count].pos.pos)
				{
					/* put placeholders for each missing stop word */
					pushStop(state);
					if (cntpos)
						pushOperator(state, data->qoperator, 1);
					cntpos++;
					pos++;
				}
			}

			/* save current word's position */
			pos = prs.words[count].pos.pos;

			/* Go through all variants obtained from this token */
			cntvar = 0;
			while (count < prs.curwords && pos == prs.words[count].pos.pos)
			{
				variant = prs.words[count].nvariant;

				/* Push all words belonging to the same variant */
				cnt = 0;
				while (count < prs.curwords &&
					   pos == prs.words[count].pos.pos &&
					   variant == prs.words[count].nvariant)
				{
					pushValue(state,
							  prs.words[count].word,
							  prs.words[count].len,
							  weight,
						  ((prs.words[count].flags & TSL_PREFIX) || prefix));
					pfree(prs.words[count].word);
					if (cnt)
						pushOperator(state, OP_AND, 0);
					cnt++;
					count++;
				}

				if (cntvar)
					pushOperator(state, OP_OR, 0);
				cntvar++;
			}

			if (cntpos)
			{
				/* distance may be useful */
				pushOperator(state, data->qoperator, 1);
			}

			cntpos++;
		}

		pfree(prs.words);

	}
	else
		pushStop(state);
}

Datum
to_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(1);
	TSQuery		query;
	MorphOpaque data;

	data.cfg_id = PG_GETARG_OID(0);
	data.qoperator = OP_AND;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  false);

	PG_RETURN_TSQUERY(query);
}

Datum
to_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(to_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}

Datum
plainto_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(1);
	TSQuery		query;
	MorphOpaque data;

	data.cfg_id = PG_GETARG_OID(0);
	data.qoperator = OP_AND;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  true);

	PG_RETURN_POINTER(query);
}

Datum
plainto_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(plainto_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}


Datum
phraseto_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(1);
	TSQuery		query;
	MorphOpaque data;

	data.cfg_id = PG_GETARG_OID(0);
	data.qoperator = OP_PHRASE;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  true);

	PG_RETURN_TSQUERY(query);
}

Datum
phraseto_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(phraseto_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}
