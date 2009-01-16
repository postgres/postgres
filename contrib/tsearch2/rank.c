/*
 * Relevation
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"
#include <math.h>

#include "access/gist.h"
#include "access/itup.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "funcapi.h"
#include "storage/bufpage.h"
#include "executor/spi.h"
#include "commands/trigger.h"
#include "nodes/pg_list.h"
#include "catalog/namespace.h"

#include "utils/array.h"

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

#define wpos(wep)	( w[ ((WordEntryPos*)(wep))->weight ] )

#define DEF_NORM_METHOD 0

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


static char * SortAndUniqOperand=NULL;

static int
compareITEM( const void * a, const void * b ) {
	if (  (*(ITEM**)a)->length == (*(ITEM**)b)->length )
		return strncmp( SortAndUniqOperand + (*(ITEM**)a)->distance,
				SortAndUniqOperand + (*(ITEM**)b)->distance,
				(*(ITEM**)b)->length );

	return ((*(ITEM**)a)->length > (*(ITEM**)b)->length) ? 1 : -1;
}
         
static ITEM**
SortAndUniqItems( char *operand, ITEM *item, int *size ) {
	ITEM   **res, **ptr, **prevptr;

	ptr = res = (ITEM**) palloc( sizeof(ITEM*) * *size );

	while( (*size)-- ) {
		if ( item->type == VAL ) {
			*ptr = item;
			ptr++;
		}   
		item++;
	}

	*size = ptr-res;
	if ( *size < 2 )
		return res;

	SortAndUniqOperand=operand;
	qsort( res, *size, sizeof(ITEM**), compareITEM );

	ptr = res + 1;
	prevptr = res;

	while( ptr - res < *size ) {
		if ( compareITEM( (void*) ptr, (void*) prevptr ) != 0 ) {
			prevptr++;
			*prevptr = *ptr;
		}
		ptr++;
	}

	*size = prevptr + 1 - res;
	return res;
}

static WordEntryPos POSNULL[] = {
	{0, 0},
	{0, MAXENTRYPOS - 1}
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
	ITEM	   **item;
	int size = q->size;

	item = SortAndUniqItems( GETOPERAND(q), GETQUERY(q), &size);
	if ( size < 2 ) {
		pfree(item);
		return calc_rank_or(w, t, q);
	}

	pos = (uint16 **) palloc(sizeof(uint16 *) * q->size);
	memset(pos, 0, sizeof(uint16 **) * q->size);
	*(uint16 *) POSNULL = lengthof(POSNULL) - 1;

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
					dist = abs(post[l].pos - ct[p].pos);
					if (dist || (dist == 0 && (pos[i] == (uint16 *) POSNULL || pos[k] == (uint16 *) POSNULL)))
					{
						float		curw;

						if (!dist)
							dist = MAXENTRYPOS;
						curw = sqrt(wpos(&(post[l])) * wpos(&(ct[p])) * word_distance(dist));
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
	float		res = -1.0;
	ITEM	   **item;
	int	size = q->size;

	*(uint16 *) POSNULL = lengthof(POSNULL) - 1;
	item = SortAndUniqItems( GETOPERAND(q), GETQUERY(q), &size);

	for (i = 0; i < size; i++)
	{
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

		for (j = 0; j < dimt; j++)
		{
			if (res < 0)
				res = wpos(&(post[j]));
			else
				res = 1.0 - (1.0 - res) * (1.0 - wpos(&(post[j])));
		}
	}
	pfree( item );
	return res;
}

static float
calc_rank(float *w, tsvector * t, QUERYTYPE * q, int4 method)
{
	ITEM	   *item = GETQUERY(q);
	float		res = 0.0;
	int 	   len;

	if (!t->size || !q->size)
		return 0.0;

	res = (item->type != VAL && item->val == (int4) '&') ?
		calc_rank_and(w, t, q) : calc_rank_or(w, t, q);

	if (res < 0)
		res = 1e-20;

	switch (method)
	{
		case 0:
			break;
		case 1:
			res /= log( (float)(cnt_length(t)+1) ) / log(2.0);
			break;
		case 2:
			len = cnt_length(t);
			if ( len > 0 )  res /= (float)len; 
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized normalization method: %d", method);
	}

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
	int			i;

	if (ARR_NDIM(win) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight must be one-dimensional")));

	if (ARRNELEMS(win) < lengthof(weights))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight is too short")));

	for (i = 0; i < lengthof(weights); i++)
	{
		ws[i] = (((float4 *) ARR_DATA_PTR(win))[i] >= 0) ? ((float4 *) ARR_DATA_PTR(win))[i] : weights[i];
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
	ITEM	   *item;
	int32		pos;
}	DocRepresentation;

static int
compareDocR(const void *a, const void *b)
{
	if (((DocRepresentation *) a)->pos == ((DocRepresentation *) b)->pos)
		return 0;
	return (((DocRepresentation *) a)->pos > ((DocRepresentation *) b)->pos) ? 1 : -1;
}


typedef struct
{
	DocRepresentation *doc;
	int			len;
}	ChkDocR;

static bool
checkcondition_DR(void *checkval, ITEM * val)
{
	DocRepresentation *ptr = ((ChkDocR *) checkval)->doc;

	while (ptr - ((ChkDocR *) checkval)->doc < ((ChkDocR *) checkval)->len)
	{
		if ( val == ptr->item || compareITEM( &val, &(ptr->item) ) == 0 )
			return true;
		ptr++;
	}

	return false;
}


static bool
Cover(DocRepresentation * doc, int len, QUERYTYPE * query, int *pos, int *p, int *q)
{
	int			i;
	DocRepresentation *ptr,
			   *f = (DocRepresentation *) 0xffffffff;
	ITEM	   *item = GETQUERY(query);
	int			lastpos = *pos;
	int			oldq = *q;

	*p = 0x7fffffff;
	*q = 0;

	for (i = 0; i < query->size; i++)
	{
		if (item->type != VAL)
		{
			item++;
			continue;
		}
		ptr = doc + *pos;

		while (ptr - doc < len)
		{
			if (ptr->item == item)
			{
				if (ptr->pos > *q)
				{
					*q = ptr->pos;
					lastpos = ptr - doc;
				}
				break;
			}
			ptr++;
		}

		item++;
	}

	if (*q == 0)
		return false;

	if (*q == oldq)
	{							/* already check this pos */
		(*pos)++;
		return Cover(doc, len, query, pos, p, q);
	}

	item = GETQUERY(query);
	for (i = 0; i < query->size; i++)
	{
		if (item->type != VAL)
		{
			item++;
			continue;
		}
		ptr = doc + lastpos;

		while (ptr >= doc + *pos)
		{
			if (ptr->item == item)
			{
				if (ptr->pos < *p)
				{
					*p = ptr->pos;
					f = ptr;
				}
				break;
			}
			ptr--;
		}
		item++;
	}

	if (*p <= *q)
	{
		ChkDocR		ch = {f, (doc + lastpos) - f + 1};

		*pos = f - doc + 1;
		SortAndUniqOperand = GETOPERAND(query); 
		if (TS_execute(GETQUERY(query), &ch, false, checkcondition_DR))
		{
			/*
			 * elog(NOTICE,"OP:%d NP:%d P:%d Q:%d", *pos, lastpos, *p,
			 * *q);
			 */
			return true;
		}
		else
			return Cover(doc, len, query, pos, p, q);
	}

	return false;
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

	*(uint16 *) POSNULL = lengthof(POSNULL) - 1;
	doc = (DocRepresentation *) palloc(sizeof(DocRepresentation) * len);
	for (i = 0; i < query->size; i++)
	{
		if (item[i].type != VAL)
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
			doc[cur].item = &(item[i]);
			doc[cur].pos = post[j].pos;
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


Datum
rank_cd(PG_FUNCTION_ARGS)
{
	int			K = PG_GETARG_INT32(0);
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_DATUM(2));
	int			method = DEF_NORM_METHOD;
	DocRepresentation *doc;
	float		res = 0.0;
	int			p = 0,
				q = 0,
				len,
				cur;

	doc = get_docrep(txt, query, &len);
	if (!doc)
	{
		PG_FREE_IF_COPY(txt, 1);
		PG_FREE_IF_COPY(query, 2);
		PG_RETURN_FLOAT4(0.0);
	}

	cur = 0;
	if (K <= 0)
		K = 4;
	while (Cover(doc, len, query, &cur, &p, &q))
		res += (q - p + 1 > K) ? ((float) K) / ((float) (q - p + 1)) : 1.0;

	if (PG_NARGS() == 4)
		method = PG_GETARG_INT32(3);

	switch (method)
	{
		case 0:
			break;
		case 1:
			res /= log( (float)(cnt_length(txt)+1) );
			break;
		case 2:
			len = cnt_length(txt);
			if ( len > 0 )  res /= (float)len; 
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized normalization method: %d", method);
	}

	pfree(doc);
	PG_FREE_IF_COPY(txt, 1);
	PG_FREE_IF_COPY(query, 2);

	PG_RETURN_FLOAT4(res);
}


Datum
rank_cd_def(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(
										rank_cd,
										Int32GetDatum(-1),
										PG_GETARG_DATUM(0),
										PG_GETARG_DATUM(1),
										(PG_NARGS() == 3) ? PG_GETARG_DATUM(2) : Int32GetDatum(DEF_NORM_METHOD)
										));
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
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
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
	int			pos = 0,
				p = 0,
				q = 0,
				olddwpos = 0;
	int			ncover = 1;

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
			dw[cur].pos = posdata[j].pos;
			cur++;
		}
		len += (pptr[i].len + 1) * (int) POSDATALEN(txt, &(pptr[i]));
	}
	qsort((void *) dw, dlen, sizeof(DocWord), compareDocWord);

	while (Cover(doc, rlen, query, &pos, &p, &q))
	{
		dwptr = dw + olddwpos;
		while (dwptr->pos < p && dwptr - dw < dlen)
			dwptr++;
		olddwpos = dwptr - dw;
		dwptr->start = ncover;
		while (dwptr->pos < q + 1 && dwptr - dw < dlen)
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
	pfree(doc);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_POINTER(out);
}
