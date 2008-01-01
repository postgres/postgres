/*-------------------------------------------------------------------------
 *
 * tsrank.c
 *		rank tsvector by tsquery
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsrank.c,v 1.12 2008/01/01 19:45:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/array.h"
#include "miscadmin.h"


static float weights[] = {0.1f, 0.2f, 0.4f, 1.0f};

#define wpos(wep)	( w[ WEP_GETWEIGHT(wep) ] )

#define RANK_NO_NORM			0x00
#define RANK_NORM_LOGLENGTH		0x01
#define RANK_NORM_LENGTH		0x02
#define RANK_NORM_EXTDIST		0x04
#define RANK_NORM_UNIQ			0x08
#define RANK_NORM_LOGUNIQ		0x10
#define RANK_NORM_RDIVRPLUS1	0x20
#define DEF_NORM_METHOD			RANK_NO_NORM

static float calc_rank_or(float *w, TSVector t, TSQuery q);
static float calc_rank_and(float *w, TSVector t, TSQuery q);

/*
 * Returns a weight of a word collocation
 */
static float4
word_distance(int4 w)
{
	if (w > 100)
		return 1e-30f;

	return 1.0 / (1.005 + 0.05 * exp(((float4) w) / 1.5 - 2));
}

static int
cnt_length(TSVector t)
{
	WordEntry  *ptr = ARRPTR(t),
			   *end = (WordEntry *) STRPTR(t);
	int			len = 0;

	while (ptr < end)
	{
		int			clen = POSDATALEN(t, ptr);

		if (clen == 0)
			len += 1;
		else
			len += clen;

		ptr++;
	}

	return len;
}

static int
WordECompareQueryItem(char *eval, char *qval, WordEntry *ptr, QueryOperand *item)
{
	if (ptr->len == item->length)
		return strncmp(
					   eval + ptr->pos,
					   qval + item->distance,
					   item->length);

	return (ptr->len > item->length) ? 1 : -1;
}

/*
 * Returns a pointer to a WordEntry corresponding 'item' from tsvector 't'. 'q'
 * is the TSQuery containing 'item'. Returns NULL if not found.
 */
static WordEntry *
find_wordentry(TSVector t, TSQuery q, QueryOperand *item)
{
	WordEntry  *StopLow = ARRPTR(t);
	WordEntry  *StopHigh = (WordEntry *) STRPTR(t);
	WordEntry  *StopMiddle;
	int			difference;

	/* Loop invariant: StopLow <= item < StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = WordECompareQueryItem(STRPTR(t), GETOPERAND(q), StopMiddle, item);
		if (difference == 0)
			return StopMiddle;
		else if (difference < 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	return NULL;
}


/*
 * sort QueryOperands by (length, word)
 */
static int
compareQueryOperand(const void *a, const void *b, void *arg)
{
	char	   *operand = (char *) arg;
	QueryOperand *qa = (*(QueryOperand **) a);
	QueryOperand *qb = (*(QueryOperand **) b);

	if (qa->length == qb->length)
		return strncmp(operand + qa->distance,
					   operand + qb->distance,
					   qb->length);

	return (qa->length > qb->length) ? 1 : -1;
}

/*
 * Returns a sorted, de-duplicated array of QueryOperands in a query.
 * The returned QueryOperands are pointers to the original QueryOperands
 * in the query.
 *
 * Length of the returned array is stored in *size
 */
static QueryOperand **
SortAndUniqItems(TSQuery q, int *size)
{
	char	   *operand = GETOPERAND(q);
	QueryItem  *item = GETQUERY(q);
	QueryOperand **res,
			  **ptr,
			  **prevptr;

	ptr = res = (QueryOperand **) palloc(sizeof(QueryOperand *) * *size);

	/* Collect all operands from the tree to res */
	while ((*size)--)
	{
		if (item->type == QI_VAL)
		{
			*ptr = (QueryOperand *) item;
			ptr++;
		}
		item++;
	}

	*size = ptr - res;
	if (*size < 2)
		return res;

	qsort_arg(res, *size, sizeof(QueryOperand **), compareQueryOperand, (void *) operand);

	ptr = res + 1;
	prevptr = res;

	/* remove duplicates */
	while (ptr - res < *size)
	{
		if (compareQueryOperand((void *) ptr, (void *) prevptr, (void *) operand) != 0)
		{
			prevptr++;
			*prevptr = *ptr;
		}
		ptr++;
	}

	*size = prevptr + 1 - res;
	return res;
}

/* A dummy WordEntryPos array to use when haspos is false */
static WordEntryPosVector POSNULL = {
	1,							/* Number of elements that follow */
	{0}
};

static float
calc_rank_and(float *w, TSVector t, TSQuery q)
{
	WordEntryPosVector **pos;
	int			i,
				k,
				l,
				p;
	WordEntry  *entry;
	WordEntryPos *post,
			   *ct;
	int4		dimt,
				lenct,
				dist;
	float		res = -1.0;
	QueryOperand **item;
	int			size = q->size;

	item = SortAndUniqItems(q, &size);
	if (size < 2)
	{
		pfree(item);
		return calc_rank_or(w, t, q);
	}
	pos = (WordEntryPosVector **) palloc0(sizeof(WordEntryPosVector *) * q->size);
	WEP_SETPOS(POSNULL.pos[0], MAXENTRYPOS - 1);

	for (i = 0; i < size; i++)
	{
		entry = find_wordentry(t, q, item[i]);
		if (!entry)
			continue;

		if (entry->haspos)
			pos[i] = _POSVECPTR(t, entry);
		else
			pos[i] = &POSNULL;


		dimt = pos[i]->npos;
		post = pos[i]->pos;
		for (k = 0; k < i; k++)
		{
			if (!pos[k])
				continue;
			lenct = pos[k]->npos;
			ct = pos[k]->pos;
			for (l = 0; l < dimt; l++)
			{
				for (p = 0; p < lenct; p++)
				{
					dist = Abs((int) WEP_GETPOS(post[l]) - (int) WEP_GETPOS(ct[p]));
					if (dist || (dist == 0 && (pos[i] == &POSNULL || pos[k] == &POSNULL)))
					{
						float		curw;

						if (!dist)
							dist = MAXENTRYPOS;
						curw = sqrt(wpos(post[l]) * wpos(ct[p]) * word_distance(dist));
						res = (res < 0) ? curw : 1.0 - (1.0 - res) * (1.0 - curw);
					}
				}
			}
		}
	}
	pfree(pos);
	pfree(item);
	return res;
}

static float
calc_rank_or(float *w, TSVector t, TSQuery q)
{
	WordEntry  *entry;
	WordEntryPos *post;
	int4		dimt,
				j,
				i;
	float		res = 0.0;
	QueryOperand **item;
	int			size = q->size;

	item = SortAndUniqItems(q, &size);

	for (i = 0; i < size; i++)
	{
		float		resj,
					wjm;
		int4		jm;

		entry = find_wordentry(t, q, item[i]);
		if (!entry)
			continue;

		if (entry->haspos)
		{
			dimt = POSDATALEN(t, entry);
			post = POSDATAPTR(t, entry);
		}
		else
		{
			dimt = POSNULL.npos;
			post = POSNULL.pos;
		}

		resj = 0.0;
		wjm = -1.0;
		jm = 0;
		for (j = 0; j < dimt; j++)
		{
			resj = resj + wpos(post[j]) / ((j + 1) * (j + 1));
			if (wpos(post[j]) > wjm)
			{
				wjm = wpos(post[j]);
				jm = j;
			}
		}
/*
		limit (sum(i/i^2),i->inf) = pi^2/6
		resj = sum(wi/i^2),i=1,noccurence,
		wi - should be sorted desc,
		don't sort for now, just choose maximum weight. This should be corrected
		Oleg Bartunov
*/
		res = res + (wjm + resj - wjm / ((jm + 1) * (jm + 1))) / 1.64493406685;
	}
	if (size > 0)
		res = res / size;
	pfree(item);
	return res;
}

static float
calc_rank(float *w, TSVector t, TSQuery q, int4 method)
{
	QueryItem  *item = GETQUERY(q);
	float		res = 0.0;
	int			len;

	if (!t->size || !q->size)
		return 0.0;

	/* XXX: What about NOT? */
	res = (item->type == QI_OPR && item->operator.oper == OP_AND) ?
		calc_rank_and(w, t, q) : calc_rank_or(w, t, q);

	if (res < 0)
		res = 1e-20f;

	if ((method & RANK_NORM_LOGLENGTH) && t->size > 0)
		res /= log((double) (cnt_length(t) + 1)) / log(2.0);

	if (method & RANK_NORM_LENGTH)
	{
		len = cnt_length(t);
		if (len > 0)
			res /= (float) len;
	}

	/* RANK_NORM_EXTDIST not applicable */

	if ((method & RANK_NORM_UNIQ) && t->size > 0)
		res /= (float) (t->size);

	if ((method & RANK_NORM_LOGUNIQ) && t->size > 0)
		res /= log((double) (t->size + 1)) / log(2.0);

	if (method & RANK_NORM_RDIVRPLUS1)
		res /= (res + 1);

	return res;
}

static float *
getWeights(ArrayType *win)
{
	static float ws[lengthof(weights)];
	int			i;
	float4	   *arrdata;

	if (win == 0)
		return weights;

	if (ARR_NDIM(win) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight must be one-dimensional")));

	if (ArrayGetNItems(ARR_NDIM(win), ARR_DIMS(win)) < lengthof(weights))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight is too short")));

	if (ARR_HASNULL(win))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array of weight must not contain nulls")));

	arrdata = (float4 *) ARR_DATA_PTR(win);
	for (i = 0; i < lengthof(weights); i++)
	{
		ws[i] = (arrdata[i] >= 0) ? arrdata[i] : weights[i];
		if (ws[i] > 1.0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("weight out of range")));
	}

	return ws;
}

Datum
ts_rank_wttf(PG_FUNCTION_ARGS)
{
	ArrayType  *win = (ArrayType *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	TSVector	txt = PG_GETARG_TSVECTOR(1);
	TSQuery		query = PG_GETARG_TSQUERY(2);
	int			method = PG_GETARG_INT32(3);
	float		res;

	res = calc_rank(getWeights(win), txt, query, method);

	PG_FREE_IF_COPY(win, 0);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);
	PG_RETURN_FLOAT4(res);
}

Datum
ts_rank_wtt(PG_FUNCTION_ARGS)
{
	ArrayType  *win = (ArrayType *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	TSVector	txt = PG_GETARG_TSVECTOR(1);
	TSQuery		query = PG_GETARG_TSQUERY(2);
	float		res;

	res = calc_rank(getWeights(win), txt, query, DEF_NORM_METHOD);

	PG_FREE_IF_COPY(win, 0);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);
	PG_RETURN_FLOAT4(res);
}

Datum
ts_rank_ttf(PG_FUNCTION_ARGS)
{
	TSVector	txt = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	int			method = PG_GETARG_INT32(2);
	float		res;

	res = calc_rank(getWeights(NULL), txt, query, method);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_FLOAT4(res);
}

Datum
ts_rank_tt(PG_FUNCTION_ARGS)
{
	TSVector	txt = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	float		res;

	res = calc_rank(getWeights(NULL), txt, query, DEF_NORM_METHOD);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_FLOAT4(res);
}

typedef struct
{
	QueryItem **item;
	int16		nitem;
	uint8		wclass;
	int32		pos;
} DocRepresentation;

static int
compareDocR(const void *va, const void *vb)
{
	DocRepresentation *a = (DocRepresentation *) va;
	DocRepresentation *b = (DocRepresentation *) vb;

	if (a->pos == b->pos)
		return 0;
	return (a->pos > b->pos) ? 1 : -1;
}

typedef struct
{
	TSQuery		query;
	bool	   *operandexist;
} QueryRepresentation;

#define QR_GET_OPERAND_EXISTS(q, v)		( (q)->operandexist[ ((QueryItem*)(v)) - GETQUERY((q)->query) ] )
#define QR_SET_OPERAND_EXISTS(q, v)  QR_GET_OPERAND_EXISTS(q,v) = true

static bool
checkcondition_QueryOperand(void *checkval, QueryOperand *val)
{
	QueryRepresentation *qr = (QueryRepresentation *) checkval;

	return QR_GET_OPERAND_EXISTS(qr, val);
}

typedef struct
{
	int			pos;
	int			p;
	int			q;
	DocRepresentation *begin;
	DocRepresentation *end;
} Extention;


static bool
Cover(DocRepresentation *doc, int len, QueryRepresentation *qr, Extention *ext)
{
	DocRepresentation *ptr;
	int			lastpos = ext->pos;
	int			i;
	bool		found = false;

	/*
	 * since this function recurses, it could be driven to stack overflow.
	 * (though any decent compiler will optimize away the tail-recursion.
	 */
	check_stack_depth();

	memset(qr->operandexist, 0, sizeof(bool) * qr->query->size);

	ext->p = 0x7fffffff;
	ext->q = 0;
	ptr = doc + ext->pos;

	/* find upper bound of cover from current position, move up */
	while (ptr - doc < len)
	{
		for (i = 0; i < ptr->nitem; i++)
		{
			if (ptr->item[i]->type == QI_VAL)
				QR_SET_OPERAND_EXISTS(qr, ptr->item[i]);
		}
		if (TS_execute(GETQUERY(qr->query), (void *) qr, false, checkcondition_QueryOperand))
		{
			if (ptr->pos > ext->q)
			{
				ext->q = ptr->pos;
				ext->end = ptr;
				lastpos = ptr - doc;
				found = true;
			}
			break;
		}
		ptr++;
	}

	if (!found)
		return false;

	memset(qr->operandexist, 0, sizeof(bool) * qr->query->size);

	ptr = doc + lastpos;

	/* find lower bound of cover from found upper bound, move down */
	while (ptr >= doc + ext->pos)
	{
		for (i = 0; i < ptr->nitem; i++)
			if (ptr->item[i]->type == QI_VAL)
				QR_SET_OPERAND_EXISTS(qr, ptr->item[i]);
		if (TS_execute(GETQUERY(qr->query), (void *) qr, true, checkcondition_QueryOperand))
		{
			if (ptr->pos < ext->p)
			{
				ext->begin = ptr;
				ext->p = ptr->pos;
			}
			break;
		}
		ptr--;
	}

	if (ext->p <= ext->q)
	{
		/*
		 * set position for next try to next lexeme after begining of founded
		 * cover
		 */
		ext->pos = (ptr - doc) + 1;
		return true;
	}

	ext->pos++;
	return Cover(doc, len, qr, ext);
}

static DocRepresentation *
get_docrep(TSVector txt, QueryRepresentation *qr, int *doclen)
{
	QueryItem  *item = GETQUERY(qr->query);
	WordEntry  *entry;
	WordEntryPos *post;
	int4		dimt,
				j,
				i;
	int			len = qr->query->size * 4,
				cur = 0;
	DocRepresentation *doc;
	char	   *operand;

	doc = (DocRepresentation *) palloc(sizeof(DocRepresentation) * len);
	operand = GETOPERAND(qr->query);

	for (i = 0; i < qr->query->size; i++)
	{
		QueryOperand *curoperand;

		if (item[i].type != QI_VAL)
			continue;

		curoperand = &item[i].operand;

		if (QR_GET_OPERAND_EXISTS(qr, &item[i]))
			continue;

		entry = find_wordentry(txt, qr->query, curoperand);
		if (!entry)
			continue;

		if (entry->haspos)
		{
			dimt = POSDATALEN(txt, entry);
			post = POSDATAPTR(txt, entry);
		}
		else
		{
			dimt = POSNULL.npos;
			post = POSNULL.pos;
		}

		while (cur + dimt >= len)
		{
			len *= 2;
			doc = (DocRepresentation *) repalloc(doc, sizeof(DocRepresentation) * len);
		}

		for (j = 0; j < dimt; j++)
		{
			if (j == 0)
			{
				int			k;

				doc[cur].nitem = 0;
				doc[cur].item = (QueryItem **) palloc(sizeof(QueryItem *) * qr->query->size);

				for (k = 0; k < qr->query->size; k++)
				{
					QueryOperand *kptr = &item[k].operand;
					QueryOperand *iptr = &item[i].operand;

					if (k == i ||
						(item[k].type == QI_VAL &&
						 compareQueryOperand(&kptr, &iptr, operand) == 0))
					{
						/*
						 * if k == i, we've already checked above that it's
						 * type == Q_VAL
						 */
						doc[cur].item[doc[cur].nitem] = item + k;
						doc[cur].nitem++;
						QR_SET_OPERAND_EXISTS(qr, item + k);
					}
				}
			}
			else
			{
				doc[cur].nitem = doc[cur - 1].nitem;
				doc[cur].item = doc[cur - 1].item;
			}
			doc[cur].pos = WEP_GETPOS(post[j]);
			doc[cur].wclass = WEP_GETWEIGHT(post[j]);
			cur++;
		}
	}

	*doclen = cur;

	if (cur > 0)
	{
		qsort((void *) doc, cur, sizeof(DocRepresentation), compareDocR);
		return doc;
	}

	pfree(doc);
	return NULL;
}

static float4
calc_rank_cd(float4 *arrdata, TSVector txt, TSQuery query, int method)
{
	DocRepresentation *doc;
	int			len,
				i,
				doclen = 0;
	Extention	ext;
	double		Wdoc = 0.0;
	double		invws[lengthof(weights)];
	double		SumDist = 0.0,
				PrevExtPos = 0.0,
				CurExtPos = 0.0;
	int			NExtent = 0;
	QueryRepresentation qr;


	for (i = 0; i < lengthof(weights); i++)
	{
		invws[i] = ((double) ((arrdata[i] >= 0) ? arrdata[i] : weights[i]));
		if (invws[i] > 1.0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("weight out of range")));
		invws[i] = 1.0 / invws[i];
	}

	qr.query = query;
	qr.operandexist = (bool *) palloc0(sizeof(bool) * query->size);

	doc = get_docrep(txt, &qr, &doclen);
	if (!doc)
	{
		pfree(qr.operandexist);
		return 0.0;
	}

	MemSet(&ext, 0, sizeof(Extention));
	while (Cover(doc, doclen, &qr, &ext))
	{
		double		Cpos = 0.0;
		double		InvSum = 0.0;
		int			nNoise;
		DocRepresentation *ptr = ext.begin;

		while (ptr <= ext.end)
		{
			InvSum += invws[ptr->wclass];
			ptr++;
		}

		Cpos = ((double) (ext.end - ext.begin + 1)) / InvSum;

		/*
		 * if doc are big enough then ext.q may be equal to ext.p due to limit
		 * of posional information. In this case we approximate number of
		 * noise word as half cover's length
		 */
		nNoise = (ext.q - ext.p) - (ext.end - ext.begin);
		if (nNoise < 0)
			nNoise = (ext.end - ext.begin) / 2;
		Wdoc += Cpos / ((double) (1 + nNoise));

		CurExtPos = ((double) (ext.q + ext.p)) / 2.0;
		if (NExtent > 0 && CurExtPos > PrevExtPos		/* prevent devision by
														 * zero in a case of
				multiple lexize */ )
			SumDist += 1.0 / (CurExtPos - PrevExtPos);

		PrevExtPos = CurExtPos;
		NExtent++;
	}

	if ((method & RANK_NORM_LOGLENGTH) && txt->size > 0)
		Wdoc /= log((double) (cnt_length(txt) + 1));

	if (method & RANK_NORM_LENGTH)
	{
		len = cnt_length(txt);
		if (len > 0)
			Wdoc /= (double) len;
	}

	if ((method & RANK_NORM_EXTDIST) && NExtent > 0 && SumDist > 0)
		Wdoc /= ((double) NExtent) / SumDist;

	if ((method & RANK_NORM_UNIQ) && txt->size > 0)
		Wdoc /= (double) (txt->size);

	if ((method & RANK_NORM_LOGUNIQ) && txt->size > 0)
		Wdoc /= log((double) (txt->size + 1)) / log(2.0);

	if (method & RANK_NORM_RDIVRPLUS1)
		Wdoc /= (Wdoc + 1);

	pfree(doc);

	pfree(qr.operandexist);

	return (float4) Wdoc;
}

Datum
ts_rankcd_wttf(PG_FUNCTION_ARGS)
{
	ArrayType  *win = (ArrayType *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	TSVector	txt = PG_GETARG_TSVECTOR(1);
	TSQuery		query = PG_GETARG_TSQUERY(2);
	int			method = PG_GETARG_INT32(3);
	float		res;

	res = calc_rank_cd(getWeights(win), txt, query, method);

	PG_FREE_IF_COPY(win, 0);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);
	PG_RETURN_FLOAT4(res);
}

Datum
ts_rankcd_wtt(PG_FUNCTION_ARGS)
{
	ArrayType  *win = (ArrayType *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	TSVector	txt = PG_GETARG_TSVECTOR(1);
	TSQuery		query = PG_GETARG_TSQUERY(2);
	float		res;

	res = calc_rank_cd(getWeights(win), txt, query, DEF_NORM_METHOD);

	PG_FREE_IF_COPY(win, 0);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);
	PG_RETURN_FLOAT4(res);
}

Datum
ts_rankcd_ttf(PG_FUNCTION_ARGS)
{
	TSVector	txt = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	int			method = PG_GETARG_INT32(2);
	float		res;

	res = calc_rank_cd(getWeights(NULL), txt, query, method);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_FLOAT4(res);
}

Datum
ts_rankcd_tt(PG_FUNCTION_ARGS)
{
	TSVector	txt = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	float		res;

	res = calc_rank_cd(getWeights(NULL), txt, query, DEF_NORM_METHOD);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_FLOAT4(res);
}
