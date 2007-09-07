/*
 * Relevation
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <math.h>

#include "access/gist.h"
#include "access/itup.h"
#include "catalog/namespace.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/pg_list.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "tsvector.h"
#include "query.h"
#include "common.h"

PG_FUNCTION_INFO_V1(rank);
Datum		rank(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(rank_def);
Datum		rank_def(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(rank_cd);
Datum		rank_cd(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(rank_cd_def);
Datum		rank_cd_def(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(get_covers);
Datum		get_covers(PG_FUNCTION_ARGS);

static float weights[] = {0.1, 0.2, 0.4, 1.0};

#define wpos(wep)	( w[ WEP_GETWEIGHT(wep) ] )

#define RANK_NO_NORM		0x00
#define RANK_NORM_LOGLENGTH		0x01
#define RANK_NORM_LENGTH	0x02
#define RANK_NORM_EXTDIST	0x04
#define RANK_NORM_UNIQ		0x08
#define RANK_NORM_LOGUNIQ	0x10
#define DEF_NORM_METHOD		RANK_NO_NORM

static float calc_rank_or(float *w, tsvector * t, QUERYTYPE * q);
static float calc_rank_and(float *w, tsvector * t, QUERYTYPE * q);

/*
 * Returns a weight of a word collocation
 */
static float4
word_distance(int4 w)
{
	if (w > 100)
		return 1e-30;

	return 1.0 / (1.005 + 0.05 * exp(((float4) w) / 1.5 - 2));
}

static int
cnt_length(tsvector * t)
{
	WordEntry  *ptr = ARRPTR(t),
			   *end = (WordEntry *) STRPTR(t);
	int			len = 0,
				clen;

	while (ptr < end)
	{
		if ((clen = POSDATALEN(t, ptr)) == 0)
			len += 1;
		else
			len += clen;
		ptr++;
	}

	return len;
}

static int4
WordECompareITEM(char *eval, char *qval, WordEntry * ptr, ITEM * item)
{
	if (ptr->len == item->length)
		return strncmp(
					   eval + ptr->pos,
					   qval + item->distance,
					   item->length);

	return (ptr->len > item->length) ? 1 : -1;
}

static WordEntry *
find_wordentry(tsvector * t, QUERYTYPE * q, ITEM * item)
{
	WordEntry  *StopLow = ARRPTR(t);
	WordEntry  *StopHigh = (WordEntry *) STRPTR(t);
	WordEntry  *StopMiddle;
	int			difference;

	/* Loop invariant: StopLow <= item < StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = WordECompareITEM(STRPTR(t), GETOPERAND(q), StopMiddle, item);
		if (difference == 0)
			return StopMiddle;
		else if (difference < 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	return NULL;
}


static int
compareITEM(const void *a, const void *b, void *arg)
{
	char *operand = (char *) arg;

	if ((*(ITEM **) a)->length == (*(ITEM **) b)->length)
		return strncmp(operand + (*(ITEM **) a)->distance,
					   operand + (*(ITEM **) b)->distance,
					   (*(ITEM **) b)->length);

	return ((*(ITEM **) a)->length > (*(ITEM **) b)->length) ? 1 : -1;
}

static ITEM **
SortAndUniqItems(char *operand, ITEM * item, int *size)
{
	ITEM	  **res,
			  **ptr,
			  **prevptr;

	ptr = res = (ITEM **) palloc(sizeof(ITEM *) * *size);

	while ((*size)--)
	{
		if (item->type == VAL)
		{
			*ptr = item;
			ptr++;
		}
		item++;
	}

	*size = ptr - res;
	if (*size < 2)
		return res;

	qsort_arg(res, *size, sizeof(ITEM **), compareITEM, (void *) operand);

	ptr = res + 1;
	prevptr = res;

	while (ptr - res < *size)
	{
		if (compareITEM((void *) ptr, (void *) prevptr, (void *) operand) != 0)
		{
			prevptr++;
			*prevptr = *ptr;
		}
		ptr++;
	}

	*size = prevptr + 1 - res;
	return res;
}

static WordEntryPos POSNULL[] = {
	0,
	0
};

static float
calc_rank_and(float *w, tsvector * t, QUERYTYPE * q)
{
	uint16	  **pos;
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
	ITEM	  **item;
	int			size = q->size;

	item = SortAndUniqItems(GETOPERAND(q), GETQUERY(q), &size);
	if (size < 2)
	{
		pfree(item);
		return calc_rank_or(w, t, q);
	}
	pos = (uint16 **) palloc(sizeof(uint16 *) * q->size);
	memset(pos, 0, sizeof(uint16 *) * q->size);
	*(uint16 *) POSNULL = lengthof(POSNULL) - 1;
	WEP_SETPOS(POSNULL[1], MAXENTRYPOS - 1);

	for (i = 0; i < size; i++)
	{
		entry = find_wordentry(t, q, item[i]);
		if (!entry)
			continue;

		if (entry->haspos)
			pos[i] = (uint16 *) _POSDATAPTR(t, entry);
		else
			pos[i] = (uint16 *) POSNULL;


		dimt = *(uint16 *) (pos[i]);
		post = (WordEntryPos *) (pos[i] + 1);
		for (k = 0; k < i; k++)
		{
			if (!pos[k])
				continue;
			lenct = *(uint16 *) (pos[k]);
			ct = (WordEntryPos *) (pos[k] + 1);
			for (l = 0; l < dimt; l++)
			{
				for (p = 0; p < lenct; p++)
				{
					dist = Abs((int) WEP_GETPOS(post[l]) - (int) WEP_GETPOS(ct[p]));
					if (dist || (dist == 0 && (pos[i] == (uint16 *) POSNULL || pos[k] == (uint16 *) POSNULL)))
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
calc_rank_or(float *w, tsvector * t, QUERYTYPE * q)
{
	WordEntry  *entry;
	WordEntryPos *post;
	int4		dimt,
				j,
				i;
	float		res = 0.0;
	ITEM	  **item;
	int			size = q->size;

	*(uint16 *) POSNULL = lengthof(POSNULL) - 1;
	item = SortAndUniqItems(GETOPERAND(q), GETQUERY(q), &size);

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
			dimt = *(uint16 *) POSNULL;
			post = POSNULL + 1;
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
calc_rank(float *w, tsvector * t, QUERYTYPE * q, int4 method)
{
	ITEM	   *item = GETQUERY(q);
	float		res = 0.0;
	int			len;

	if (!t->size || !q->size)
		return 0.0;

	res = (item->type != VAL && item->val == (int4) '&') ?
		calc_rank_and(w, t, q) : calc_rank_or(w, t, q);

	if (res < 0)
		res = 1e-20;

	if ((method & RANK_NORM_LOGLENGTH) && t->size > 0)
		res /= log((double) (cnt_length(t) + 1)) / log(2.0);

	if (method & RANK_NORM_LENGTH)
	{
		len = cnt_length(t);
		if (len > 0)
			res /= (float) len;
	}

	if ((method & RANK_NORM_UNIQ) && t->size > 0)
		res /= (float) (t->size);

	if ((method & RANK_NORM_LOGUNIQ) && t->size > 0)
		res /= log((double) (t->size + 1)) / log(2.0);

	return res;
}

Datum
rank(PG_FUNCTION_ARGS)
{
	ArrayType  *win = (ArrayType *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_DATUM(2));
	int			method = DEF_NORM_METHOD;
	float		res = 0.0;
	float		ws[lengthof(weights)];
	float4	   *arrdata;
	int			i;

	if (ARR_NDIM(win) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight must be one-dimensional")));

	if (ARRNELEMS(win) < lengthof(weights))
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

	if (PG_NARGS() == 4)
		method = PG_GETARG_INT32(3);

	res = calc_rank(ws, txt, query, method);

	PG_FREE_IF_COPY(win, 0);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);
	PG_RETURN_FLOAT4(res);
}

Datum
rank_def(PG_FUNCTION_ARGS)
{
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	float		res = 0.0;
	int			method = DEF_NORM_METHOD;

	if (PG_NARGS() == 3)
		method = PG_GETARG_INT32(2);

	res = calc_rank(weights, txt, query, method);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_FLOAT4(res);
}


typedef struct
{
	ITEM	  **item;
	int16		nitem;
	bool		needfree;
	uint8		wclass;
	int32		pos;
}	DocRepresentation;

static int
compareDocR(const void *a, const void *b)
{
	if (((DocRepresentation *) a)->pos == ((DocRepresentation *) b)->pos)
		return 0;
	return (((DocRepresentation *) a)->pos > ((DocRepresentation *) b)->pos) ? 1 : -1;
}

static bool
checkcondition_ITEM(void *checkval, ITEM * val)
{
	return (bool) (val->istrue);
}

static void
reset_istrue_flag(QUERYTYPE * query)
{
	ITEM	   *item = GETQUERY(query);
	int			i;

	/* reset istrue flag */
	for (i = 0; i < query->size; i++)
	{
		if (item->type == VAL)
			item->istrue = 0;
		item++;
	}
}

typedef struct
{
	int			pos;
	int			p;
	int			q;
	DocRepresentation *begin;
	DocRepresentation *end;
}	Extention;


static bool
Cover(DocRepresentation * doc, int len, QUERYTYPE * query, Extention * ext)
{
	DocRepresentation *ptr;
	int			lastpos = ext->pos;
	int			i;
	bool		found = false;

	reset_istrue_flag(query);

	ext->p = 0x7fffffff;
	ext->q = 0;
	ptr = doc + ext->pos;

	/* find upper bound of cover from current position, move up */
	while (ptr - doc < len)
	{
		for (i = 0; i < ptr->nitem; i++)
			ptr->item[i]->istrue = 1;
		if (TS_execute(GETQUERY(query), NULL, false, checkcondition_ITEM))
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

	reset_istrue_flag(query);

	ptr = doc + lastpos;

	/* find lower bound of cover from founded upper bound, move down */
	while (ptr >= doc + ext->pos)
	{
		for (i = 0; i < ptr->nitem; i++)
			ptr->item[i]->istrue = 1;
		if (TS_execute(GETQUERY(query), NULL, true, checkcondition_ITEM))
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
	return Cover(doc, len, query, ext);
}

static DocRepresentation *
get_docrep(tsvector * txt, QUERYTYPE * query, int *doclen)
{
	ITEM	   *item = GETQUERY(query);
	WordEntry  *entry;
	WordEntryPos *post;
	int4		dimt,
				j,
				i;
	int			len = query->size * 4,
				cur = 0;
	DocRepresentation *doc;
	char	   *operand;

	*(uint16 *) POSNULL = lengthof(POSNULL) - 1;
	doc = (DocRepresentation *) palloc(sizeof(DocRepresentation) * len);
	operand = GETOPERAND(query);
	reset_istrue_flag(query);

	for (i = 0; i < query->size; i++)
	{
		if (item[i].type != VAL || item[i].istrue)
			continue;

		entry = find_wordentry(txt, query, &(item[i]));
		if (!entry)
			continue;

		if (entry->haspos)
		{
			dimt = POSDATALEN(txt, entry);
			post = POSDATAPTR(txt, entry);
		}
		else
		{
			dimt = *(uint16 *) POSNULL;
			post = POSNULL + 1;
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
				ITEM	   *kptr,
						   *iptr = item + i;
				int			k;

				doc[cur].needfree = false;
				doc[cur].nitem = 0;
				doc[cur].item = (ITEM **) palloc(sizeof(ITEM *) * query->size);

				for (k = 0; k < query->size; k++)
				{
					kptr = item + k;
					if (k == i ||
						(item[k].type == VAL &&
						 compareITEM(&kptr, &iptr, operand) == 0))
					{
						doc[cur].item[doc[cur].nitem] = item + k;
						doc[cur].nitem++;
						kptr->istrue = 1;
					}
				}
			}
			else
			{
				doc[cur].needfree = false;
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
		if (cur > 1)
			qsort((void *) doc, cur, sizeof(DocRepresentation), compareDocR);
		return doc;
	}

	pfree(doc);
	return NULL;
}

static float4
calc_rank_cd(float4 *arrdata, tsvector * txt, QUERYTYPE * query, int method)
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

	for (i = 0; i < lengthof(weights); i++)
	{
		invws[i] = ((double) ((arrdata[i] >= 0) ? arrdata[i] : weights[i]));
		if (invws[i] > 1.0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("weight out of range")));
		invws[i] = 1.0 / invws[i];
	}

	doc = get_docrep(txt, query, &doclen);
	if (!doc)
		return 0.0;

	MemSet(&ext, 0, sizeof(Extention));
	while (Cover(doc, doclen, query, &ext))
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
		 * if doc are big enough then ext.q may be equal to ext.p
		 * due to limit of posional information. In this case we 
		 * approximate number of noise word as half cover's
		 * length
		 */
		nNoise = (ext.q - ext.p) - (ext.end - ext.begin);
		if ( nNoise < 0 )
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

	if ((method & RANK_NORM_EXTDIST) && SumDist > 0)
		Wdoc /= ((double) NExtent) / SumDist;

	if ((method & RANK_NORM_UNIQ) && txt->size > 0)
		Wdoc /= (double) (txt->size);

	if ((method & RANK_NORM_LOGUNIQ) && txt->size > 0)
		Wdoc /= log((double) (txt->size + 1)) / log(2.0);

	for (i = 0; i < doclen; i++)
		if (doc[i].needfree)
			pfree(doc[i].item);
	pfree(doc);

	return (float4) Wdoc;
}

Datum
rank_cd(PG_FUNCTION_ARGS)
{
	ArrayType  *win;
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(2));
	int			method = DEF_NORM_METHOD;
	float4		res;

	/*
	 * Pre-8.2, rank_cd took just a plain int as its first argument.
	 * It was a mistake to keep the same C function name while changing the
	 * signature, but it's too late to fix that.  Instead, do a runtime test
	 * to make sure the expected datatype has been passed.  This is needed
	 * to prevent core dumps if tsearch2 function definitions from an old
	 * database are loaded into an 8.2 server.
	 */
	if (get_fn_expr_argtype(fcinfo->flinfo, 0) != FLOAT4ARRAYOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("rank_cd() now takes real[] as its first argument, not integer")));

	/* now safe to dereference the first arg */
	win = (ArrayType *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	if (ARR_NDIM(win) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight must be one-dimensional")));

	if (ARRNELEMS(win) < lengthof(weights))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight is too short")));

	if (ARR_HASNULL(win))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array of weight must not contain nulls")));

	if (PG_NARGS() == 4)
		method = PG_GETARG_INT32(3);

	res = calc_rank_cd((float4 *) ARR_DATA_PTR(win), txt, query, method);

	PG_FREE_IF_COPY(win, 0);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);

	PG_RETURN_FLOAT4(res);
}


Datum
rank_cd_def(PG_FUNCTION_ARGS)
{
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1));
	float4		res;

	res = calc_rank_cd(weights, txt, query, (PG_NARGS() == 3) ? PG_GETARG_DATUM(2) : DEF_NORM_METHOD);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);

	PG_RETURN_FLOAT4(res);
}

/**************debug*************/

typedef struct
{
	char	   *w;
	int2		len;
	int2		pos;
	int2		start;
	int2		finish;
}	DocWord;

static int
compareDocWord(const void *a, const void *b)
{
	if (((DocWord *) a)->pos == ((DocWord *) b)->pos)
		return 0;
	return (((DocWord *) a)->pos > ((DocWord *) b)->pos) ? 1 : -1;
}


Datum
get_covers(PG_FUNCTION_ARGS)
{
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(1));
	WordEntry  *pptr = ARRPTR(txt);
	int			i,
				dlen = 0,
				j,
				cur = 0,
				len = 0,
				rlen;
	DocWord    *dw,
			   *dwptr;
	text	   *out;
	char	   *cptr;
	DocRepresentation *doc;
	int			olddwpos = 0;
	int			ncover = 1;
	Extention	ext;

	doc = get_docrep(txt, query, &rlen);

	if (!doc)
	{
		out = palloc(VARHDRSZ);
		VARATT_SIZEP(out) = VARHDRSZ;
		PG_FREE_IF_COPY(txt, 0);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_POINTER(out);
	}

	for (i = 0; i < txt->size; i++)
	{
		if (!pptr[i].haspos)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("no pos info")));
		dlen += POSDATALEN(txt, &(pptr[i]));
	}

	dwptr = dw = palloc(sizeof(DocWord) * dlen);
	memset(dw, 0, sizeof(DocWord) * dlen);

	for (i = 0; i < txt->size; i++)
	{
		WordEntryPos *posdata = POSDATAPTR(txt, &(pptr[i]));

		for (j = 0; j < POSDATALEN(txt, &(pptr[i])); j++)
		{
			dw[cur].w = STRPTR(txt) + pptr[i].pos;
			dw[cur].len = pptr[i].len;
			dw[cur].pos = WEP_GETPOS(posdata[j]);
			cur++;
		}
		len += (pptr[i].len + 1) * (int) POSDATALEN(txt, &(pptr[i]));
	}
	qsort((void *) dw, dlen, sizeof(DocWord), compareDocWord);

	MemSet(&ext, 0, sizeof(Extention));
	while (Cover(doc, rlen, query, &ext))
	{
		dwptr = dw + olddwpos;
		while (dwptr->pos < ext.p && dwptr - dw < dlen)
			dwptr++;
		olddwpos = dwptr - dw;
		dwptr->start = ncover;
		while (dwptr->pos < ext.q + 1 && dwptr - dw < dlen)
			dwptr++;
		(dwptr - 1)->finish = ncover;
		len += 4 /* {}+two spaces */ + 2 * 16 /* numbers */ ;
		ncover++;
	}

	out = palloc(VARHDRSZ + len);
	cptr = ((char *) out) + VARHDRSZ;
	dwptr = dw;

	while (dwptr - dw < dlen)
	{
		if (dwptr->start)
		{
			sprintf(cptr, "{%d ", dwptr->start);
			cptr = strchr(cptr, '\0');
		}
		memcpy(cptr, dwptr->w, dwptr->len);
		cptr += dwptr->len;
		*cptr = ' ';
		cptr++;
		if (dwptr->finish)
		{
			sprintf(cptr, "}%d ", dwptr->finish);
			cptr = strchr(cptr, '\0');
		}
		dwptr++;
	}

	VARATT_SIZEP(out) = cptr - ((char *) out);

	pfree(dw);
	for (i = 0; i < rlen; i++)
		if (doc[i].needfree)
			pfree(doc[i].item);
	pfree(doc);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_POINTER(out);
}
