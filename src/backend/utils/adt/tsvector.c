/*-------------------------------------------------------------------------
 *
 * tsvector.c
 *	  I/O functions for tsvector
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsvector.c,v 1.2 2007/08/21 01:45:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/memutils.h"


static int
comparePos(const void *a, const void *b)
{
	if (WEP_GETPOS(*(WordEntryPos *) a) == WEP_GETPOS(*(WordEntryPos *) b))
		return 0;
	return (WEP_GETPOS(*(WordEntryPos *) a) > WEP_GETPOS(*(WordEntryPos *) b)) ? 1 : -1;
}

static int
uniquePos(WordEntryPos * a, int4 l)
{
	WordEntryPos *ptr,
			   *res;

	if (l == 1)
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
uniqueentry(WordEntryIN * a, int4 l, char *buf, int4 *outbuflen)
{
	WordEntryIN *ptr,
			   *res;

	res = a;
	if (l == 1)
	{
		if (a->entry.haspos)
		{
			*(uint16 *) (a->pos) = uniquePos(&(a->pos[1]), *(uint16 *) (a->pos));
			*outbuflen = SHORTALIGN(res->entry.len) + (*(uint16 *) (a->pos) + 1) * sizeof(WordEntryPos);
		}
		return l;
	}

	ptr = a + 1;
	qsort_arg((void *) a, l, sizeof(WordEntryIN), compareentry, (void *) buf);

	while (ptr - a < l)
	{
		if (!(ptr->entry.len == res->entry.len &&
			  strncmp(&buf[ptr->entry.pos], &buf[res->entry.pos], res->entry.len) == 0))
		{
			if (res->entry.haspos)
			{
				*(uint16 *) (res->pos) = uniquePos(&(res->pos[1]), *(uint16 *) (res->pos));
				*outbuflen += *(uint16 *) (res->pos) * sizeof(WordEntryPos);
			}
			*outbuflen += SHORTALIGN(res->entry.len);
			res++;
			memcpy(res, ptr, sizeof(WordEntryIN));
		}
		else if (ptr->entry.haspos)
		{
			if (res->entry.haspos)
			{
				int4		len = *(uint16 *) (ptr->pos) + 1 + *(uint16 *) (res->pos);

				res->pos = (WordEntryPos *) repalloc(res->pos, len * sizeof(WordEntryPos));
				memcpy(&(res->pos[*(uint16 *) (res->pos) + 1]),
					   &(ptr->pos[1]), *(uint16 *) (ptr->pos) * sizeof(WordEntryPos));
				*(uint16 *) (res->pos) += *(uint16 *) (ptr->pos);
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
		*(uint16 *) (res->pos) = uniquePos(&(res->pos[1]), *(uint16 *) (res->pos));
		*outbuflen += *(uint16 *) (res->pos) * sizeof(WordEntryPos);
	}
	*outbuflen += SHORTALIGN(res->entry.len);

	return res + 1 - a;
}

static int
WordEntryCMP(WordEntry * a, WordEntry * b, char *buf)
{
	return compareentry(a, b, buf);
}

#define WAITWORD		1
#define WAITENDWORD		2
#define WAITNEXTCHAR	3
#define WAITENDCMPLX	4
#define WAITPOSINFO		5
#define INPOSINFO		6
#define WAITPOSDELIM	7
#define WAITCHARCMPLX	8

#define RESIZEPRSBUF \
do { \
	if ( state->curpos - state->word + pg_database_encoding_max_length() >= state->len ) \
	{ \
		int4 clen = state->curpos - state->word; \
		state->len *= 2; \
		state->word = (char*)repalloc( (void*)state->word, state->len ); \
		state->curpos = state->word + clen; \
	} \
} while (0)

bool
gettoken_tsvector(TSVectorParseState *state)
{
	int4		oldstate = 0;

	state->curpos = state->word;
	state->state = WAITWORD;
	state->alen = 0;

	while (1)
	{
		if (state->state == WAITWORD)
		{
			if (*(state->prsbuf) == '\0')
				return false;
			else if (t_iseq(state->prsbuf, '\''))
				state->state = WAITENDCMPLX;
			else if (t_iseq(state->prsbuf, '\\'))
			{
				state->state = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (state->oprisdelim && ISOPERATOR(state->prsbuf))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
			else if (!t_isspace(state->prsbuf))
			{
				COPYCHAR(state->curpos, state->prsbuf);
				state->curpos += pg_mblen(state->prsbuf);
				state->state = WAITENDWORD;
			}
		}
		else if (state->state == WAITNEXTCHAR)
		{
			if (*(state->prsbuf) == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("there is no escaped character")));
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(state->curpos, state->prsbuf);
				state->curpos += pg_mblen(state->prsbuf);
				state->state = oldstate;
			}
		}
		else if (state->state == WAITENDWORD)
		{
			if (t_iseq(state->prsbuf, '\\'))
			{
				state->state = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (t_isspace(state->prsbuf) || *(state->prsbuf) == '\0' ||
					 (state->oprisdelim && ISOPERATOR(state->prsbuf)))
			{
				RESIZEPRSBUF;
				if (state->curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				*(state->curpos) = '\0';
				return true;
			}
			else if (t_iseq(state->prsbuf, ':'))
			{
				if (state->curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				*(state->curpos) = '\0';
				if (state->oprisdelim)
					return true;
				else
					state->state = INPOSINFO;
			}
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(state->curpos, state->prsbuf);
				state->curpos += pg_mblen(state->prsbuf);
			}
		}
		else if (state->state == WAITENDCMPLX)
		{
			if (t_iseq(state->prsbuf, '\''))
			{
				state->state = WAITCHARCMPLX;
			}
			else if (t_iseq(state->prsbuf, '\\'))
			{
				state->state = WAITNEXTCHAR;
				oldstate = WAITENDCMPLX;
			}
			else if (*(state->prsbuf) == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(state->curpos, state->prsbuf);
				state->curpos += pg_mblen(state->prsbuf);
			}
		}
		else if (state->state == WAITCHARCMPLX)
		{
			if (t_iseq(state->prsbuf, '\''))
			{
				RESIZEPRSBUF;
				COPYCHAR(state->curpos, state->prsbuf);
				state->curpos += pg_mblen(state->prsbuf);
				state->state = WAITENDCMPLX;
			}
			else
			{
				RESIZEPRSBUF;
				*(state->curpos) = '\0';
				if (state->curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				if (state->oprisdelim)
				{
					/* state->prsbuf+=pg_mblen(state->prsbuf); */
					return true;
				}
				else
					state->state = WAITPOSINFO;
				continue;		/* recheck current character */
			}
		}
		else if (state->state == WAITPOSINFO)
		{
			if (t_iseq(state->prsbuf, ':'))
				state->state = INPOSINFO;
			else
				return true;
		}
		else if (state->state == INPOSINFO)
		{
			if (t_isdigit(state->prsbuf))
			{
				if (state->alen == 0)
				{
					state->alen = 4;
					state->pos = (WordEntryPos *) palloc(sizeof(WordEntryPos) * state->alen);
					*(uint16 *) (state->pos) = 0;
				}
				else if (*(uint16 *) (state->pos) + 1 >= state->alen)
				{
					state->alen *= 2;
					state->pos = (WordEntryPos *) repalloc(state->pos, sizeof(WordEntryPos) * state->alen);
				}
				(*(uint16 *) (state->pos))++;
				WEP_SETPOS(state->pos[*(uint16 *) (state->pos)], LIMITPOS(atoi(state->prsbuf)));
				if (WEP_GETPOS(state->pos[*(uint16 *) (state->pos)]) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("wrong position info in tsvector")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 0);
				state->state = WAITPOSDELIM;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
		}
		else if (state->state == WAITPOSDELIM)
		{
			if (t_iseq(state->prsbuf, ','))
				state->state = INPOSINFO;
			else if (t_iseq(state->prsbuf, 'a') || t_iseq(state->prsbuf, 'A') || t_iseq(state->prsbuf, '*'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 3);
			}
			else if (t_iseq(state->prsbuf, 'b') || t_iseq(state->prsbuf, 'B'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 2);
			}
			else if (t_iseq(state->prsbuf, 'c') || t_iseq(state->prsbuf, 'C'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 1);
			}
			else if (t_iseq(state->prsbuf, 'd') || t_iseq(state->prsbuf, 'D'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 0);
			}
			else if (t_isspace(state->prsbuf) ||
					 *(state->prsbuf) == '\0')
				return true;
			else if (!t_isdigit(state->prsbuf))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
		}
		else					/* internal error */
			elog(ERROR, "internal error in gettoken_tsvector");

		/* get next char */
		state->prsbuf += pg_mblen(state->prsbuf);
	}

	return false;
}

Datum
tsvectorin(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	TSVectorParseState state;
	WordEntryIN *arr;
	WordEntry  *inarr;
	int4		len = 0,
				totallen = 64;
	TSVector	in;
	char	   *tmpbuf,
			   *cur;
	int4		i,
				buflen = 256;

	pg_verifymbstr(buf, strlen(buf), false);
	state.prsbuf = buf;
	state.len = 32;
	state.word = (char *) palloc(state.len);
	state.oprisdelim = false;

	arr = (WordEntryIN *) palloc(sizeof(WordEntryIN) * totallen);
	cur = tmpbuf = (char *) palloc(buflen);

	while (gettoken_tsvector(&state))
	{
		/*
		 * Realloc buffers if it's needed
		 */
		if (len >= totallen)
		{
			totallen *= 2;
			arr = (WordEntryIN *) repalloc((void *) arr, sizeof(WordEntryIN) * totallen);
		}

		while ((cur - tmpbuf) + (state.curpos - state.word) >= buflen)
		{
			int4		dist = cur - tmpbuf;

			buflen *= 2;
			tmpbuf = (char *) repalloc((void *) tmpbuf, buflen);
			cur = tmpbuf + dist;
		}

		if (state.curpos - state.word >= MAXSTRLEN)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("word is too long (%ld bytes, max %ld bytes)",
							(long) (state.curpos - state.word),
							(long) MAXSTRLEN)));

		arr[len].entry.len = state.curpos - state.word;
		if (cur - tmpbuf > MAXSTRPOS)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("position value too large")));
		arr[len].entry.pos = cur - tmpbuf;
		memcpy((void *) cur, (void *) state.word, arr[len].entry.len);
		cur += arr[len].entry.len;

		if (state.alen)
		{
			arr[len].entry.haspos = 1;
			arr[len].pos = state.pos;
		}
		else
			arr[len].entry.haspos = 0;
		len++;
	}
	pfree(state.word);

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
			memcpy(cur, arr[i].pos, (*(uint16 *) arr[i].pos + 1) * sizeof(WordEntryPos));
			cur += (*(uint16 *) arr[i].pos + 1) * sizeof(WordEntryPos);
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
	int			i,
				size,
				len = DATAHDRSIZE;
	WordEntry  *weptr;
	int			datalen = 0;

	size = pq_getmsgint(buf, sizeof(uint32));
	if (size < 0 || size > (MaxAllocSize / sizeof(WordEntry)))
		elog(ERROR, "invalid size of tsvector");

	len += sizeof(WordEntry) * size;

	len *= 2;
	vec = (TSVector) palloc0(len);
	vec->size = size;

	weptr = ARRPTR(vec);
	for (i = 0; i < size; i++)
	{
		int			tmp;

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

			npos = (uint16) pq_getmsgint(buf, sizeof(int16));
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
