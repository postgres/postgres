/*
 * In/Out definitions for txtidx type
 * Internal structure:
 * string of values, array of position lexem in string and it's length
 * Teodor Sigaev <teodor@stack.net>
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"
#include "executor/spi.h"
#include "commands/trigger.h"

#include "utils/pg_locale.h"

#include <ctype.h>				/* tolower */
#include "txtidx.h"
#include "query.h"

#include "deflex.h"
#include "parser.h"

#include "morph.h"

PG_FUNCTION_INFO_V1(txtidx_in);
Datum		txtidx_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(txtidx_out);
Datum		txtidx_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(txt2txtidx);
Datum		txt2txtidx(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsearch);
Datum		tsearch(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(txtidxsize);
Datum		txtidxsize(PG_FUNCTION_ARGS);

/*
 * in/out text index type
 */
static char *BufferStr;
static int
compareentry(const void *a, const void *b)
{
	if (((WordEntry *) a)->len == ((WordEntry *) b)->len)
	{
		return strncmp(
					   &BufferStr[((WordEntry *) a)->pos],
					   &BufferStr[((WordEntry *) b)->pos],
					   ((WordEntry *) b)->len);
	}
	return (((WordEntry *) a)->len > ((WordEntry *) b)->len) ? 1 : -1;
}

static int
uniqueentry(WordEntry * a, int4 l, char *buf, int4 *outbuflen)
{
	WordEntry  *ptr,
			   *res;

	res = a;
	*outbuflen = res->len;
	if (l == 1)
		return l;

	ptr = a + 1;
	BufferStr = buf;
	qsort((void *) a, l, sizeof(int4), compareentry);
	*outbuflen = res->len;

	while (ptr - a < l)
	{
		if (!(ptr->len == res->len &&
			  strncmp(&buf[ptr->pos], &buf[res->pos], res->len) == 0))
		{
			res++;
			res->len = ptr->len;
			res->pos = ptr->pos;
			*outbuflen += res->len;

		}
		ptr++;
	}
	return res + 1 - a;
}

#define WAITWORD	1
#define WAITENDWORD 2
#define WAITNEXTCHAR	3
#define WAITENDCMPLX	4

#define RESIZEPRSBUF \
do { \
	if ( state->curpos - state->word == state->len ) \
	{ \
		int4 clen = state->curpos - state->word; \
		state->len *= 2; \
		state->word = (char*)repalloc( (void*)state->word, state->len ); \
		state->curpos = state->word + clen; \
	} \
} while (0)

int4
gettoken_txtidx(TI_IN_STATE * state)
{
	int4		oldstate = 0;

	state->curpos = state->word;
	state->state = WAITWORD;

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
				state->prsbuf++;
				return 1;
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
		else
			/* internal error */
			elog(ERROR, "internal error");
		state->prsbuf++;
	}

	return 0;
}

Datum
txtidx_in(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	TI_IN_STATE state;
	WordEntry  *arr;
	int4		len = 0,
				totallen = 64;
	txtidx	   *in;
	char	   *tmpbuf,
			   *cur;
	int4		i,
				buflen = 256;

	state.prsbuf = buf;
	state.len = 32;
	state.word = (char *) palloc(state.len);
	state.oprisdelim = false;

	arr = (WordEntry *) palloc(sizeof(WordEntry) * totallen);
	cur = tmpbuf = (char *) palloc(buflen);
	while (gettoken_txtidx(&state))
	{
		if (len == totallen)
		{
			totallen *= 2;
			arr = (WordEntry *) repalloc((void *) arr, sizeof(int4) * totallen);
		}
		while (cur - tmpbuf + state.curpos - state.word >= buflen)
		{
			int4		dist = cur - tmpbuf;

			buflen *= 2;
			tmpbuf = (char *) repalloc((void *) tmpbuf, buflen);
			cur = tmpbuf + dist;
		}
		if (state.curpos - state.word > 0xffff)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("word is too long")));
		arr[len].len = state.curpos - state.word;
		if (cur - tmpbuf > 0xffff)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("too long value")));
		arr[len].pos = cur - tmpbuf;
		memcpy((void *) cur, (void *) state.word, arr[len].len);
		cur += arr[len].len;
		len++;
	}
	pfree(state.word);

	if (!len)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("void value")));

	len = uniqueentry(arr, len, tmpbuf, &buflen);
	totallen = CALCDATASIZE(len, buflen);
	in = (txtidx *) palloc(totallen);
	in->len = totallen;
	in->size = len;
	cur = STRPTR(in);
	for (i = 0; i < len; i++)
	{
		memcpy((void *) cur, (void *) &tmpbuf[arr[i].pos], arr[i].len);
		arr[i].pos = cur - STRPTR(in);
		cur += arr[i].len;
	}
	pfree(tmpbuf);
	memcpy((void *) ARRPTR(in), (void *) arr, sizeof(int4) * len);
	pfree(arr);
	PG_RETURN_POINTER(in);
}

Datum
txtidxsize(PG_FUNCTION_ARGS)
{
	txtidx	   *in = (txtidx *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	int4		ret = in->size;

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_INT32(ret);
}

Datum
txtidx_out(PG_FUNCTION_ARGS)
{
	txtidx	   *out = (txtidx *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	char	   *outbuf;
	int4		i,
				j,
				lenbuf = STRSIZE(out) + 1 /* \0 */ + out->size * 2 /* '' */ + out->size - 1 /* space */ ;
	WordEntry  *ptr = ARRPTR(out);
	char	   *curin,
			   *curout;

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
		ptr++;
	}
	outbuf[lenbuf - 1] = '\0';
	PG_FREE_IF_COPY(out, 0);
	PG_RETURN_POINTER(outbuf);
}

typedef struct
{
	uint16		len;
	char	   *word;
}	WORD;

typedef struct
{
	WORD	   *words;
	int4		lenwords;
	int4		curwords;
}	PRSTEXT;

/*
 * Parse text to lexems
 */
static void
parsetext(PRSTEXT * prs, char *buf, int4 buflen)
{
	int			type,
				lenlemm;
	char	   *ptr,
			   *ptrw;
	char	   *lemm;

	start_parse_str(buf, buflen);
	while ((type = tsearch_yylex()) != 0)
	{
		if (prs->curwords == prs->lenwords)
		{
			prs->lenwords *= 2;
			prs->words = (WORD *) repalloc((void *) prs->words, prs->lenwords * sizeof(WORD));
		}
		if (tokenlen > 0xffff)
		{
			end_parse();
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("word is too long")));
		}

		lenlemm = tokenlen;
		lemm = lemmatize(token, &lenlemm, type);

		if (!lemm)
			continue;

		if (lemm != token)
		{
			prs->words[prs->curwords].len = lenlemm;
			prs->words[prs->curwords].word = lemm;
		}
		else
		{
			prs->words[prs->curwords].len = lenlemm;
			ptrw = prs->words[prs->curwords].word = (char *) palloc(lenlemm);
			ptr = token;
			while (ptr - token < lenlemm)
			{
				*ptrw = tolower((unsigned char) *ptr);
				ptr++;
				ptrw++;
			}
		}
		prs->curwords++;
	}
	end_parse();
}

static int
compareWORD(const void *a, const void *b)
{
	if (((WORD *) a)->len == ((WORD *) b)->len)
		return strncmp(
					   ((WORD *) a)->word,
					   ((WORD *) b)->word,
					   ((WORD *) b)->len);
	return (((WORD *) a)->len > ((WORD *) b)->len) ? 1 : -1;
}

static int
uniqueWORD(WORD * a, int4 l)
{
	WORD	   *ptr,
			   *res;

	if (l == 1)
		return l;

	res = a;
	ptr = a + 1;

	qsort((void *) a, l, sizeof(WORD), compareWORD);

	while (ptr - a < l)
	{
		if (!(ptr->len == res->len &&
			  strncmp(ptr->word, res->word, res->len) == 0))
		{
			res++;
			res->len = ptr->len;
			res->word = ptr->word;
		}
		else
			pfree(ptr->word);
		ptr++;
	}

	return res + 1 - a;
}

/*
 * make value of txtidx
 */
static txtidx *
makevalue(PRSTEXT * prs)
{
	int4		i,
				lenstr = 0,
				totallen;
	txtidx	   *in;
	WordEntry  *ptr;
	char	   *str,
			   *cur;

	prs->curwords = uniqueWORD(prs->words, prs->curwords);
	for (i = 0; i < prs->curwords; i++)
		lenstr += prs->words[i].len;

	totallen = CALCDATASIZE(prs->curwords, lenstr);
	in = (txtidx *) palloc(totallen);
	in->len = totallen;
	in->size = prs->curwords;

	ptr = ARRPTR(in);
	cur = str = STRPTR(in);
	for (i = 0; i < prs->curwords; i++)
	{
		ptr->len = prs->words[i].len;
		if (cur - str > 0xffff)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("value is too big")));
		ptr->pos = cur - str;
		ptr++;
		memcpy((void *) cur, (void *) prs->words[i].word, prs->words[i].len);
		pfree(prs->words[i].word);
		cur += prs->words[i].len;
	}
	pfree(prs->words);
	return in;
}

Datum
txt2txtidx(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(0);
	PRSTEXT		prs;
	txtidx	   *out = NULL;

	prs.lenwords = 32;
	prs.curwords = 0;
	prs.words = (WORD *) palloc(sizeof(WORD) * prs.lenwords);

	initmorph();
	parsetext(&prs, VARDATA(in), VARSIZE(in) - VARHDRSZ);
	PG_FREE_IF_COPY(in, 0);

	if (prs.curwords)
	{
		out = makevalue(&prs);
		PG_RETURN_POINTER(out);
	}
	pfree(prs.words);
	PG_RETURN_NULL();
}

/*
 * Trigger
 */
Datum
tsearch(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata;
	Trigger    *trigger;
	Relation	rel;
	HeapTuple	rettuple = NULL;
	int			numidxattr,
				i;
	PRSTEXT		prs;
	Datum		datum = (Datum) 0;

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
		elog(ERROR, "TSearch: format tsearch(txtidx_field, text_field1,...)");

	numidxattr = SPI_fnumber(rel->rd_att, trigger->tgargs[0]);
	if (numidxattr == SPI_ERROR_NOATTRIBUTE)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("could not find txtidx_field")));
	prs.lenwords = 32;
	prs.curwords = 0;
	prs.words = (WORD *) palloc(sizeof(WORD) * prs.lenwords);

	initmorph();
	/* find all words in indexable column */
	for (i = 1; i < trigger->tgnargs; i++)
	{
		int			numattr;
		Oid			oidtype;
		Datum		txt_datum;
		bool		isnull;
		text	   *txt;

		numattr = SPI_fnumber(rel->rd_att, trigger->tgargs[i]);
		if (numattr == SPI_ERROR_NOATTRIBUTE)
		{
			elog(WARNING, "TSearch: can not find field '%s'",
				 trigger->tgargs[i]);
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
		txt_datum = SPI_getbinval(rettuple, rel->rd_att, numattr, &isnull);
		if (isnull)
			continue;
		txt = DatumGetTextP(txt_datum);

		parsetext(&prs, VARDATA(txt), VARSIZE(txt) - VARHDRSZ);
	}

	/* make txtidx value */
	if (prs.curwords)
	{
		datum = PointerGetDatum(makevalue(&prs));
		rettuple = SPI_modifytuple(rel, rettuple, 1, &numidxattr,
								   &datum, NULL);
		pfree(DatumGetPointer(datum));
	}
	else
	{
		char		nulls = 'n';

		pfree(prs.words);
		rettuple = SPI_modifytuple(rel, rettuple, 1, &numidxattr,
								   &datum, &nulls);
	}

	if (rettuple == NULL)
		/* internal error */
		elog(ERROR, "TSearch: %d returned by SPI_modifytuple", SPI_result);

	return PointerGetDatum(rettuple);
}
