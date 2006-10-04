/*
 * stat functions
 */

#include "tsvector.h"
#include "ts_stat.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "common.h"
#include "ts_locale.h"

PG_FUNCTION_INFO_V1(tsstat_in);
Datum		tsstat_in(PG_FUNCTION_ARGS);
Datum
tsstat_in(PG_FUNCTION_ARGS)
{
	tsstat	   *stat = palloc(STATHDRSIZE);

	stat->len = STATHDRSIZE;
	stat->size = 0;
	stat->weight = 0;
	PG_RETURN_POINTER(stat);
}

PG_FUNCTION_INFO_V1(tsstat_out);
Datum		tsstat_out(PG_FUNCTION_ARGS);
Datum
tsstat_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tsstat_out not implemented")));
	PG_RETURN_NULL();
}

static int
check_weight(tsvector * txt, WordEntry * wptr, int8 weight)
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
compareStatWord(StatEntry * a, WordEntry * b, tsstat * stat, tsvector * txt)
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
formstat(tsstat * stat, tsvector * txt, WordEntry ** entry, uint32 len)
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
	newstat->len = totallen;
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

PG_FUNCTION_INFO_V1(ts_accum);
Datum		ts_accum(PG_FUNCTION_ARGS);
Datum
ts_accum(PG_FUNCTION_ARGS)
{
	tsstat	   *newstat,
			   *stat = (tsstat *) PG_GETARG_POINTER(0);
	tsvector   *txt = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	WordEntry **newentry = NULL;
	uint32		len = 0,
				cur = 0;
	StatEntry  *sptr;
	WordEntry  *wptr;
	int			n = 0;

	if (stat == NULL || PG_ARGISNULL(0))
	{							/* Init in first */
		stat = palloc(STATHDRSIZE);
		stat->len = STATHDRSIZE;
		stat->size = 0;
		stat->weight = 0;
	}

	/* simple check of correctness */
	if (txt == NULL || PG_ARGISNULL(1) || txt->size == 0)
	{
		PG_FREE_IF_COPY(txt, 1);
		PG_RETURN_POINTER(stat);
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
		PG_FREE_IF_COPY(txt, 1);
		PG_RETURN_POINTER(stat);
	}

	newstat = formstat(stat, txt, newentry, cur);
	pfree(newentry);
	PG_FREE_IF_COPY(txt, 1);
	/* pfree(stat); */

	PG_RETURN_POINTER(newstat);
}

typedef struct
{
	uint32		cur;
	tsvector   *stat;
}	StatStorage;

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
	st->stat = palloc(stat->len);
	memcpy(st->stat, stat, stat->len);
	funcctx->user_fctx = (void *) st;
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = CreateTupleDescCopy(tupdesc);
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

		values[1] = ndoc;
		sprintf(ndoc, "%d", entry->ndoc);
		values[2] = nentry;
		sprintf(nentry, "%d", entry->nentry);
		values[0] = palloc(entry->len + 1);
		memcpy(values[0], STATSTRPTR(st->stat) + entry->pos, entry->len);
		(values[0])[entry->len] = '\0';

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

PG_FUNCTION_INFO_V1(ts_accum_finish);
Datum		ts_accum_finish(PG_FUNCTION_ARGS);
Datum
ts_accum_finish(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		funcctx = SRF_FIRSTCALL_INIT();
		ts_setup_firstcall(fcinfo, funcctx, (tsstat *) PG_GETARG_POINTER(0));
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}

static Oid	tiOid = InvalidOid;

static void
get_ti_Oid(void)
{
	int			ret;
	bool		isnull;

	if ((ret = SPI_exec("select oid from pg_type where typname='tsvector'", 1)) < 0)
		/* internal error */
		elog(ERROR, "SPI_exec to get tsvector oid returns %d", ret);

	if (SPI_processed < 1)
		/* internal error */
		elog(ERROR, "there is no tsvector type");
	tiOid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	if (tiOid == InvalidOid)
		/* internal error */
		elog(ERROR, "tsvector type has InvalidOid");
}

static tsstat *
ts_stat_sql(text *txt, text *ws)
{
	char	   *query = text2char(txt);
	int			i;
	tsstat	   *newstat,
			   *stat;
	bool		isnull;
	Portal		portal;
	void	   *plan;

	if (tiOid == InvalidOid)
		get_ti_Oid();

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		/* internal error */
		elog(ERROR, "SPI_prepare('%s') returns NULL", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, false)) == NULL)
		/* internal error */
		elog(ERROR, "SPI_cursor_open('%s') returns NULL", query);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable->tupdesc->natts != 1)
		/* internal error */
		elog(ERROR, "number of fields doesn't equal to 1");

	if (SPI_gettypeid(SPI_tuptable->tupdesc, 1) != tiOid)
		/* internal error */
		elog(ERROR, "column isn't of tsvector type");

	stat = palloc(STATHDRSIZE);
	stat->len = STATHDRSIZE;
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
				newstat = (tsstat *) DatumGetPointer(DirectFunctionCall2(
																	ts_accum,
													   PointerGetDatum(stat),
																		 data
																		 ));
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

PG_FUNCTION_INFO_V1(ts_stat);
Datum		ts_stat(PG_FUNCTION_ARGS);
Datum
ts_stat(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		tsstat	   *stat;
		text	   *txt = PG_GETARG_TEXT_P(0);
		text	   *ws = (PG_NARGS() > 1) ? PG_GETARG_TEXT_P(1) : NULL;

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(txt, ws);
		PG_FREE_IF_COPY(txt, 0);
		if (PG_NARGS() > 1)
			PG_FREE_IF_COPY(ws, 1);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}
