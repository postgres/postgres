/*
 * Operations for tsvector type
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"


#include "access/gist.h"
#include "access/itup.h"
#include "catalog/namespace.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "nodes/pg_list.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"

#include "tsvector.h"
#include "query.h"
#include "ts_cfg.h"
#include "common.h"

PG_FUNCTION_INFO_V1(strip);
Datum		strip(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(setweight);
Datum		setweight(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(concat);
Datum		concat(PG_FUNCTION_ARGS);

Datum
strip(PG_FUNCTION_ARGS)
{
	tsvector   *in = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	tsvector   *out;
	int			i,
				len = 0;
	WordEntry  *arrin = ARRPTR(in),
			   *arrout;
	char	   *cur;

	for (i = 0; i < in->size; i++)
		len += SHORTALIGN(arrin[i].len);

	len = CALCDATASIZE(in->size, len);
	out = (tsvector *) palloc(len);
	memset(out, 0, len);
	out->len = len;
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
setweight(PG_FUNCTION_ARGS)
{
	tsvector   *in = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	char		cw = PG_GETARG_CHAR(1);
	tsvector   *out;
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

	out = (tsvector *) palloc(in->len);
	memcpy(out, in, in->len);
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
add_pos(tsvector * src, WordEntry * srcptr, tsvector * dest, WordEntry * destptr, int4 maxpos)
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
concat(PG_FUNCTION_ARGS)
{
	tsvector   *in1 = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	tsvector   *in2 = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	tsvector   *out;
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
	out = (tsvector *) palloc(in1->len + in2->len);
	memset(out, 0, in1->len + in2->len);
	out->len = in1->len + in2->len;
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
	out->len = CALCDATASIZE(out->size, cur - data);
	if (data != STRPTR(out))
		memmove(STRPTR(out), data, cur - data);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_POINTER(out);
}
