/*-------------------------------------------------------------------------
 *
 * tsvector_op.c
 *	  operations over tsvector
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsvector_op.c,v 1.1 2007/08/21 01:11:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


typedef struct
{
	WordEntry  *arrb;
	WordEntry  *arre;
	char	   *values;
	char	   *operand;
} CHKVAL;

typedef struct
{
	uint32		cur;
	TSVector	stat;
} StatStorage;

typedef struct
{
	uint32		len;
	uint32		pos;
	uint32		ndoc;
	uint32		nentry;
} StatEntry;

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int4		size;
	int4		weight;
	char		data[1];
} tsstat;

#define STATHDRSIZE (sizeof(int4) * 4)
#define CALCSTATSIZE(x, lenstr) ( (x) * sizeof(StatEntry) + STATHDRSIZE + (lenstr) )
#define STATPTR(x)	( (StatEntry*) ( (char*)(x) + STATHDRSIZE ) )
#define STATSTRPTR(x)	( (char*)(x) + STATHDRSIZE + ( sizeof(StatEntry) * ((TSVector)(x))->size ) )
#define STATSTRSIZE(x)	( VARSIZE((TSVector)(x)) - STATHDRSIZE - ( sizeof(StatEntry) * ((TSVector)(x))->size ) )


static Datum tsvector_update_trigger(PG_FUNCTION_ARGS, bool config_column);


static int
silly_cmp_tsvector(const TSVector a, const TSVector b)
{
	if (VARSIZE(a) < VARSIZE(b))
		return -1;
	else if (VARSIZE(a) > VARSIZE(b))
		return 1;
	else if (a->size < b->size)
		return -1;
	else if (a->size > b->size)
		return 1;
	else
	{
		WordEntry  *aptr = ARRPTR(a);
		WordEntry  *bptr = ARRPTR(b);
		int			i = 0;
		int			res;


		for (i = 0; i < a->size; i++)
		{
			if (aptr->haspos != bptr->haspos)
			{
				return (aptr->haspos > bptr->haspos) ? -1 : 1;
			}
			else if (aptr->len != bptr->len)
			{
				return (aptr->len > bptr->len) ? -1 : 1;
			}
			else if ((res = strncmp(STRPTR(a) + aptr->pos, STRPTR(b) + bptr->pos, bptr->len)) != 0)
			{
				return res;
			}
			else if (aptr->haspos)
			{
				WordEntryPos *ap = POSDATAPTR(a, aptr);
				WordEntryPos *bp = POSDATAPTR(b, bptr);
				int			j;

				if (POSDATALEN(a, aptr) != POSDATALEN(b, bptr))
					return (POSDATALEN(a, aptr) > POSDATALEN(b, bptr)) ? -1 : 1;

				for (j = 0; j < POSDATALEN(a, aptr); j++)
				{
					if (WEP_GETPOS(*ap) != WEP_GETPOS(*bp))
					{
						return (WEP_GETPOS(*ap) > WEP_GETPOS(*bp)) ? -1 : 1;
					}
					else if (WEP_GETWEIGHT(*ap) != WEP_GETWEIGHT(*bp))
					{
						return (WEP_GETWEIGHT(*ap) > WEP_GETWEIGHT(*bp)) ? -1 : 1;
					}
					ap++, bp++;
				}
			}

			aptr++;
			bptr++;
		}
	}

	return 0;
}

#define TSVECTORCMPFUNC( type, action, ret )			\
Datum													\
tsvector_##type(PG_FUNCTION_ARGS)						\
{														\
	TSVector	a = PG_GETARG_TSVECTOR(0);				\
	TSVector	b = PG_GETARG_TSVECTOR(1);				\
	int			res = silly_cmp_tsvector(a, b);			\
	PG_FREE_IF_COPY(a,0);								\
	PG_FREE_IF_COPY(b,1);								\
	PG_RETURN_##ret( res action 0 );					\
}

TSVECTORCMPFUNC(lt, <, BOOL);
TSVECTORCMPFUNC(le, <=, BOOL);
TSVECTORCMPFUNC(eq, ==, BOOL);
TSVECTORCMPFUNC(ge, >=, BOOL);
TSVECTORCMPFUNC(gt, >, BOOL);
TSVECTORCMPFUNC(ne, !=, BOOL);
TSVECTORCMPFUNC(cmp, +, INT32);

Datum
tsvector_strip(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	TSVector	out;
	int			i,
				len = 0;
	WordEntry  *arrin = ARRPTR(in),
			   *arrout;
	char	   *cur;

	for (i = 0; i < in->size; i++)
		len += SHORTALIGN(arrin[i].len);

	len = CALCDATASIZE(in->size, len);
	out = (TSVector) palloc0(len);
	SET_VARSIZE(out, len);
	out->size = in->size;
	arrout = ARRPTR(out);
	cur = STRPTR(out);
	for (i = 0; i < in->size; i++)
	{
		memcpy(cur, STRPTR(in) + arrin[i].pos, arrin[i].len);
		arrout[i].haspos = 0;
		arrout[i].len = arrin[i].len;
		arrout[i].pos = cur - STRPTR(out);
		cur += SHORTALIGN(arrout[i].len);
	}

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_POINTER(out);
}

Datum
tsvector_length(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	int4		ret = in->size;

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_INT32(ret);
}

Datum
tsvector_setweight(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	char		cw = PG_GETARG_CHAR(1);
	TSVector	out;
	int			i,
				j;
	WordEntry  *entry;
	WordEntryPos *p;
	int			w = 0;

	switch (cw)
	{
		case 'A':
		case 'a':
			w = 3;
			break;
		case 'B':
		case 'b':
			w = 2;
			break;
		case 'C':
		case 'c':
			w = 1;
			break;
		case 'D':
		case 'd':
			w = 0;
			break;
			/* internal error */
		default:
			elog(ERROR, "unrecognized weight");
	}

	out = (TSVector) palloc(VARSIZE(in));
	memcpy(out, in, VARSIZE(in));
	entry = ARRPTR(out);
	i = out->size;
	while (i--)
	{
		if ((j = POSDATALEN(out, entry)) != 0)
		{
			p = POSDATAPTR(out, entry);
			while (j--)
			{
				WEP_SETWEIGHT(*p, w);
				p++;
			}
		}
		entry++;
	}

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_POINTER(out);
}

static int
compareEntry(char *ptra, WordEntry * a, char *ptrb, WordEntry * b)
{
	if (a->len == b->len)
	{
		return strncmp(
					   ptra + a->pos,
					   ptrb + b->pos,
					   a->len);
	}
	return (a->len > b->len) ? 1 : -1;
}

static int4
add_pos(TSVector src, WordEntry * srcptr, TSVector dest, WordEntry * destptr, int4 maxpos)
{
	uint16	   *clen = (uint16 *) _POSDATAPTR(dest, destptr);
	int			i;
	uint16		slen = POSDATALEN(src, srcptr),
				startlen;
	WordEntryPos *spos = POSDATAPTR(src, srcptr),
			   *dpos = POSDATAPTR(dest, destptr);

	if (!destptr->haspos)
		*clen = 0;

	startlen = *clen;
	for (i = 0; i < slen && *clen < MAXNUMPOS && (*clen == 0 || WEP_GETPOS(dpos[*clen - 1]) != MAXENTRYPOS - 1); i++)
	{
		WEP_SETWEIGHT(dpos[*clen], WEP_GETWEIGHT(spos[i]));
		WEP_SETPOS(dpos[*clen], LIMITPOS(WEP_GETPOS(spos[i]) + maxpos));
		(*clen)++;
	}

	if (*clen != startlen)
		destptr->haspos = 1;
	return *clen - startlen;
}


Datum
tsvector_concat(PG_FUNCTION_ARGS)
{
	TSVector	in1 = PG_GETARG_TSVECTOR(0);
	TSVector	in2 = PG_GETARG_TSVECTOR(1);
	TSVector	out;
	WordEntry  *ptr;
	WordEntry  *ptr1,
			   *ptr2;
	WordEntryPos *p;
	int			maxpos = 0,
				i,
				j,
				i1,
				i2;
	char	   *cur;
	char	   *data,
			   *data1,
			   *data2;

	ptr = ARRPTR(in1);
	i = in1->size;
	while (i--)
	{
		if ((j = POSDATALEN(in1, ptr)) != 0)
		{
			p = POSDATAPTR(in1, ptr);
			while (j--)
			{
				if (WEP_GETPOS(*p) > maxpos)
					maxpos = WEP_GETPOS(*p);
				p++;
			}
		}
		ptr++;
	}

	ptr1 = ARRPTR(in1);
	ptr2 = ARRPTR(in2);
	data1 = STRPTR(in1);
	data2 = STRPTR(in2);
	i1 = in1->size;
	i2 = in2->size;
	out = (TSVector) palloc0(VARSIZE(in1) + VARSIZE(in2));
	SET_VARSIZE(out, VARSIZE(in1) + VARSIZE(in2));
	out->size = in1->size + in2->size;
	data = cur = STRPTR(out);
	ptr = ARRPTR(out);
	while (i1 && i2)
	{
		int			cmp = compareEntry(data1, ptr1, data2, ptr2);

		if (cmp < 0)
		{						/* in1 first */
			ptr->haspos = ptr1->haspos;
			ptr->len = ptr1->len;
			memcpy(cur, data1 + ptr1->pos, ptr1->len);
			ptr->pos = cur - data;
			cur += SHORTALIGN(ptr1->len);
			if (ptr->haspos)
			{
				memcpy(cur, _POSDATAPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
				cur += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
			}
			ptr++;
			ptr1++;
			i1--;
		}
		else if (cmp > 0)
		{						/* in2 first */
			ptr->haspos = ptr2->haspos;
			ptr->len = ptr2->len;
			memcpy(cur, data2 + ptr2->pos, ptr2->len);
			ptr->pos = cur - data;
			cur += SHORTALIGN(ptr2->len);
			if (ptr->haspos)
			{
				int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

				if (addlen == 0)
					ptr->haspos = 0;
				else
					cur += addlen * sizeof(WordEntryPos) + sizeof(uint16);
			}
			ptr++;
			ptr2++;
			i2--;
		}
		else
		{
			ptr->haspos = ptr1->haspos | ptr2->haspos;
			ptr->len = ptr1->len;
			memcpy(cur, data1 + ptr1->pos, ptr1->len);
			ptr->pos = cur - data;
			cur += SHORTALIGN(ptr1->len);
			if (ptr->haspos)
			{
				if (ptr1->haspos)
				{
					memcpy(cur, _POSDATAPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
					cur += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
					if (ptr2->haspos)
						cur += add_pos(in2, ptr2, out, ptr, maxpos) * sizeof(WordEntryPos);
				}
				else if (ptr2->haspos)
				{
					int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

					if (addlen == 0)
						ptr->haspos = 0;
					else
						cur += addlen * sizeof(WordEntryPos) + sizeof(uint16);
				}
			}
			ptr++;
			ptr1++;
			ptr2++;
			i1--;
			i2--;
		}
	}

	while (i1)
	{
		ptr->haspos = ptr1->haspos;
		ptr->len = ptr1->len;
		memcpy(cur, data1 + ptr1->pos, ptr1->len);
		ptr->pos = cur - data;
		cur += SHORTALIGN(ptr1->len);
		if (ptr->haspos)
		{
			memcpy(cur, _POSDATAPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
			cur += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
		}
		ptr++;
		ptr1++;
		i1--;
	}

	while (i2)
	{
		ptr->haspos = ptr2->haspos;
		ptr->len = ptr2->len;
		memcpy(cur, data2 + ptr2->pos, ptr2->len);
		ptr->pos = cur - data;
		cur += SHORTALIGN(ptr2->len);
		if (ptr->haspos)
		{
			int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

			if (addlen == 0)
				ptr->haspos = 0;
			else
				cur += addlen * sizeof(WordEntryPos) + sizeof(uint16);
		}
		ptr++;
		ptr2++;
		i2--;
	}

	out->size = ptr - ARRPTR(out);
	SET_VARSIZE(out, CALCDATASIZE(out->size, cur - data));
	if (data != STRPTR(out))
		memmove(STRPTR(out), data, cur - data);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_POINTER(out);
}

/*
 * compare 2 string values
 */
static int4
ValCompare(CHKVAL * chkval, WordEntry * ptr, QueryItem * item)
{
	if (ptr->len == item->length)
		return strncmp(
					   &(chkval->values[ptr->pos]),
					   &(chkval->operand[item->distance]),
					   item->length);

	return (ptr->len > item->length) ? 1 : -1;
}

/*
 * check weight info
 */
static bool
checkclass_str(CHKVAL * chkval, WordEntry * val, QueryItem * item)
{
	WordEntryPos *ptr = (WordEntryPos *) (chkval->values + val->pos + SHORTALIGN(val->len) + sizeof(uint16));
	uint16		len = *((uint16 *) (chkval->values + val->pos + SHORTALIGN(val->len)));

	while (len--)
	{
		if (item->weight & (1 << WEP_GETWEIGHT(*ptr)))
			return true;
		ptr++;
	}
	return false;
}

/*
 * is there value 'val' in array or not ?
 */
static bool
checkcondition_str(void *checkval, QueryItem * val)
{
	WordEntry  *StopLow = ((CHKVAL *) checkval)->arrb;
	WordEntry  *StopHigh = ((CHKVAL *) checkval)->arre;
	WordEntry  *StopMiddle;
	int			difference;

	/* Loop invariant: StopLow <= val < StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = ValCompare((CHKVAL *) checkval, StopMiddle, val);
		if (difference == 0)
			return (val->weight && StopMiddle->haspos) ?
				checkclass_str((CHKVAL *) checkval, StopMiddle, val) : true;
		else if (difference < 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	return (false);
}

/*
 * check for boolean condition
 */
bool
TS_execute(QueryItem * curitem, void *checkval, bool calcnot, bool (*chkcond) (void *checkval, QueryItem * val))
{
	if (curitem->type == VAL)
		return chkcond(checkval, curitem);
	else if (curitem->val == (int4) '!')
	{
		return (calcnot) ?
			((TS_execute(curitem + 1, checkval, calcnot, chkcond)) ? false : true)
			: true;
	}
	else if (curitem->val == (int4) '&')
	{
		if (TS_execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return TS_execute(curitem + 1, checkval, calcnot, chkcond);
		else
			return false;
	}
	else
	{							/* |-operator */
		if (TS_execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return true;
		else
			return TS_execute(curitem + 1, checkval, calcnot, chkcond);
	}
	return false;
}

/*
 * boolean operations
 */
Datum
ts_match_qv(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(ts_match_vq,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)));
}

Datum
ts_match_vq(PG_FUNCTION_ARGS)
{
	TSVector	val = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	CHKVAL		chkval;
	bool		result;

	if (!val->size || !query->size)
	{
		PG_FREE_IF_COPY(val, 0);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(false);
	}

	chkval.arrb = ARRPTR(val);
	chkval.arre = chkval.arrb + val->size;
	chkval.values = STRPTR(val);
	chkval.operand = GETOPERAND(query);
	result = TS_execute(
						GETQUERY(query),
						&chkval,
						true,
						checkcondition_str
		);

	PG_FREE_IF_COPY(val, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

Datum
ts_match_tt(PG_FUNCTION_ARGS)
{
	TSVector	vector;
	TSQuery		query;
	bool		res;

	vector = DatumGetTSVector(DirectFunctionCall1(to_tsvector,
												  PG_GETARG_DATUM(0)));
	query = DatumGetTSQuery(DirectFunctionCall1(plainto_tsquery,
												PG_GETARG_DATUM(1)));

	res = DatumGetBool(DirectFunctionCall2(ts_match_vq,
										   TSVectorGetDatum(vector),
										   TSQueryGetDatum(query)));

	pfree(vector);
	pfree(query);

	PG_RETURN_BOOL(res);
}

Datum
ts_match_tq(PG_FUNCTION_ARGS)
{
	TSVector	vector;
	TSQuery		query = PG_GETARG_TSQUERY(1);
	bool		res;

	vector = DatumGetTSVector(DirectFunctionCall1(to_tsvector,
												  PG_GETARG_DATUM(0)));

	res = DatumGetBool(DirectFunctionCall2(ts_match_vq,
										   TSVectorGetDatum(vector),
										   TSQueryGetDatum(query)));

	pfree(vector);
	PG_FREE_IF_COPY(query, 1);

	PG_RETURN_BOOL(res);
}

/*
 * Statistics of tsvector
 */
static int
check_weight(TSVector txt, WordEntry * wptr, int8 weight)
{
	int			len = POSDATALEN(txt, wptr);
	int			num = 0;
	WordEntryPos *ptr = POSDATAPTR(txt, wptr);

	while (len--)
	{
		if (weight & (1 << WEP_GETWEIGHT(*ptr)))
			num++;
		ptr++;
	}
	return num;
}

static WordEntry **
SEI_realloc(WordEntry ** in, uint32 *len)
{
	if (*len == 0 || in == NULL)
	{
		*len = 8;
		in = palloc(sizeof(WordEntry *) * (*len));
	}
	else
	{
		*len *= 2;
		in = repalloc(in, sizeof(WordEntry *) * (*len));
	}
	return in;
}

static int
compareStatWord(StatEntry * a, WordEntry * b, tsstat * stat, TSVector txt)
{
	if (a->len == b->len)
		return strncmp(
					   STATSTRPTR(stat) + a->pos,
					   STRPTR(txt) + b->pos,
					   a->len
			);
	return (a->len > b->len) ? 1 : -1;
}

static tsstat *
formstat(tsstat * stat, TSVector txt, WordEntry ** entry, uint32 len)
{
	tsstat	   *newstat;
	uint32		totallen,
				nentry;
	uint32		slen = 0;
	WordEntry **ptr = entry;
	char	   *curptr;
	StatEntry  *sptr,
			   *nptr;

	while (ptr - entry < len)
	{
		slen += (*ptr)->len;
		ptr++;
	}

	nentry = stat->size + len;
	slen += STATSTRSIZE(stat);
	totallen = CALCSTATSIZE(nentry, slen);
	newstat = palloc(totallen);
	SET_VARSIZE(newstat, totallen);
	newstat->weight = stat->weight;
	newstat->size = nentry;

	memcpy(STATSTRPTR(newstat), STATSTRPTR(stat), STATSTRSIZE(stat));
	curptr = STATSTRPTR(newstat) + STATSTRSIZE(stat);

	ptr = entry;
	sptr = STATPTR(stat);
	nptr = STATPTR(newstat);

	if (len == 1)
	{
		StatEntry  *StopLow = STATPTR(stat);
		StatEntry  *StopHigh = (StatEntry *) STATSTRPTR(stat);

		while (StopLow < StopHigh)
		{
			sptr = StopLow + (StopHigh - StopLow) / 2;
			if (compareStatWord(sptr, *ptr, stat, txt) < 0)
				StopLow = sptr + 1;
			else
				StopHigh = sptr;
		}
		nptr = STATPTR(newstat) + (StopLow - STATPTR(stat));
		memcpy(STATPTR(newstat), STATPTR(stat), sizeof(StatEntry) * (StopLow - STATPTR(stat)));
		if ((*ptr)->haspos)
			nptr->nentry = (stat->weight) ? check_weight(txt, *ptr, stat->weight) : POSDATALEN(txt, *ptr);
		else
			nptr->nentry = 1;
		nptr->ndoc = 1;
		nptr->len = (*ptr)->len;
		memcpy(curptr, STRPTR(txt) + (*ptr)->pos, nptr->len);
		nptr->pos = curptr - STATSTRPTR(newstat);
		memcpy(nptr + 1, StopLow, sizeof(StatEntry) * (((StatEntry *) STATSTRPTR(stat)) - StopLow));
	}
	else
	{
		while (sptr - STATPTR(stat) < stat->size && ptr - entry < len)
		{
			if (compareStatWord(sptr, *ptr, stat, txt) < 0)
			{
				memcpy(nptr, sptr, sizeof(StatEntry));
				sptr++;
			}
			else
			{
				if ((*ptr)->haspos)
					nptr->nentry = (stat->weight) ? check_weight(txt, *ptr, stat->weight) : POSDATALEN(txt, *ptr);
				else
					nptr->nentry = 1;
				nptr->ndoc = 1;
				nptr->len = (*ptr)->len;
				memcpy(curptr, STRPTR(txt) + (*ptr)->pos, nptr->len);
				nptr->pos = curptr - STATSTRPTR(newstat);
				curptr += nptr->len;
				ptr++;
			}
			nptr++;
		}

		memcpy(nptr, sptr, sizeof(StatEntry) * (stat->size - (sptr - STATPTR(stat))));

		while (ptr - entry < len)
		{
			if ((*ptr)->haspos)
				nptr->nentry = (stat->weight) ? check_weight(txt, *ptr, stat->weight) : POSDATALEN(txt, *ptr);
			else
				nptr->nentry = 1;
			nptr->ndoc = 1;
			nptr->len = (*ptr)->len;
			memcpy(curptr, STRPTR(txt) + (*ptr)->pos, nptr->len);
			nptr->pos = curptr - STATSTRPTR(newstat);
			curptr += nptr->len;
			ptr++;
			nptr++;
		}
	}

	return newstat;
}

static tsstat *
ts_accum(tsstat * stat, Datum data)
{
	tsstat	   *newstat;
	TSVector	txt = DatumGetTSVector(data);
	WordEntry **newentry = NULL;
	uint32		len = 0,
				cur = 0;
	StatEntry  *sptr;
	WordEntry  *wptr;
	int			n = 0;

	if (stat == NULL)
	{							/* Init in first */
		stat = palloc(STATHDRSIZE);
		SET_VARSIZE(stat, STATHDRSIZE);
		stat->size = 0;
		stat->weight = 0;
	}

	/* simple check of correctness */
	if (txt == NULL || txt->size == 0)
	{
		if (txt != (TSVector) DatumGetPointer(data))
			pfree(txt);
		return stat;
	}

	sptr = STATPTR(stat);
	wptr = ARRPTR(txt);

	if (stat->size < 100 * txt->size)
	{							/* merge */
		while (sptr - STATPTR(stat) < stat->size && wptr - ARRPTR(txt) < txt->size)
		{
			int			cmp = compareStatWord(sptr, wptr, stat, txt);

			if (cmp < 0)
				sptr++;
			else if (cmp == 0)
			{
				if (stat->weight == 0)
				{
					sptr->ndoc++;
					sptr->nentry += (wptr->haspos) ? POSDATALEN(txt, wptr) : 1;
				}
				else if (wptr->haspos && (n = check_weight(txt, wptr, stat->weight)) != 0)
				{
					sptr->ndoc++;
					sptr->nentry += n;
				}
				sptr++;
				wptr++;
			}
			else
			{
				if (stat->weight == 0 || check_weight(txt, wptr, stat->weight) != 0)
				{
					if (cur == len)
						newentry = SEI_realloc(newentry, &len);
					newentry[cur] = wptr;
					cur++;
				}
				wptr++;
			}
		}

		while (wptr - ARRPTR(txt) < txt->size)
		{
			if (stat->weight == 0 || check_weight(txt, wptr, stat->weight) != 0)
			{
				if (cur == len)
					newentry = SEI_realloc(newentry, &len);
				newentry[cur] = wptr;
				cur++;
			}
			wptr++;
		}
	}
	else
	{							/* search */
		while (wptr - ARRPTR(txt) < txt->size)
		{
			StatEntry  *StopLow = STATPTR(stat);
			StatEntry  *StopHigh = (StatEntry *) STATSTRPTR(stat);
			int			cmp;

			while (StopLow < StopHigh)
			{
				sptr = StopLow + (StopHigh - StopLow) / 2;
				cmp = compareStatWord(sptr, wptr, stat, txt);
				if (cmp == 0)
				{
					if (stat->weight == 0)
					{
						sptr->ndoc++;
						sptr->nentry += (wptr->haspos) ? POSDATALEN(txt, wptr) : 1;
					}
					else if (wptr->haspos && (n = check_weight(txt, wptr, stat->weight)) != 0)
					{
						sptr->ndoc++;
						sptr->nentry += n;
					}
					break;
				}
				else if (cmp < 0)
					StopLow = sptr + 1;
				else
					StopHigh = sptr;
			}

			if (StopLow >= StopHigh)
			{					/* not found */
				if (stat->weight == 0 || check_weight(txt, wptr, stat->weight) != 0)
				{
					if (cur == len)
						newentry = SEI_realloc(newentry, &len);
					newentry[cur] = wptr;
					cur++;
				}
			}
			wptr++;
		}
	}


	if (cur == 0)
	{							/* no new words */
		if (txt != (TSVector) DatumGetPointer(data))
			pfree(txt);
		return stat;
	}

	newstat = formstat(stat, txt, newentry, cur);
	pfree(newentry);

	if (txt != (TSVector) DatumGetPointer(data))
		pfree(txt);
	return newstat;
}

static void
ts_setup_firstcall(FunctionCallInfo fcinfo, FuncCallContext *funcctx,
				   tsstat * stat)
{
	TupleDesc	tupdesc;
	MemoryContext oldcontext;
	StatStorage *st;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	st = palloc(sizeof(StatStorage));
	st->cur = 0;
	st->stat = palloc(VARSIZE(stat));
	memcpy(st->stat, stat, VARSIZE(stat));
	funcctx->user_fctx = (void *) st;

	tupdesc = CreateTemplateTupleDesc(3, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "word",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "ndoc",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "nentry",
					   INT4OID, -1, 0);
	funcctx->tuple_desc = BlessTupleDesc(tupdesc);
	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	MemoryContextSwitchTo(oldcontext);
}


static Datum
ts_process_call(FuncCallContext *funcctx)
{
	StatStorage *st;

	st = (StatStorage *) funcctx->user_fctx;

	if (st->cur < st->stat->size)
	{
		Datum		result;
		char	   *values[3];
		char		ndoc[16];
		char		nentry[16];
		StatEntry  *entry = STATPTR(st->stat) + st->cur;
		HeapTuple	tuple;

		values[0] = palloc(entry->len + 1);
		memcpy(values[0], STATSTRPTR(st->stat) + entry->pos, entry->len);
		(values[0])[entry->len] = '\0';
		sprintf(ndoc, "%d", entry->ndoc);
		values[1] = ndoc;
		sprintf(nentry, "%d", entry->nentry);
		values[2] = nentry;

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);

		pfree(values[0]);
		st->cur++;
		return result;
	}
	else
	{
		pfree(st->stat);
		pfree(st);
	}

	return (Datum) 0;
}

static tsstat *
ts_stat_sql(text *txt, text *ws)
{
	char	   *query = TextPGetCString(txt);
	int			i;
	tsstat	   *newstat,
			   *stat;
	bool		isnull;
	Portal		portal;
	void	   *plan;

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		/* internal error */
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, false)) == NULL)
		/* internal error */
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable->tupdesc->natts != 1 ||
		SPI_gettypeid(SPI_tuptable->tupdesc, 1) != TSVECTOROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ts_stat query must return one tsvector column")));

	stat = palloc(STATHDRSIZE);
	SET_VARSIZE(stat, STATHDRSIZE);
	stat->size = 0;
	stat->weight = 0;

	if (ws)
	{
		char	   *buf;

		buf = VARDATA(ws);
		while (buf - VARDATA(ws) < VARSIZE(ws) - VARHDRSZ)
		{
			if (pg_mblen(buf) == 1)
			{
				switch (*buf)
				{
					case 'A':
					case 'a':
						stat->weight |= 1 << 3;
						break;
					case 'B':
					case 'b':
						stat->weight |= 1 << 2;
						break;
					case 'C':
					case 'c':
						stat->weight |= 1 << 1;
						break;
					case 'D':
					case 'd':
						stat->weight |= 1;
						break;
					default:
						stat->weight |= 0;
				}
			}
			buf += pg_mblen(buf);
		}
	}

	while (SPI_processed > 0)
	{
		for (i = 0; i < SPI_processed; i++)
		{
			Datum		data = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);

			if (!isnull)
			{
				newstat = ts_accum(stat, data);
				if (stat != newstat && stat)
					pfree(stat);
				stat = newstat;
			}
		}

		SPI_freetuptable(SPI_tuptable);
		SPI_cursor_fetch(portal, true, 100);
	}

	SPI_freetuptable(SPI_tuptable);
	SPI_cursor_close(portal);
	SPI_freeplan(plan);
	pfree(query);

	return stat;
}

Datum
ts_stat1(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		tsstat	   *stat;
		text	   *txt = PG_GETARG_TEXT_P(0);

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(txt, NULL);
		PG_FREE_IF_COPY(txt, 0);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}

Datum
ts_stat2(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		tsstat	   *stat;
		text	   *txt = PG_GETARG_TEXT_P(0);
		text	   *ws = PG_GETARG_TEXT_P(1);

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(txt, ws);
		PG_FREE_IF_COPY(txt, 0);
		PG_FREE_IF_COPY(ws, 1);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}


/* Check if datatype is TEXT or binary-equivalent to it */
static bool
istexttype(Oid typid)
{
	/* varchar(n) and char(n) are binary-compatible with text */
	if (typid==TEXTOID || typid==VARCHAROID || typid==BPCHAROID)
		return true;
	/* Allow domains over these types, too */
	typid = getBaseType(typid);
	if (typid==TEXTOID || typid==VARCHAROID || typid==BPCHAROID)
		return true;
	return false;
}


/*
 * Triggers for automatic update of a tsvector column from text column(s)
 *
 * Trigger arguments are either
 *		name of tsvector col, name of tsconfig to use, name(s) of text col(s)
 *		name of tsvector col, name of regconfig col, name(s) of text col(s)
 * ie, tsconfig can either be specified by name, or indirectly as the
 * contents of a regconfig field in the row.  If the name is used, it must
 * be explicitly schema-qualified.
 */
Datum
tsvector_update_trigger_byid(PG_FUNCTION_ARGS)
{
	return tsvector_update_trigger(fcinfo, false);
}

Datum
tsvector_update_trigger_bycolumn(PG_FUNCTION_ARGS)
{
	return tsvector_update_trigger(fcinfo, true);
}

static Datum
tsvector_update_trigger(PG_FUNCTION_ARGS, bool config_column)
{
	TriggerData *trigdata;
	Trigger    *trigger;
	Relation	rel;
	HeapTuple	rettuple = NULL;
	int			tsvector_attr_num,
				i;
	ParsedText	prs;
	Datum		datum;
	bool		isnull;
	text	   *txt;
	Oid			cfgId;

	/* Check call context */
	if (!CALLED_AS_TRIGGER(fcinfo))	/* internal error */
		elog(ERROR, "tsvector_update_trigger: not fired by trigger manager");

	trigdata = (TriggerData *) fcinfo->context;
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "tsvector_update_trigger: can't process STATEMENT events");
	if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		elog(ERROR, "tsvector_update_trigger: must be fired BEFORE event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		rettuple = trigdata->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		elog(ERROR, "tsvector_update_trigger: must be fired for INSERT or UPDATE");

	trigger = trigdata->tg_trigger;
	rel = trigdata->tg_relation;

	if (trigger->tgnargs < 3)
		elog(ERROR, "tsvector_update_trigger: arguments must be tsvector_field, ts_config, text_field1, ...)");

	/* Find the target tsvector column */
	tsvector_attr_num = SPI_fnumber(rel->rd_att, trigger->tgargs[0]);
	if (tsvector_attr_num == SPI_ERROR_NOATTRIBUTE)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("tsvector column \"%s\" does not exist",
						trigger->tgargs[0])));
	if (SPI_gettypeid(rel->rd_att, tsvector_attr_num) != TSVECTOROID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" is not of tsvector type",
						trigger->tgargs[0])));

	/* Find the configuration to use */
	if (config_column)
	{
		int		config_attr_num;

		config_attr_num = SPI_fnumber(rel->rd_att, trigger->tgargs[1]);
		if (config_attr_num == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("config column \"%s\" does not exist",
							trigger->tgargs[1])));
		if (SPI_gettypeid(rel->rd_att, config_attr_num) != REGCONFIGOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is not of regconfig type",
							trigger->tgargs[1])));

		datum = SPI_getbinval(rettuple, rel->rd_att, config_attr_num, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("config column \"%s\" must not be NULL",
							trigger->tgargs[1])));
		cfgId = DatumGetObjectId(datum);
	}
	else
	{
		List	*names;

		names = stringToQualifiedNameList(trigger->tgargs[1]);
		/* require a schema so that results are not search path dependent */
		if (list_length(names) < 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text search configuration name \"%s\" must be schema-qualified",
							trigger->tgargs[1])));
		cfgId = TSConfigGetCfgid(names, false);
	}

	/* initialize parse state */
	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	/* find all words in indexable column(s) */
	for (i = 2; i < trigger->tgnargs; i++)
	{
		int			numattr;

		numattr = SPI_fnumber(rel->rd_att, trigger->tgargs[i]);
		if (numattr == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							trigger->tgargs[i])));
		if (!istexttype(SPI_gettypeid(rel->rd_att, numattr)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is not of character type",
							trigger->tgargs[i])));

		datum = SPI_getbinval(rettuple, rel->rd_att, numattr, &isnull);
		if (isnull)
			continue;

		txt = DatumGetTextP(datum);

		parsetext(cfgId, &prs, VARDATA(txt), VARSIZE(txt) - VARHDRSZ);

		if (txt != (text *) DatumGetPointer(datum))
			pfree(txt);
	}

	/* make tsvector value */
	if (prs.curwords)
	{
		datum = PointerGetDatum(make_tsvector(&prs));
		rettuple = SPI_modifytuple(rel, rettuple, 1, &tsvector_attr_num,
								   &datum, NULL);
		pfree(DatumGetPointer(datum));
	}
	else
	{
		TSVector	out = palloc(CALCDATASIZE(0, 0));

		SET_VARSIZE(out, CALCDATASIZE(0, 0));
		out->size = 0;
		datum = PointerGetDatum(out);
		rettuple = SPI_modifytuple(rel, rettuple, 1, &tsvector_attr_num,
								   &datum, NULL);
		pfree(prs.words);
	}

	if (rettuple == NULL)		/* internal error */
		elog(ERROR, "tsvector_update_trigger: %d returned by SPI_modifytuple",
			 SPI_result);

	return PointerGetDatum(rettuple);
}
