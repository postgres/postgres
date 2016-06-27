/*-------------------------------------------------------------------------
 *
 * tsrank.c
 *		rank tsvector by tsquery
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsrank.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "tsearch/ts_utils.h"
#include "utils/array.h"
#include "miscadmin.h"


static const float weights[] = {0.1f, 0.2f, 0.4f, 1.0f};

#define wpos(wep)	( w[ WEP_GETWEIGHT(wep) ] )

#define RANK_NO_NORM			0x00
#define RANK_NORM_LOGLENGTH		0x01
#define RANK_NORM_LENGTH		0x02
#define RANK_NORM_EXTDIST		0x04
#define RANK_NORM_UNIQ			0x08
#define RANK_NORM_LOGUNIQ		0x10
#define RANK_NORM_RDIVRPLUS1	0x20
#define DEF_NORM_METHOD			RANK_NO_NORM

static float calc_rank_or(const float *w, TSVector t, TSQuery q);
static float calc_rank_and(const float *w, TSVector t, TSQuery q);

/*
 * Returns a weight of a word collocation
 */
static float4
word_distance(int32 w)
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


#define WordECompareQueryItem(e,q,p,i,m) \
	tsCompareString((q) + (i)->distance, (i)->length,	\
					(e) + (p)->pos, (p)->len, (m))


/*
 * Returns a pointer to a WordEntry's array corresponding to 'item' from
 * tsvector 't'. 'q' is the TSQuery containing 'item'.
 * Returns NULL if not found.
 */
static WordEntry *
find_wordentry(TSVector t, TSQuery q, QueryOperand *item, int32 *nitem)
{
	WordEntry  *StopLow = ARRPTR(t);
	WordEntry  *StopHigh = (WordEntry *) STRPTR(t);
	WordEntry  *StopMiddle = StopHigh;
	int			difference;

	*nitem = 0;

	/* Loop invariant: StopLow <= item < StopHigh */
	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = WordECompareQueryItem(STRPTR(t), GETOPERAND(q), StopMiddle, item, false);
		if (difference == 0)
		{
			StopHigh = StopMiddle;
			*nitem = 1;
			break;
		}
		else if (difference > 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	if (item->prefix)
	{
		if (StopLow >= StopHigh)
			StopMiddle = StopHigh;

		*nitem = 0;

		while (StopMiddle < (WordEntry *) STRPTR(t) &&
			   WordECompareQueryItem(STRPTR(t), GETOPERAND(q), StopMiddle, item, true) == 0)
		{
			(*nitem)++;
			StopMiddle++;
		}
	}

	return (*nitem > 0) ? StopHigh : NULL;
}


/*
 * sort QueryOperands by (length, word)
 */
static int
compareQueryOperand(const void *a, const void *b, void *arg)
{
	char	   *operand = (char *) arg;
	QueryOperand *qa = (*(QueryOperand *const *) a);
	QueryOperand *qb = (*(QueryOperand *const *) b);

	return tsCompareString(operand + qa->distance, qa->length,
						   operand + qb->distance, qb->length,
						   false);
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

	qsort_arg(res, *size, sizeof(QueryOperand *), compareQueryOperand, (void *) operand);

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

static float
calc_rank_and(const float *w, TSVector t, TSQuery q)
{
	WordEntryPosVector **pos;
	WordEntryPosVector1 posnull;
	WordEntryPosVector *POSNULL;
	int			i,
				k,
				l,
				p;
	WordEntry  *entry,
			   *firstentry;
	WordEntryPos *post,
			   *ct;
	int32		dimt,
				lenct,
				dist,
				nitem;
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

	/* A dummy WordEntryPos array to use when haspos is false */
	posnull.npos = 1;
	posnull.pos[0] = 0;
	WEP_SETPOS(posnull.pos[0], MAXENTRYPOS - 1);
	POSNULL = (WordEntryPosVector *) &posnull;

	for (i = 0; i < size; i++)
	{
		firstentry = entry = find_wordentry(t, q, item[i], &nitem);
		if (!entry)
			continue;

		while (entry - firstentry < nitem)
		{
			if (entry->haspos)
				pos[i] = _POSVECPTR(t, entry);
			else
				pos[i] = POSNULL;

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
						if (dist || (dist == 0 && (pos[i] == POSNULL || pos[k] == POSNULL)))
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

			entry++;
		}
	}
	pfree(pos);
	pfree(item);
	return res;
}

static float
calc_rank_or(const float *w, TSVector t, TSQuery q)
{
	WordEntry  *entry,
			   *firstentry;
	WordEntryPosVector1 posnull;
	WordEntryPos *post;
	int32		dimt,
				j,
				i,
				nitem;
	float		res = 0.0;
	QueryOperand **item;
	int			size = q->size;

	/* A dummy WordEntryPos array to use when haspos is false */
	posnull.npos = 1;
	posnull.pos[0] = 0;

	item = SortAndUniqItems(q, &size);

	for (i = 0; i < size; i++)
	{
		float		resj,
					wjm;
		int32		jm;

		firstentry = entry = find_wordentry(t, q, item[i], &nitem);
		if (!entry)
			continue;

		while (entry - firstentry < nitem)
		{
			if (entry->haspos)
			{
				dimt = POSDATALEN(t, entry);
				post = POSDATAPTR(t, entry);
			}
			else
			{
				dimt = posnull.npos;
				post = posnull.pos;
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

			entry++;
		}
	}
	if (size > 0)
		res = res / size;
	pfree(item);
	return res;
}

static float
calc_rank(const float *w, TSVector t, TSQuery q, int32 method)
{
	QueryItem  *item = GETQUERY(q);
	float		res = 0.0;
	int			len;

	if (!t->size || !q->size)
		return 0.0;

	/* XXX: What about NOT? */
	res = (item->type == QI_OPR && (item->qoperator.oper == OP_AND ||
									item->qoperator.oper == OP_PHRASE)) ?
		calc_rank_and(w, t, q) :
		calc_rank_or(w, t, q);

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

static const float *
getWeights(ArrayType *win)
{
	static float ws[lengthof(weights)];
	int			i;
	float4	   *arrdata;

	if (win == NULL)
		return weights;

	if (ARR_NDIM(win) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight must be one-dimensional")));

	if (ArrayGetNItems(ARR_NDIM(win), ARR_DIMS(win)) < lengthof(weights))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight is too short")));

	if (array_contains_nulls(win))
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
	union
	{
		struct
		{						/* compiled doc representation */
			QueryItem **items;
			int16		nitem;
		}			query;
		struct
		{						/* struct is used for preparing doc
								 * representation */
			QueryItem  *item;
			WordEntry  *entry;
		}			map;
	}			data;
	WordEntryPos pos;
} DocRepresentation;

static int
compareDocR(const void *va, const void *vb)
{
	const DocRepresentation *a = (const DocRepresentation *) va;
	const DocRepresentation *b = (const DocRepresentation *) vb;

	if (WEP_GETPOS(a->pos) == WEP_GETPOS(b->pos))
	{
		if (WEP_GETWEIGHT(a->pos) == WEP_GETWEIGHT(b->pos))
		{
			if (a->data.map.entry == b->data.map.entry)
				return 0;

			return (a->data.map.entry > b->data.map.entry) ? 1 : -1;
		}

		return (WEP_GETWEIGHT(a->pos) > WEP_GETWEIGHT(b->pos)) ? 1 : -1;
	}

	return (WEP_GETPOS(a->pos) > WEP_GETPOS(b->pos)) ? 1 : -1;
}

#define MAXQROPOS	MAXENTRYPOS
typedef struct
{
	bool		operandexists;
	bool		reverseinsert;	/* indicates insert order, true means
								 * descending order */
	uint32		npos;
	WordEntryPos pos[MAXQROPOS];
} QueryRepresentationOperand;

typedef struct
{
	TSQuery		query;
	QueryRepresentationOperand *operandData;
} QueryRepresentation;

#define QR_GET_OPERAND_DATA(q, v) \
	( (q)->operandData + (((QueryItem*)(v)) - GETQUERY((q)->query)) )

static bool
checkcondition_QueryOperand(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	QueryRepresentation *qr = (QueryRepresentation *) checkval;
	QueryRepresentationOperand *opData = QR_GET_OPERAND_DATA(qr, val);

	if (!opData->operandexists)
		return false;

	if (data)
	{
		data->npos = opData->npos;
		data->pos = opData->pos;
		if (opData->reverseinsert)
			data->pos += MAXQROPOS - opData->npos;
	}

	return true;
}

typedef struct
{
	int			pos;
	int			p;
	int			q;
	DocRepresentation *begin;
	DocRepresentation *end;
} CoverExt;

static void
resetQueryRepresentation(QueryRepresentation *qr, bool reverseinsert)
{
	int			i;

	for (i = 0; i < qr->query->size; i++)
	{
		qr->operandData[i].operandexists = false;
		qr->operandData[i].reverseinsert = reverseinsert;
		qr->operandData[i].npos = 0;
	}
}

static void
fillQueryRepresentationData(QueryRepresentation *qr, DocRepresentation *entry)
{
	int			i;
	int			lastPos;
	QueryRepresentationOperand *opData;

	for (i = 0; i < entry->data.query.nitem; i++)
	{
		if (entry->data.query.items[i]->type != QI_VAL)
			continue;

		opData = QR_GET_OPERAND_DATA(qr, entry->data.query.items[i]);

		opData->operandexists = true;

		if (opData->npos == 0)
		{
			lastPos = (opData->reverseinsert) ? (MAXQROPOS - 1) : 0;
			opData->pos[lastPos] = entry->pos;
			opData->npos++;
			continue;
		}

		lastPos = opData->reverseinsert ?
			(MAXQROPOS - opData->npos) :
			(opData->npos - 1);

		if (WEP_GETPOS(opData->pos[lastPos]) != WEP_GETPOS(entry->pos))
		{
			lastPos = opData->reverseinsert ?
				(MAXQROPOS - 1 - opData->npos) :
				(opData->npos);

			opData->pos[lastPos] = entry->pos;
			opData->npos++;
		}
	}
}

static bool
Cover(DocRepresentation *doc, int len, QueryRepresentation *qr, CoverExt *ext)
{
	DocRepresentation *ptr;
	int			lastpos = ext->pos;
	bool		found = false;

	/*
	 * since this function recurses, it could be driven to stack overflow.
	 * (though any decent compiler will optimize away the tail-recursion.
	 */
	check_stack_depth();

	resetQueryRepresentation(qr, false);

	ext->p = INT_MAX;
	ext->q = 0;
	ptr = doc + ext->pos;

	/* find upper bound of cover from current position, move up */
	while (ptr - doc < len)
	{
		fillQueryRepresentationData(qr, ptr);

		if (TS_execute(GETQUERY(qr->query), (void *) qr,
					   TS_EXEC_EMPTY, checkcondition_QueryOperand))
		{
			if (WEP_GETPOS(ptr->pos) > ext->q)
			{
				ext->q = WEP_GETPOS(ptr->pos);
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

	resetQueryRepresentation(qr, true);

	ptr = doc + lastpos;

	/* find lower bound of cover from found upper bound, move down */
	while (ptr >= doc + ext->pos)
	{
		/*
		 * we scan doc from right to left, so pos info in reverse order!
		 */
		fillQueryRepresentationData(qr, ptr);

		if (TS_execute(GETQUERY(qr->query), (void *) qr,
					   TS_EXEC_CALC_NOT, checkcondition_QueryOperand))
		{
			if (WEP_GETPOS(ptr->pos) < ext->p)
			{
				ext->begin = ptr;
				ext->p = WEP_GETPOS(ptr->pos);
			}
			break;
		}
		ptr--;
	}

	if (ext->p <= ext->q)
	{
		/*
		 * set position for next try to next lexeme after beginning of found
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
	WordEntry  *entry,
			   *firstentry;
	WordEntryPos *post;
	int32		dimt,			/* number of 'post' items */
				j,
				i,
				nitem;
	int			len = qr->query->size * 4,
				cur = 0;
	DocRepresentation *doc;

	doc = (DocRepresentation *) palloc(sizeof(DocRepresentation) * len);

	/*
	 * Iterate through query to make DocRepresentaion for words and it's
	 * entries satisfied by query
	 */
	for (i = 0; i < qr->query->size; i++)
	{
		QueryOperand *curoperand;

		if (item[i].type != QI_VAL)
			continue;

		curoperand = &item[i].qoperand;

		firstentry = entry = find_wordentry(txt, qr->query, curoperand, &nitem);
		if (!entry)
			continue;

		/* iterations over entries in tsvector */
		while (entry - firstentry < nitem)
		{
			if (entry->haspos)
			{
				dimt = POSDATALEN(txt, entry);
				post = POSDATAPTR(txt, entry);
			}
			else
			{
				/* ignore words without positions */
				entry++;
				continue;
			}

			while (cur + dimt >= len)
			{
				len *= 2;
				doc = (DocRepresentation *) repalloc(doc, sizeof(DocRepresentation) * len);
			}

			/* iterations over entry's positions */
			for (j = 0; j < dimt; j++)
			{
				if (curoperand->weight == 0 ||
					curoperand->weight & (1 << WEP_GETWEIGHT(post[j])))
				{
					doc[cur].pos = post[j];
					doc[cur].data.map.entry = entry;
					doc[cur].data.map.item = (QueryItem *) curoperand;
					cur++;
				}
			}

			entry++;
		}
	}

	if (cur > 0)
	{
		DocRepresentation *rptr = doc + 1,
				   *wptr = doc,
					storage;

		/*
		 * Sort representation in ascending order by pos and entry
		 */
		qsort((void *) doc, cur, sizeof(DocRepresentation), compareDocR);

		/*
		 * Join QueryItem per WordEntry and it's position
		 */
		storage.pos = doc->pos;
		storage.data.query.items = palloc(sizeof(QueryItem *) * qr->query->size);
		storage.data.query.items[0] = doc->data.map.item;
		storage.data.query.nitem = 1;

		while (rptr - doc < cur)
		{
			if (rptr->pos == (rptr - 1)->pos &&
				rptr->data.map.entry == (rptr - 1)->data.map.entry)
			{
				storage.data.query.items[storage.data.query.nitem] = rptr->data.map.item;
				storage.data.query.nitem++;
			}
			else
			{
				*wptr = storage;
				wptr++;
				storage.pos = rptr->pos;
				storage.data.query.items = palloc(sizeof(QueryItem *) * qr->query->size);
				storage.data.query.items[0] = rptr->data.map.item;
				storage.data.query.nitem = 1;
			}

			rptr++;
		}

		*wptr = storage;
		wptr++;

		*doclen = wptr - doc;
		return doc;
	}

	pfree(doc);
	return NULL;
}

static float4
calc_rank_cd(const float4 *arrdata, TSVector txt, TSQuery query, int method)
{
	DocRepresentation *doc;
	int			len,
				i,
				doclen = 0;
	CoverExt	ext;
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
	qr.operandData = (QueryRepresentationOperand *)
		palloc0(sizeof(QueryRepresentationOperand) * query->size);

	doc = get_docrep(txt, &qr, &doclen);
	if (!doc)
	{
		pfree(qr.operandData);
		return 0.0;
	}

	MemSet(&ext, 0, sizeof(CoverExt));
	while (Cover(doc, doclen, &qr, &ext))
	{
		double		Cpos = 0.0;
		double		InvSum = 0.0;
		int			nNoise;
		DocRepresentation *ptr = ext.begin;

		while (ptr <= ext.end)
		{
			InvSum += invws[WEP_GETWEIGHT(ptr->pos)];
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

	pfree(qr.operandData);

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
