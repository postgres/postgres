/*-------------------------------------------------------------------------
 *
 * tsvector.c
 *	  I/O functions for tsvector
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsvector.c,v 1.3 2007/09/07 15:09:56 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/memutils.h"

typedef struct
{
	WordEntry	entry;			/* should be first ! */
	WordEntryPos *pos;
	int			poslen;			/* number of elements in pos */
} WordEntryIN;

static int
comparePos(const void *a, const void *b)
{
	int apos = WEP_GETPOS(*(WordEntryPos *) a);
	int bpos = WEP_GETPOS(*(WordEntryPos *) b);

	if (apos == bpos)
		return 0;
	return (apos > bpos) ? 1 : -1;
}

/*
 * Removes duplicate pos entries. If there's two entries with same pos
 * but different weight, the higher weight is retained.
 *
 * Returns new length.
 */
static int
uniquePos(WordEntryPos * a, int l)
{
	WordEntryPos *ptr,
			   *res;

	if (l <= 1)
		return l;

	res = a;
	qsort((void *) a, l, sizeof(WordEntryPos), comparePos);

	ptr = a + 1;
	while (ptr - a < l)
	{
		if (WEP_GETPOS(*ptr) != WEP_GETPOS(*res))
		{
			res++;
			*res = *ptr;
			if (res - a >= MAXNUMPOS - 1 || WEP_GETPOS(*res) == MAXENTRYPOS - 1)
				break;
		}
		else if (WEP_GETWEIGHT(*ptr) > WEP_GETWEIGHT(*res))
			WEP_SETWEIGHT(*res, WEP_GETWEIGHT(*ptr));
		ptr++;
	}

	return res + 1 - a;
}

static int
compareentry(const void *a, const void *b, void *arg)
{
	char	   *BufferStr = (char *) arg;

	if (((WordEntryIN *) a)->entry.len == ((WordEntryIN *) b)->entry.len)
	{
		return strncmp(&BufferStr[((WordEntryIN *) a)->entry.pos],
					   &BufferStr[((WordEntryIN *) b)->entry.pos],
					   ((WordEntryIN *) a)->entry.len);
	}

	return (((WordEntryIN *) a)->entry.len > ((WordEntryIN *) b)->entry.len) ? 1 : -1;
}

static int
uniqueentry(WordEntryIN * a, int l, char *buf, int *outbuflen)
{
	WordEntryIN *ptr,
			   *res;

	Assert(l >= 1);

	if (l == 1)
	{
		if (a->entry.haspos)
		{
			a->poslen = uniquePos(a->pos, a->poslen);
			*outbuflen = SHORTALIGN(a->entry.len) + (a->poslen + 1) * sizeof(WordEntryPos);
		}
		return l;
	}
	res = a;

	ptr = a + 1;
	qsort_arg((void *) a, l, sizeof(WordEntryIN), compareentry, (void *) buf);

	while (ptr - a < l)
	{
		if (!(ptr->entry.len == res->entry.len &&
			  strncmp(&buf[ptr->entry.pos], &buf[res->entry.pos], res->entry.len) == 0))
		{
			if (res->entry.haspos)
			{
				res->poslen = uniquePos(res->pos, res->poslen);
				*outbuflen += res->poslen * sizeof(WordEntryPos);
			}
			*outbuflen += SHORTALIGN(res->entry.len);
			res++;
			memcpy(res, ptr, sizeof(WordEntryIN));
		}
		else if (ptr->entry.haspos)
		{
			if (res->entry.haspos)
			{
				int	newlen = ptr->poslen + res->poslen;

				/* Append res to pos */

				res->pos = (WordEntryPos *) repalloc(res->pos, newlen * sizeof(WordEntryPos));
				memcpy(&res->pos[res->poslen],
					   ptr->pos, ptr->poslen * sizeof(WordEntryPos));
				res->poslen = newlen;
				pfree(ptr->pos);
			}
			else
			{
				res->entry.haspos = 1;
				res->pos = ptr->pos;
			}
		}
		ptr++;
	}
	if (res->entry.haspos)
	{
		res->poslen = uniquePos(res->pos, res->poslen);
		*outbuflen += res->poslen * sizeof(WordEntryPos);
	}
	*outbuflen += SHORTALIGN(res->entry.len);

	return res + 1 - a;
}

static int
WordEntryCMP(WordEntry * a, WordEntry * b, char *buf)
{
	return compareentry(a, b, buf);
}


Datum
tsvectorin(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	TSVectorParseState state;
	WordEntryIN *arr;
	int			totallen;
	int			arrlen;  /* allocated size of arr */
	WordEntry  *inarr;
	int			len = 0;
	TSVector	in;
	int			i;
	char	   *token;
	int			toklen;
	WordEntryPos *pos;
	int			poslen;

	/*
	 * Tokens are appended to tmpbuf, cur is a pointer
	 * to the end of used space in tmpbuf.
	 */
	char	   *tmpbuf;
	char	   *cur;
	int			buflen = 256; /* allocated size of tmpbuf */

	pg_verifymbstr(buf, strlen(buf), false);

	state = init_tsvector_parser(buf, false);
	
	arrlen = 64;
	arr = (WordEntryIN *) palloc(sizeof(WordEntryIN) * arrlen);
	cur = tmpbuf = (char *) palloc(buflen);

	while (gettoken_tsvector(state, &token, &toklen, &pos, &poslen, NULL))
	{

		if (toklen >= MAXSTRLEN)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("word is too long (%ld bytes, max %ld bytes)",
							(long) toklen,
							(long) MAXSTRLEN)));


		if (cur - tmpbuf > MAXSTRPOS)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("position value too large")));

		/*
		 * Enlarge buffers if needed
		 */
		if (len >= arrlen)
		{
			arrlen *= 2;
			arr = (WordEntryIN *) repalloc((void *) arr, sizeof(WordEntryIN) * arrlen);
		}
		while ((cur - tmpbuf) + toklen >= buflen)
		{
			int	dist = cur - tmpbuf;

			buflen *= 2;
			tmpbuf = (char *) repalloc((void *) tmpbuf, buflen);
			cur = tmpbuf + dist;
		}
		arr[len].entry.len = toklen;
		arr[len].entry.pos = cur - tmpbuf;
		memcpy((void *) cur, (void *) token, toklen);
		cur += toklen;

		if (poslen != 0)
		{
			arr[len].entry.haspos = 1;
			arr[len].pos = pos;
			arr[len].poslen = poslen;
		}
		else
			arr[len].entry.haspos = 0;
		len++;
	}

	close_tsvector_parser(state);

	if (len > 0)
		len = uniqueentry(arr, len, tmpbuf, &buflen);
	else
		buflen = 0;
	totallen = CALCDATASIZE(len, buflen);
	in = (TSVector) palloc0(totallen);

	SET_VARSIZE(in, totallen);
	in->size = len;
	cur = STRPTR(in);
	inarr = ARRPTR(in);
	for (i = 0; i < len; i++)
	{
		memcpy((void *) cur, (void *) &tmpbuf[arr[i].entry.pos], arr[i].entry.len);
		arr[i].entry.pos = cur - STRPTR(in);
		cur += SHORTALIGN(arr[i].entry.len);
		if (arr[i].entry.haspos)
		{
			uint16 tmplen;

			if(arr[i].poslen > 0xFFFF)
				elog(ERROR, "positions array too long");

			tmplen = (uint16) arr[i].poslen;

			/* Copy length to output struct */
			memcpy(cur, &tmplen, sizeof(uint16));
			cur += sizeof(uint16);

			/* Copy positions */
			memcpy(cur, arr[i].pos, (arr[i].poslen) * sizeof(WordEntryPos));
			cur += arr[i].poslen * sizeof(WordEntryPos);

			pfree(arr[i].pos);
		}
		inarr[i] = arr[i].entry;
	}

	PG_RETURN_TSVECTOR(in);
}

Datum
tsvectorout(PG_FUNCTION_ARGS)
{
	TSVector	out = PG_GETARG_TSVECTOR(0);
	char	   *outbuf;
	int4		i,
				lenbuf = 0,
				pp;
	WordEntry  *ptr = ARRPTR(out);
	char	   *curbegin,
			   *curin,
			   *curout;

	lenbuf = out->size * 2 /* '' */ + out->size - 1 /* space */ + 2 /* \0 */ ;
	for (i = 0; i < out->size; i++)
	{
		lenbuf += ptr[i].len * 2 * pg_database_encoding_max_length() /* for escape */ ;
		if (ptr[i].haspos)
			lenbuf += 1 /* : */ + 7 /* int2 + , + weight */ * POSDATALEN(out, &(ptr[i]));
	}

	curout = outbuf = (char *) palloc(lenbuf);
	for (i = 0; i < out->size; i++)
	{
		curbegin = curin = STRPTR(out) + ptr->pos;
		if (i != 0)
			*curout++ = ' ';
		*curout++ = '\'';
		while (curin - curbegin < ptr->len)
		{
			int			len = pg_mblen(curin);

			if (t_iseq(curin, '\''))
				*curout++ = '\'';

			while (len--)
				*curout++ = *curin++;
		}

		*curout++ = '\'';
		if ((pp = POSDATALEN(out, ptr)) != 0)
		{
			WordEntryPos *wptr;

			*curout++ = ':';
			wptr = POSDATAPTR(out, ptr);
			while (pp)
			{
				curout += sprintf(curout, "%d", WEP_GETPOS(*wptr));
				switch (WEP_GETWEIGHT(*wptr))
				{
					case 3:
						*curout++ = 'A';
						break;
					case 2:
						*curout++ = 'B';
						break;
					case 1:
						*curout++ = 'C';
						break;
					case 0:
					default:
						break;
				}

				if (pp > 1)
					*curout++ = ',';
				pp--;
				wptr++;
			}
		}
		ptr++;
	}

	*curout = '\0';
	PG_FREE_IF_COPY(out, 0);
	PG_RETURN_CSTRING(outbuf);
}

Datum
tsvectorsend(PG_FUNCTION_ARGS)
{
	TSVector	vec = PG_GETARG_TSVECTOR(0);
	StringInfoData buf;
	int			i,
				j;
	WordEntry  *weptr = ARRPTR(vec);

	pq_begintypsend(&buf);

	pq_sendint(&buf, vec->size, sizeof(int32));
	for (i = 0; i < vec->size; i++)
	{
		/*
		 * We are sure that sizeof(WordEntry) == sizeof(int32)
		 */
		pq_sendint(&buf, *(int32 *) weptr, sizeof(int32));

		pq_sendbytes(&buf, STRPTR(vec) + weptr->pos, weptr->len);
		if (weptr->haspos)
		{
			WordEntryPos *wepptr = POSDATAPTR(vec, weptr);

			pq_sendint(&buf, POSDATALEN(vec, weptr), sizeof(WordEntryPos));
			for (j = 0; j < POSDATALEN(vec, weptr); j++)
				pq_sendint(&buf, wepptr[j], sizeof(WordEntryPos));
		}
		weptr++;
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tsvectorrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TSVector	vec;
	int			i;
	uint32		size;
	WordEntry  *weptr;
	int			datalen = 0;
	Size		len;

	size = pq_getmsgint(buf, sizeof(uint32));
	if (size < 0 || size > (MaxAllocSize / sizeof(WordEntry)))
		elog(ERROR, "invalid size of tsvector");

	len = DATAHDRSIZE + sizeof(WordEntry) * size;

	len = len * 2; /* times two to make room for lexemes */
	vec = (TSVector) palloc0(len);
	vec->size = size;

	weptr = ARRPTR(vec);
	for (i = 0; i < size; i++)
	{
		int32 tmp;

		weptr = ARRPTR(vec) + i;

		/*
		 * We are sure that sizeof(WordEntry) == sizeof(int32)
		 */
		tmp = pq_getmsgint(buf, sizeof(int32));
		*weptr = *(WordEntry *) & tmp;

		while (CALCDATASIZE(size, datalen + SHORTALIGN(weptr->len)) >= len)
		{
			len *= 2;
			vec = (TSVector) repalloc(vec, len);
			weptr = ARRPTR(vec) + i;
		}

		memcpy(STRPTR(vec) + weptr->pos,
			   pq_getmsgbytes(buf, weptr->len),
			   weptr->len);
		datalen += SHORTALIGN(weptr->len);

		if (i > 0 && WordEntryCMP(weptr, weptr - 1, STRPTR(vec)) <= 0)
			elog(ERROR, "lexemes are unordered");

		if (weptr->haspos)
		{
			uint16		j,
						npos;
			WordEntryPos *wepptr;

			npos = (uint16) pq_getmsgint(buf, sizeof(uint16));
			if (npos > MAXNUMPOS)
				elog(ERROR, "unexpected number of positions");

			while (CALCDATASIZE(size, datalen + (npos + 1) * sizeof(WordEntryPos)) >= len)
			{
				len *= 2;
				vec = (TSVector) repalloc(vec, len);
				weptr = ARRPTR(vec) + i;
			}

			memcpy(_POSDATAPTR(vec, weptr), &npos, sizeof(int16));
			wepptr = POSDATAPTR(vec, weptr);
			for (j = 0; j < npos; j++)
			{
				wepptr[j] = (WordEntryPos) pq_getmsgint(buf, sizeof(int16));
				if (j > 0 && WEP_GETPOS(wepptr[j]) <= WEP_GETPOS(wepptr[j - 1]))
					elog(ERROR, "position information is unordered");
			}

			datalen += (npos + 1) * sizeof(WordEntry);
		}
	}

	SET_VARSIZE(vec, CALCDATASIZE(vec->size, datalen));

	PG_RETURN_TSVECTOR(vec);
}
