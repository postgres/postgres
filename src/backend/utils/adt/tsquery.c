/*-------------------------------------------------------------------------
 *
 * tsquery.c
 *	  I/O functions for tsquery
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsquery.c,v 1.2 2007/08/31 02:26:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"


/* parser's states */
#define WAITOPERAND 1
#define WAITOPERATOR	2
#define WAITFIRSTOPERAND 3
#define WAITSINGLEOPERAND 4

/*
 * node of query tree, also used
 * for storing polish notation in parser
 */
typedef struct ParseQueryNode
{
	int2		weight;
	int2		type;
	int4		val;
	int2		distance;
	int2		length;
	struct ParseQueryNode *next;
} ParseQueryNode;

static char *
get_weight(char *buf, int2 *weight)
{
	*weight = 0;

	if (!t_iseq(buf, ':'))
		return buf;

	buf++;
	while (*buf && pg_mblen(buf) == 1)
	{
		switch (*buf)
		{
			case 'a':
			case 'A':
				*weight |= 1 << 3;
				break;
			case 'b':
			case 'B':
				*weight |= 1 << 2;
				break;
			case 'c':
			case 'C':
				*weight |= 1 << 1;
				break;
			case 'd':
			case 'D':
				*weight |= 1;
				break;
			default:
				return buf;
		}
		buf++;
	}

	return buf;
}

/*
 * get token from query string
 */
static int4
gettoken_query(TSQueryParserState * state, int4 *val, int4 *lenval, char **strval, int2 *weight)
{
	while (1)
	{
		switch (state->state)
		{
			case WAITFIRSTOPERAND:
			case WAITOPERAND:
				if (t_iseq(state->buf, '!'))
				{
					(state->buf)++;		/* can safely ++, t_iseq guarantee
										 * that pg_mblen()==1 */
					*val = (int4) '!';
					state->state = WAITOPERAND;
					return OPR;
				}
				else if (t_iseq(state->buf, '('))
				{
					state->count++;
					(state->buf)++;
					state->state = WAITOPERAND;
					return OPEN;
				}
				else if (t_iseq(state->buf, ':'))
				{
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error at start of operand in tsearch query: \"%s\"",
									state->buffer)));
				}
				else if (!t_isspace(state->buf))
				{
					state->valstate.prsbuf = state->buf;
					if (gettoken_tsvector(&(state->valstate)))
					{
						*strval = state->valstate.word;
						*lenval = state->valstate.curpos - state->valstate.word;
						state->buf = get_weight(state->valstate.prsbuf, weight);
						state->state = WAITOPERATOR;
						return VAL;
					}
					else if (state->state == WAITFIRSTOPERAND)
						return END;
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("no operand in tsearch query: \"%s\"",
										state->buffer)));
				}
				break;
			case WAITOPERATOR:
				if (t_iseq(state->buf, '&') || t_iseq(state->buf, '|'))
				{
					state->state = WAITOPERAND;
					*val = (int4) *(state->buf);
					(state->buf)++;
					return OPR;
				}
				else if (t_iseq(state->buf, ')'))
				{
					(state->buf)++;
					state->count--;
					return (state->count < 0) ? ERR : CLOSE;
				}
				else if (*(state->buf) == '\0')
					return (state->count) ? ERR : END;
				else if (!t_isspace(state->buf))
					return ERR;
				break;
			case WAITSINGLEOPERAND:
				if (*(state->buf) == '\0')
					return END;
				*strval = state->buf;
				*lenval = strlen(state->buf);
				state->buf += strlen(state->buf);
				state->count++;
				return VAL;
			default:
				return ERR;
				break;
		}
		state->buf += pg_mblen(state->buf);
	}
	return END;
}

/*
 * push new one in polish notation reverse view
 */
void
pushquery(TSQueryParserState * state, int4 type, int4 val, int4 distance, int4 lenval, int2 weight)
{
	ParseQueryNode *tmp = (ParseQueryNode *) palloc(sizeof(ParseQueryNode));

	tmp->weight = weight;
	tmp->type = type;
	tmp->val = val;
	if (distance >= MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("value is too big in tsearch query: \"%s\"",
						state->buffer)));
	if (lenval >= MAXSTRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("operand is too long in tsearch query: \"%s\"",
						state->buffer)));
	tmp->distance = distance;
	tmp->length = lenval;
	tmp->next = state->str;
	state->str = tmp;
	state->num++;
}

/*
 * This function is used for tsquery parsing
 */
void
pushval_asis(TSQueryParserState * state, int type, char *strval, int lenval, int2 weight)
{
	pg_crc32	c;

	if (lenval >= MAXSTRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("word is too long in tsearch query: \"%s\"",
						state->buffer)));

	INIT_CRC32(c);
	COMP_CRC32(c, strval, lenval);
	FIN_CRC32(c);
	pushquery(state, type, *(int4 *) &c,
			  state->curop - state->op, lenval, weight);

	while (state->curop - state->op + lenval + 1 >= state->lenop)
	{
		int4		tmp = state->curop - state->op;

		state->lenop *= 2;
		state->op = (char *) repalloc((void *) state->op, state->lenop);
		state->curop = state->op + tmp;
	}
	memcpy((void *) state->curop, (void *) strval, lenval);
	state->curop += lenval;
	*(state->curop) = '\0';
	state->curop++;
	state->sumlen += lenval + 1 /* \0 */ ;
	return;
}

#define STACKDEPTH	32

/*
 * make polish notation of query
 */
static int4
makepol(TSQueryParserState * state,
		void (*pushval) (TSQueryParserState *, int, char *, int, int2))
{
	int4		val = 0,
				type;
	int4		lenval = 0;
	char	   *strval = NULL;
	int4		stack[STACKDEPTH];
	int4		lenstack = 0;
	int2		weight = 0;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	while ((type = gettoken_query(state, &val, &lenval, &strval, &weight)) != END)
	{
		switch (type)
		{
			case VAL:
				pushval(state, VAL, strval, lenval, weight);
				while (lenstack && (stack[lenstack - 1] == (int4) '&' ||
									stack[lenstack - 1] == (int4) '!'))
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack], 0, 0, 0);
				}
				break;
			case OPR:
				if (lenstack && val == (int4) '|')
					pushquery(state, OPR, val, 0, 0, 0);
				else
				{
					if (lenstack == STACKDEPTH)			/* internal error */
						elog(ERROR, "tsquery stack too small");
					stack[lenstack] = val;
					lenstack++;
				}
				break;
			case OPEN:
				if (makepol(state, pushval) == ERR)
					return ERR;
				if (lenstack && (stack[lenstack - 1] == (int4) '&' ||
								 stack[lenstack - 1] == (int4) '!'))
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack], 0, 0, 0);
				}
				break;
			case CLOSE:
				while (lenstack)
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack], 0, 0, 0);
				};
				return END;
				break;
			case ERR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsearch query: \"%s\"",
								state->buffer)));
				return ERR;

		}
	}
	while (lenstack)
	{
		lenstack--;
		pushquery(state, OPR, stack[lenstack], 0, 0, 0);
	};
	return END;
}

static void
findoprnd(QueryItem * ptr, int4 *pos)
{
	if (ptr[*pos].type == VAL || ptr[*pos].type == VALSTOP)
	{
		ptr[*pos].left = 0;
		(*pos)++;
	}
	else if (ptr[*pos].val == (int4) '!')
	{
		ptr[*pos].left = 1;
		(*pos)++;
		findoprnd(ptr, pos);
	}
	else
	{
		QueryItem  *curitem = &ptr[*pos];
		int4		tmp = *pos;

		(*pos)++;
		findoprnd(ptr, pos);
		curitem->left = *pos - tmp;
		findoprnd(ptr, pos);
	}
}


/*
 * input
 */
TSQuery
parse_tsquery(char *buf, void (*pushval) (TSQueryParserState *, int, char *, int, int2), Oid cfg_id, bool isplain)
{
	TSQueryParserState state;
	int4		i;
	TSQuery		query;
	int4		commonlen;
	QueryItem  *ptr;
	ParseQueryNode *tmp;
	int4		pos = 0;

	/* init state */
	state.buffer = buf;
	state.buf = buf;
	state.state = (isplain) ? WAITSINGLEOPERAND : WAITFIRSTOPERAND;
	state.count = 0;
	state.num = 0;
	state.str = NULL;
	state.cfg_id = cfg_id;

	/* init value parser's state */
	state.valstate.oprisdelim = true;
	state.valstate.len = 32;
	state.valstate.word = (char *) palloc(state.valstate.len);

	/* init list of operand */
	state.sumlen = 0;
	state.lenop = 64;
	state.curop = state.op = (char *) palloc(state.lenop);
	*(state.curop) = '\0';

	/* parse query & make polish notation (postfix, but in reverse order) */
	makepol(&state, pushval);
	pfree(state.valstate.word);
	if (!state.num)
	{
		ereport(NOTICE,
				(errmsg("tsearch query doesn't contain lexeme(s): \"%s\"",
						state.buffer)));
		query = (TSQuery) palloc(HDRSIZETQ);
		SET_VARSIZE(query, HDRSIZETQ);
		query->size = 0;
		return query;
	}

	/* make finish struct */
	commonlen = COMPUTESIZE(state.num, state.sumlen);
	query = (TSQuery) palloc(commonlen);
	SET_VARSIZE(query, commonlen);
	query->size = state.num;
	ptr = GETQUERY(query);

	/* set item in polish notation */
	for (i = 0; i < state.num; i++)
	{
		ptr[i].weight = state.str->weight;
		ptr[i].type = state.str->type;
		ptr[i].val = state.str->val;
		ptr[i].distance = state.str->distance;
		ptr[i].length = state.str->length;
		tmp = state.str->next;
		pfree(state.str);
		state.str = tmp;
	}

	/* set user friendly-operand view */
	memcpy((void *) GETOPERAND(query), (void *) state.op, state.sumlen);
	pfree(state.op);

	/* set left operand's position for every operator */
	pos = 0;
	findoprnd(ptr, &pos);

	return query;
}

/*
 * in without morphology
 */
Datum
tsqueryin(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);

	pg_verifymbstr(in, strlen(in), false);

	PG_RETURN_TSQUERY(parse_tsquery(in, pushval_asis, InvalidOid, false));
}

/*
 * out function
 */
typedef struct
{
	QueryItem  *curpol;
	char	   *buf;
	char	   *cur;
	char	   *op;
	int4		buflen;
} INFIX;

#define RESIZEBUF(inf,addsize) \
while( ( (inf)->cur - (inf)->buf ) + (addsize) + 1 >= (inf)->buflen ) \
{ \
	int4 len = (inf)->cur - (inf)->buf; \
	(inf)->buflen *= 2; \
	(inf)->buf = (char*) repalloc( (void*)(inf)->buf, (inf)->buflen ); \
	(inf)->cur = (inf)->buf + len; \
}

/*
 * recursive walk on tree and print it in
 * infix (human-readable) view
 */
static void
infix(INFIX * in, bool first)
{
	if (in->curpol->type == VAL)
	{
		char	   *op = in->op + in->curpol->distance;
		int			clen;

		RESIZEBUF(in, in->curpol->length * (pg_database_encoding_max_length() + 1) + 2 + 5);
		*(in->cur) = '\'';
		in->cur++;
		while (*op)
		{
			if (t_iseq(op, '\''))
			{
				*(in->cur) = '\'';
				in->cur++;
			}
			COPYCHAR(in->cur, op);

			clen = pg_mblen(op);
			op += clen;
			in->cur += clen;
		}
		*(in->cur) = '\'';
		in->cur++;
		if (in->curpol->weight)
		{
			*(in->cur) = ':';
			in->cur++;
			if (in->curpol->weight & (1 << 3))
			{
				*(in->cur) = 'A';
				in->cur++;
			}
			if (in->curpol->weight & (1 << 2))
			{
				*(in->cur) = 'B';
				in->cur++;
			}
			if (in->curpol->weight & (1 << 1))
			{
				*(in->cur) = 'C';
				in->cur++;
			}
			if (in->curpol->weight & 1)
			{
				*(in->cur) = 'D';
				in->cur++;
			}
		}
		*(in->cur) = '\0';
		in->curpol++;
	}
	else if (in->curpol->val == (int4) '!')
	{
		bool		isopr = false;

		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol++;
		if (in->curpol->type == OPR)
		{
			isopr = true;
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}
		infix(in, isopr);
		if (isopr)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
	else
	{
		int4		op = in->curpol->val;
		INFIX		nrm;

		in->curpol++;
		if (op == (int4) '|' && !first)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}

		nrm.curpol = in->curpol;
		nrm.op = in->op;
		nrm.buflen = 16;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);

		/* get right operand */
		infix(&nrm, false);

		/* get & print left operand */
		in->curpol = nrm.curpol;
		infix(in, false);

		/* print operator & right operand */
		RESIZEBUF(in, 3 + (nrm.cur - nrm.buf));
		sprintf(in->cur, " %c %s", op, nrm.buf);
		in->cur = strchr(in->cur, '\0');
		pfree(nrm.buf);

		if (op == (int4) '|' && !first)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
}


Datum
tsqueryout(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	INFIX		nrm;

	if (query->size == 0)
	{
		char	   *b = palloc(1);

		*b = '\0';
		PG_RETURN_POINTER(b);
	}
	nrm.curpol = GETQUERY(query);
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
	*(nrm.cur) = '\0';
	nrm.op = GETOPERAND(query);
	infix(&nrm, true);

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_CSTRING(nrm.buf);
}

Datum
tsquerysend(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	StringInfoData buf;
	int			i;
	QueryItem  *item = GETQUERY(query);

	pq_begintypsend(&buf);

	pq_sendint(&buf, query->size, sizeof(int32));
	for (i = 0; i < query->size; i++)
	{
		int			tmp;

		pq_sendint(&buf, item->type, sizeof(item->type));
		pq_sendint(&buf, item->weight, sizeof(item->weight));
		pq_sendint(&buf, item->left, sizeof(item->left));
		pq_sendint(&buf, item->val, sizeof(item->val));

		/*
		 * We are sure that sizeof(WordEntry) == sizeof(int32), and about
		 * layout of QueryItem
		 */
		tmp = *(int32 *) (((char *) item) + HDRSIZEQI);
		pq_sendint(&buf, tmp, sizeof(tmp));

		item++;
	}

	item = GETQUERY(query);
	for (i = 0; i < query->size; i++)
	{
		if (item->type == VAL)
			pq_sendbytes(&buf, GETOPERAND(query) + item->distance, item->length);
		item++;
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tsqueryrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TSQuery		query;
	int			i,
				size,
				tmp,
				len = HDRSIZETQ;
	QueryItem  *item;
	int			datalen = 0;
	char	   *ptr;

	size = pq_getmsgint(buf, sizeof(uint32));
	if (size < 0 || size > (MaxAllocSize / sizeof(QueryItem)))
		elog(ERROR, "invalid size of tsquery");
	len += sizeof(QueryItem) * size;

	query = (TSQuery) palloc(len);
	query->size = size;
	item = GETQUERY(query);

	for (i = 0; i < size; i++)
	{
		item->type = (int8) pq_getmsgint(buf, sizeof(int8));
		item->weight = (int8) pq_getmsgint(buf, sizeof(int8));
		item->left = (int16) pq_getmsgint(buf, sizeof(int16));
		item->val = (int32) pq_getmsgint(buf, sizeof(int32));
		tmp = pq_getmsgint(buf, sizeof(int32));
		memcpy((((char *) item) + HDRSIZEQI), &tmp, sizeof(int32));

		/*
		 * Sanity checks
		 */
		if (item->type == VAL)
		{
			datalen += item->length + 1;		/* \0 */
		}
		else if (item->type == OPR)
		{
			if (item->val == '|' || item->val == '&')
			{
				if (item->left <= 0 || i + item->left >= size)
					elog(ERROR, "invalid pointer to left operand");
			}

			if (i == size - 1)
				elog(ERROR, "invalid pointer to right operand");
		}
		else
			elog(ERROR, "unknown tsquery node type");

		item++;
	}

	query = (TSQuery) repalloc(query, len + datalen);

	item = GETQUERY(query);
	ptr = GETOPERAND(query);
	for (i = 0; i < size; i++)
	{
		if (item->type == VAL)
		{
			item->distance = ptr - GETOPERAND(query);
			memcpy(ptr,
				   pq_getmsgbytes(buf, item->length),
				   item->length);
			ptr += item->length;
			*ptr++ = '\0';
		}
		item++;
	}

	Assert(ptr - GETOPERAND(query) == datalen);

	SET_VARSIZE(query, len + datalen);

	PG_RETURN_TSVECTOR(query);
}

/*
 * debug function, used only for view query
 * which will be executed in non-leaf pages in index
 */
Datum
tsquerytree(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	INFIX		nrm;
	text	   *res;
	QueryItem  *q;
	int4		len;

	if (query->size == 0)
	{
		res = (text *) palloc(VARHDRSZ);
		SET_VARSIZE(res, VARHDRSZ);
		PG_RETURN_POINTER(res);
	}

	q = clean_NOT(GETQUERY(query), &len);

	if (!q)
	{
		res = (text *) palloc(1 + VARHDRSZ);
		SET_VARSIZE(res, 1 + VARHDRSZ);
		*((char *) VARDATA(res)) = 'T';
	}
	else
	{
		nrm.curpol = q;
		nrm.buflen = 32;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
		*(nrm.cur) = '\0';
		nrm.op = GETOPERAND(query);
		infix(&nrm, true);

		res = (text *) palloc(nrm.cur - nrm.buf + VARHDRSZ);
		SET_VARSIZE(res, nrm.cur - nrm.buf + VARHDRSZ);
		strncpy(VARDATA(res), nrm.buf, nrm.cur - nrm.buf);
		pfree(q);
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_POINTER(res);
}
