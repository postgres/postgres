/*
 * In/Out definitions for tsvector type
 * Internal structure:
 * string of values, array of position lexeme in string and it's length
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
#include "mb/pg_wchar.h"

#include <ctype.h>
#include "tsvector.h"
#include "query.h"
#include "ts_cfg.h"
#include "common.h"

PG_FUNCTION_INFO_V1(tsvector_in);
Datum		tsvector_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsvector_out);
Datum		tsvector_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(to_tsvector);
Datum		to_tsvector(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(to_tsvector_current);
Datum		to_tsvector_current(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(to_tsvector_name);
Datum		to_tsvector_name(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsearch2);
Datum		tsearch2(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsvector_length);
Datum		tsvector_length(PG_FUNCTION_ARGS);

/*
 * in/out text index type
 */
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

	res = a;
	if (l == 1)
		return l;

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
	char *BufferStr = (char *) arg;

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


int4
gettoken_tsvector(TI_IN_STATE * state)
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
				return 0;
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
						 errmsg("syntax error")));
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
							 errmsg("syntax error")));
				*(state->curpos) = '\0';
				return 1;
			}
			else if (t_iseq(state->prsbuf, ':'))
			{
				if (state->curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				*(state->curpos) = '\0';
				if (state->oprisdelim)
					return 1;
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
						 errmsg("syntax error")));
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
							 errmsg("syntax error")));
				if (state->oprisdelim)
				{
					/* state->prsbuf+=pg_mblen(state->prsbuf); */
					return 1;
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
				return 1;
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
							 errmsg("wrong position info")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 0);
				state->state = WAITPOSDELIM;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));
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
							 errmsg("syntax error")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 3);
			}
			else if (t_iseq(state->prsbuf, 'b') || t_iseq(state->prsbuf, 'B'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 2);
			}
			else if (t_iseq(state->prsbuf, 'c') || t_iseq(state->prsbuf, 'C'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 1);
			}
			else if (t_iseq(state->prsbuf, 'd') || t_iseq(state->prsbuf, 'D'))
			{
				if (WEP_GETWEIGHT(state->pos[*(uint16 *) (state->pos)]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				WEP_SETWEIGHT(state->pos[*(uint16 *) (state->pos)], 0);
			}
			else if (t_isspace(state->prsbuf) ||
					 *(state->prsbuf) == '\0')
				return 1;
			else if (!t_isdigit(state->prsbuf))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));
		}
		else
			/* internal error */
			elog(ERROR, "internal error");

		/* get next char */
		state->prsbuf += pg_mblen(state->prsbuf);
	}

	return 0;
}

Datum
tsvector_in(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	TI_IN_STATE state;
	WordEntryIN *arr;
	WordEntry  *inarr;
	int4		len = 0,
				totallen = 64;
	tsvector   *in;
	char	   *tmpbuf,
			   *cur;
	int4		i,
				buflen = 256;

	SET_FUNCOID();

	pg_verifymbstr(buf, strlen(buf), false);
	state.prsbuf = buf;
	state.len = 32;
	state.word = (char *) palloc(state.len);
	state.oprisdelim = false;

	arr = (WordEntryIN *) palloc(sizeof(WordEntryIN) * totallen);
	cur = tmpbuf = (char *) palloc(buflen);
	while (gettoken_tsvector(&state))
	{
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
					 errmsg("word is too long")));
		arr[len].entry.len = state.curpos - state.word;
		if (cur - tmpbuf > MAXSTRPOS)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("too long value")));
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
	in = (tsvector *) palloc(totallen);
	memset(in, 0, totallen);
	in->len = totallen;
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
		memcpy(&(inarr[i]), &(arr[i].entry), sizeof(WordEntry));
	}
	pfree(tmpbuf);
	pfree(arr);
	PG_RETURN_POINTER(in);
}

Datum
tsvector_length(PG_FUNCTION_ARGS)
{
	tsvector   *in = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	int4		ret = in->size;

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_INT32(ret);
}

Datum
tsvector_out(PG_FUNCTION_ARGS)
{
	tsvector   *out = (tsvector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
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
			lenbuf += 7 * POSDATALEN(out, &(ptr[i]));
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
			{
				int4		pos = curout - outbuf;

				outbuf = (char *) repalloc((void *) outbuf, ++lenbuf);
				curout = outbuf + pos;
				*curout++ = '\'';
			}
			else if (t_iseq(curin, '\\'))
			{
				int4		pos = curout - outbuf;

				outbuf = (char *) repalloc((void *) outbuf, ++lenbuf);
				curout = outbuf + pos;
				*curout++ = '\\';
			}
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
				sprintf(curout, "%d", WEP_GETPOS(*wptr));
				curout = strchr(curout, '\0');
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
	outbuf[lenbuf - 1] = '\0';
	PG_FREE_IF_COPY(out, 0);
	PG_RETURN_POINTER(outbuf);
}

static int
compareWORD(const void *a, const void *b)
{
	if (((TSWORD *) a)->len == ((TSWORD *) b)->len)
	{
		int			res = strncmp(
								  ((TSWORD *) a)->word,
								  ((TSWORD *) b)->word,
								  ((TSWORD *) b)->len);

		if (res == 0)
		{
			if ( ((TSWORD *) a)->pos.pos == ((TSWORD *) b)->pos.pos )
				return 0;

			return (((TSWORD *) a)->pos.pos > ((TSWORD *) b)->pos.pos) ? 1 : -1;
		}
		return res;
	}
	return (((TSWORD *) a)->len > ((TSWORD *) b)->len) ? 1 : -1;
}

static int
uniqueWORD(TSWORD * a, int4 l)
{
	TSWORD	   *ptr,
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

	qsort((void *) a, l, sizeof(TSWORD), compareWORD);
	tmppos = LIMITPOS(a->pos.pos);
	a->alen = 2;
	a->pos.apos = (uint16 *) palloc(sizeof(uint16) * a->alen);
	a->pos.apos[0] = 1;
	a->pos.apos[1] = tmppos;

	while (ptr - a < l)
	{
		if (!(ptr->len == res->len &&
			  strncmp(ptr->word, res->word, res->len) == 0))
		{
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
			pfree(ptr->word);
			if (res->pos.apos[0] < MAXNUMPOS - 1 && res->pos.apos[res->pos.apos[0]] != MAXENTRYPOS - 1 &&
				res->pos.apos[res->pos.apos[0]] != LIMITPOS(ptr->pos.pos) )
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
 * make value of tsvector
 */
static tsvector *
makevalue(PRSTEXT * prs)
{
	int4		i,
				j,
				lenstr = 0,
				totallen;
	tsvector   *in;
	WordEntry  *ptr;
	char	   *str,
			   *cur;

	prs->curwords = uniqueWORD(prs->words, prs->curwords);
	for (i = 0; i < prs->curwords; i++)
	{
		lenstr += SHORTALIGN(prs->words[i].len);

		if (prs->words[i].alen)
			lenstr += sizeof(uint16) + prs->words[i].pos.apos[0] * sizeof(WordEntryPos);
	}

	totallen = CALCDATASIZE(prs->curwords, lenstr);
	in = (tsvector *) palloc(totallen);
	memset(in, 0, totallen);
	in->len = totallen;
	in->size = prs->curwords;

	ptr = ARRPTR(in);
	cur = str = STRPTR(in);
	for (i = 0; i < prs->curwords; i++)
	{
		ptr->len = prs->words[i].len;
		if (cur - str > MAXSTRPOS)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("value is too big")));
		ptr->pos = cur - str;
		memcpy((void *) cur, (void *) prs->words[i].word, prs->words[i].len);
		pfree(prs->words[i].word);
		cur += SHORTALIGN(prs->words[i].len);
		if (prs->words[i].alen)
		{
			WordEntryPos *wptr;

			ptr->haspos = 1;
			*(uint16 *) cur = prs->words[i].pos.apos[0];
			wptr = POSDATAPTR(in, ptr);
			for (j = 0; j < *(uint16 *) cur; j++)
			{
				WEP_SETWEIGHT(wptr[j], 0);
				WEP_SETPOS(wptr[j], prs->words[i].pos.apos[j + 1]);
			}
			cur += sizeof(uint16) + prs->words[i].pos.apos[0] * sizeof(WordEntryPos);
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
to_tsvector(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(1);
	PRSTEXT		prs;
	tsvector   *out = NULL;
	TSCfgInfo  *cfg;

	SET_FUNCOID();
	cfg = findcfg(PG_GETARG_INT32(0));

	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (TSWORD *) palloc(sizeof(TSWORD) * prs.lenwords);

	parsetext_v2(cfg, &prs, VARDATA(in), VARSIZE(in) - VARHDRSZ);
	PG_FREE_IF_COPY(in, 1);

	if (prs.curwords)
		out = makevalue(&prs);
	else
	{
		pfree(prs.words);
		out = palloc(CALCDATASIZE(0, 0));
		out->len = CALCDATASIZE(0, 0);
		out->size = 0;
	}
	PG_RETURN_POINTER(out);
}

Datum
to_tsvector_name(PG_FUNCTION_ARGS)
{
	text	   *cfg = PG_GETARG_TEXT_P(0);
	Datum		res;

	SET_FUNCOID();
	res = DirectFunctionCall3(
							  to_tsvector,
							  Int32GetDatum(name2id_cfg(cfg)),
							  PG_GETARG_DATUM(1),
							  (Datum) 0
		);

	PG_FREE_IF_COPY(cfg, 0);
	PG_RETURN_DATUM(res);
}

Datum
to_tsvector_current(PG_FUNCTION_ARGS)
{
	Datum		res;

	SET_FUNCOID();
	res = DirectFunctionCall3(
							  to_tsvector,
							  Int32GetDatum(get_currcfg()),
							  PG_GETARG_DATUM(0),
							  (Datum) 0
		);

	PG_RETURN_DATUM(res);
}

static Oid
findFunc(char *fname)
{
	FuncCandidateList clist,
				ptr;
	Oid			funcid = InvalidOid;
	List	   *names = list_make1(makeString(fname));

	ptr = clist = FuncnameGetCandidates(names, 1);
	list_free(names);

	if (!ptr)
		return funcid;

	while (ptr)
	{
		if (ptr->args[0] == TEXTOID && funcid == InvalidOid)
			funcid = ptr->oid;
		clist = ptr->next;
		pfree(ptr);
		ptr = clist;
	}

	return funcid;
}

/*
 * Trigger
 */
Datum
tsearch2(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata;
	Trigger    *trigger;
	Relation	rel;
	HeapTuple	rettuple = NULL;
	int			numidxattr,
				i;
	PRSTEXT		prs;
	Datum		datum = (Datum) 0;
	Oid			funcoid = InvalidOid;
	TSCfgInfo  *cfg;

	SET_FUNCOID();
	cfg = findcfg(get_currcfg());

	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error */
		elog(ERROR, "TSearch: Not fired by trigger manager");

	trigdata = (TriggerData *) fcinfo->context;
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		/* internal error */
		elog(ERROR, "TSearch: Can't process STATEMENT events");
	if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		/* internal error */
		elog(ERROR, "TSearch: Must be fired BEFORE event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		rettuple = trigdata->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		/* internal error */
		elog(ERROR, "TSearch: Unknown event");

	trigger = trigdata->tg_trigger;
	rel = trigdata->tg_relation;

	if (trigger->tgnargs < 2)
		/* internal error */
		elog(ERROR, "TSearch: format tsearch2(tsvector_field, text_field1,...)");

	numidxattr = SPI_fnumber(rel->rd_att, trigger->tgargs[0]);
	if (numidxattr == SPI_ERROR_NOATTRIBUTE)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("tsvector column \"%s\" does not exist",
						trigger->tgargs[0])));

	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (TSWORD *) palloc(sizeof(TSWORD) * prs.lenwords);

	/* find all words in indexable column */
	for (i = 1; i < trigger->tgnargs; i++)
	{
		int			numattr;
		Oid			oidtype;
		Datum		txt_toasted;
		bool		isnull;
		text	   *txt;

		numattr = SPI_fnumber(rel->rd_att, trigger->tgargs[i]);
		if (numattr == SPI_ERROR_NOATTRIBUTE)
		{
			funcoid = findFunc(trigger->tgargs[i]);
			if (funcoid == InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("could not find function or field \"%s\"",
								trigger->tgargs[i])));

			continue;
		}
		oidtype = SPI_gettypeid(rel->rd_att, numattr);
		/* We assume char() and varchar() are binary-equivalent to text */
		if (!(oidtype == TEXTOID ||
			  oidtype == VARCHAROID ||
			  oidtype == BPCHAROID))
		{
			elog(WARNING, "TSearch: '%s' is not of character type",
				 trigger->tgargs[i]);
			continue;
		}
		txt_toasted = SPI_getbinval(rettuple, rel->rd_att, numattr, &isnull);
		if (isnull)
			continue;

		if (funcoid != InvalidOid)
		{
			text	   *txttmp = (text *) DatumGetPointer(OidFunctionCall1(
																	 funcoid,
												 PointerGetDatum(txt_toasted)
																		   ));

			txt = (text *) DatumGetPointer(PG_DETOAST_DATUM(PointerGetDatum(txttmp)));
			if (txt == txttmp)
				txt_toasted = PointerGetDatum(txt);
		}
		else
			txt = (text *) DatumGetPointer(PG_DETOAST_DATUM(PointerGetDatum(txt_toasted)));

		parsetext_v2(cfg, &prs, VARDATA(txt), VARSIZE(txt) - VARHDRSZ);
		if (txt != (text *) DatumGetPointer(txt_toasted))
			pfree(txt);
	}

	/* make tsvector value */
	if (prs.curwords)
	{
		datum = PointerGetDatum(makevalue(&prs));
		rettuple = SPI_modifytuple(rel, rettuple, 1, &numidxattr,
								   &datum, NULL);
		pfree(DatumGetPointer(datum));
	}
	else
	{
		tsvector   *out = palloc(CALCDATASIZE(0, 0));

		out->len = CALCDATASIZE(0, 0);
		out->size = 0;
		datum = PointerGetDatum(out);
		pfree(prs.words);
		rettuple = SPI_modifytuple(rel, rettuple, 1, &numidxattr,
								   &datum, NULL);
	}

	if (rettuple == NULL)
		/* internal error */
		elog(ERROR, "TSearch: %d returned by SPI_modifytuple", SPI_result);

	return PointerGetDatum(rettuple);
}

static int
silly_cmp_tsvector(const tsvector * a, const tsvector * b)
{
	if (a->len < b->len)
		return -1;
	else if (a->len > b->len)
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

PG_FUNCTION_INFO_V1(tsvector_cmp);
PG_FUNCTION_INFO_V1(tsvector_lt);
PG_FUNCTION_INFO_V1(tsvector_le);
PG_FUNCTION_INFO_V1(tsvector_eq);
PG_FUNCTION_INFO_V1(tsvector_ne);
PG_FUNCTION_INFO_V1(tsvector_ge);
PG_FUNCTION_INFO_V1(tsvector_gt);
Datum		tsvector_cmp(PG_FUNCTION_ARGS);
Datum		tsvector_lt(PG_FUNCTION_ARGS);
Datum		tsvector_le(PG_FUNCTION_ARGS);
Datum		tsvector_eq(PG_FUNCTION_ARGS);
Datum		tsvector_ne(PG_FUNCTION_ARGS);
Datum		tsvector_ge(PG_FUNCTION_ARGS);
Datum		tsvector_gt(PG_FUNCTION_ARGS);

#define RUNCMP										\
tsvector *a		   = (tsvector *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));\
tsvector *b		   = (tsvector *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));\
int res = silly_cmp_tsvector(a,b);							\
PG_FREE_IF_COPY(a,0);									\
PG_FREE_IF_COPY(b,1);									\

Datum
tsvector_cmp(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_INT32(res);
}

Datum
tsvector_lt(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_BOOL((res < 0) ? true : false);
}

Datum
tsvector_le(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_BOOL((res <= 0) ? true : false);
}

Datum
tsvector_eq(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_BOOL((res == 0) ? true : false);
}

Datum
tsvector_ge(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_BOOL((res >= 0) ? true : false);
}

Datum
tsvector_gt(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_BOOL((res > 0) ? true : false);
}

Datum
tsvector_ne(PG_FUNCTION_ARGS)
{
	RUNCMP
		PG_RETURN_BOOL((res != 0) ? true : false);
}
