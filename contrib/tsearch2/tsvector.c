/*
 * In/Out definitions for tsvector type
 * Internal structure:
 * string of values, array of position lexem in string and it's length
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"
#include "executor/spi.h"
#include "commands/trigger.h"
#include "nodes/pg_list.h"
#include "catalog/namespace.h"

#include "utils/pg_locale.h"

#include <ctype.h>				/* tolower */
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
	if (((WordEntryPos *) a)->pos == ((WordEntryPos *) b)->pos)
		return 0;
	return (((WordEntryPos *) a)->pos > ((WordEntryPos *) b)->pos) ? 1 : -1;
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
		if (ptr->pos != res->pos)
		{
			res++;
			res->pos = ptr->pos;
			res->weight = ptr->weight;
			if (res - a >= MAXNUMPOS - 1 || res->pos == MAXENTRYPOS - 1)
				break;
		}
		else if (ptr->weight > res->weight)
			res->weight = ptr->weight;
		ptr++;
	}
	return res + 1 - a;
}

static char *BufferStr;
static int
compareentry(const void *a, const void *b)
{
	if (((WordEntryIN *) a)->entry.len == ((WordEntryIN *) b)->entry.len)
	{
		return strncmp(
					   &BufferStr[((WordEntryIN *) a)->entry.pos],
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
	BufferStr = buf;
	qsort((void *) a, l, sizeof(WordEntryIN), compareentry);

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

#define WAITWORD	1
#define WAITENDWORD 2
#define WAITNEXTCHAR	3
#define WAITENDCMPLX	4
#define WAITPOSINFO 5
#define INPOSINFO	6
#define WAITPOSDELIM	7

#define RESIZEPRSBUF \
do { \
	if ( state->curpos - state->word + 1 >= state->len ) \
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
			else if (*(state->prsbuf) == '\'')
				state->state = WAITENDCMPLX;
			else if (*(state->prsbuf) == '\\')
			{
				state->state = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (state->oprisdelim && ISOPERATOR(*(state->prsbuf)))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));
			else if (*(state->prsbuf) != ' ')
			{
				*(state->curpos) = *(state->prsbuf);
				state->curpos++;
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
				*(state->curpos) = *(state->prsbuf);
				state->curpos++;
				state->state = oldstate;
			}
		}
		else if (state->state == WAITENDWORD)
		{
			if (*(state->prsbuf) == '\\')
			{
				state->state = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (*(state->prsbuf) == ' ' || *(state->prsbuf) == '\0' ||
					 (state->oprisdelim && ISOPERATOR(*(state->prsbuf))))
			{
				RESIZEPRSBUF;
				if (state->curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				*(state->curpos) = '\0';
				return 1;
			}
			else if (*(state->prsbuf) == ':')
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
				*(state->curpos) = *(state->prsbuf);
				state->curpos++;
			}
		}
		else if (state->state == WAITENDCMPLX)
		{
			if (*(state->prsbuf) == '\'')
			{
				RESIZEPRSBUF;
				*(state->curpos) = '\0';
				if (state->curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				if (state->oprisdelim)
				{
					state->prsbuf++;
					return 1;
				}
				else
					state->state = WAITPOSINFO;
			}
			else if (*(state->prsbuf) == '\\')
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
				*(state->curpos) = *(state->prsbuf);
				state->curpos++;
			}
		}
		else if (state->state == WAITPOSINFO)
		{
			if (*(state->prsbuf) == ':')
				state->state = INPOSINFO;
			else
				return 1;
		}
		else if (state->state == INPOSINFO)
		{
			if (isdigit(*(state->prsbuf)))
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
				state->pos[*(uint16 *) (state->pos)].pos = LIMITPOS(atoi(state->prsbuf));
				if (state->pos[*(uint16 *) (state->pos)].pos == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("wrong position info")));
				state->pos[*(uint16 *) (state->pos)].weight = 0;
				state->state = WAITPOSDELIM;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));
		}
		else if (state->state == WAITPOSDELIM)
		{
			if (*(state->prsbuf) == ',')
				state->state = INPOSINFO;
			else if (tolower(*(state->prsbuf)) == 'a' || *(state->prsbuf) == '*')
			{
				if (state->pos[*(uint16 *) (state->pos)].weight)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				state->pos[*(uint16 *) (state->pos)].weight = 3;
			}
			else if (tolower(*(state->prsbuf)) == 'b')
			{
				if (state->pos[*(uint16 *) (state->pos)].weight)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				state->pos[*(uint16 *) (state->pos)].weight = 2;
			}
			else if (tolower(*(state->prsbuf)) == 'c')
			{
				if (state->pos[*(uint16 *) (state->pos)].weight)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				state->pos[*(uint16 *) (state->pos)].weight = 1;
			}
			else if (tolower(*(state->prsbuf)) == 'd')
			{
				if (state->pos[*(uint16 *) (state->pos)].weight)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error")));
				state->pos[*(uint16 *) (state->pos)].weight = 0;
			}
			else if (isspace(*(state->prsbuf)) || *(state->prsbuf) == '\0')
				return 1;
			else if (!isdigit(*(state->prsbuf)))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));
		}
		else
			/* internal error */
			elog(ERROR, "internal error");
		state->prsbuf++;
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
		buflen=0;
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
				j,
				lenbuf = 0,
				pp;
	WordEntry  *ptr = ARRPTR(out);
	char	   *curin,
			   *curout;

	lenbuf = out->size * 2 /* '' */ + out->size - 1 /* space */ + 2 /* \0 */ ;
	for (i = 0; i < out->size; i++)
	{
		lenbuf += ptr[i].len * 2 /* for escape */ ;
		if (ptr[i].haspos)
			lenbuf += 7 * POSDATALEN(out, &(ptr[i]));
	}

	curout = outbuf = (char *) palloc(lenbuf);
	for (i = 0; i < out->size; i++)
	{
		curin = STRPTR(out) + ptr->pos;
		if (i != 0)
			*curout++ = ' ';
		*curout++ = '\'';
		j = ptr->len;
		while (j--)
		{
			if (*curin == '\'')
			{
				int4		pos = curout - outbuf;

				outbuf = (char *) repalloc((void *) outbuf, ++lenbuf);
				curout = outbuf + pos;
				*curout++ = '\\';
			}
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
				sprintf(curout, "%d", wptr->pos);
				curout = strchr(curout, '\0');
				switch (wptr->weight)
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
	if (((WORD *) a)->len == ((WORD *) b)->len)
	{
		int			res = strncmp(
								  ((WORD *) a)->word,
								  ((WORD *) b)->word,
								  ((WORD *) b)->len);

		if (res == 0)
			return (((WORD *) a)->pos.pos > ((WORD *) b)->pos.pos) ? 1 : -1;
		return res;
	}
	return (((WORD *) a)->len > ((WORD *) b)->len) ? 1 : -1;
}

static int
uniqueWORD(WORD * a, int4 l)
{
	WORD	   *ptr,
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

	qsort((void *) a, l, sizeof(WORD), compareWORD);
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
			if (res->pos.apos[0] < MAXNUMPOS - 1 && res->pos.apos[res->pos.apos[0]] != MAXENTRYPOS - 1)
			{
				if (res->pos.apos[0] + 1 >= res->alen)
				{
					res->alen *= 2;
					res->pos.apos = (uint16 *) repalloc(res->pos.apos, sizeof(uint16) * res->alen);
				}
				if ( res->pos.apos[0]==0 || res->pos.apos[res->pos.apos[0]] != LIMITPOS(ptr->pos.pos) ) { 
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
				wptr[j].weight = 0;
				wptr[j].pos = prs->words[i].pos.apos[j + 1];
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
	TSCfgInfo  *cfg = findcfg(PG_GETARG_INT32(0));

	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (WORD *) palloc(sizeof(WORD) * prs.lenwords);

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
	Datum		res = DirectFunctionCall3(
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
	Datum		res = DirectFunctionCall3(
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
	List	   *names = makeList1(makeString(fname));

	ptr = clist = FuncnameGetCandidates(names, 1);
	freeList(names);

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
	TSCfgInfo  *cfg = findcfg(get_currcfg());
	int			numidxattr,
				i;
	PRSTEXT		prs;
	Datum		datum = (Datum) 0;
	Oid			funcoid = InvalidOid;

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
	prs.words = (WORD *) palloc(sizeof(WORD) * prs.lenwords);

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
