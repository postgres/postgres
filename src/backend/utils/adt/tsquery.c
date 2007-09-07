/*-------------------------------------------------------------------------
 *
 * tsquery.c
 *	  I/O functions for tsquery
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsquery.c,v 1.3 2007/09/07 15:09:56 teodor Exp $
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


struct TSQueryParserStateData
{
	/* State for gettoken_query */
	char	   *buffer;			/* entire string we are scanning */
	char	   *buf;			/* current scan point */
	int			state;
	int			count;			/* nesting count, incremented by (, 
								   decremented by ) */

	/* polish (prefix) notation in list, filled in by push* functions */
	List	   *polstr;

	/* Strings from operands are collected in op. curop is a pointer to
	 * the end of used space of op. */
	char	   *op;
	char	   *curop;
	int			lenop; /* allocated size of op */
	int			sumlen; /* used size of op */

	/* state for value's parser */
	TSVectorParseState valstate;
};

/* parser's states */
#define WAITOPERAND 1
#define WAITOPERATOR	2
#define WAITFIRSTOPERAND 3
#define WAITSINGLEOPERAND 4

/*
 * subroutine to parse the weight part, like ':1AB' of a query.
 */
static char *
get_weight(char *buf, int16 *weight)
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
 * token types for parsing
 */
typedef enum {
	PT_END = 0,
	PT_ERR = 1,
	PT_VAL = 2,
	PT_OPR = 3,
	PT_OPEN = 4,
	PT_CLOSE = 5,
} ts_tokentype;

/*
 * get token from query string
 *
 * *operator is filled in with OP_* when return values is PT_OPR
 * *strval, *lenval and *weight are filled in when return value is PT_VAL
 */
static ts_tokentype
gettoken_query(TSQueryParserState state, 
			   int8 *operator,
			   int *lenval, char **strval, int16 *weight)
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
					*operator = OP_NOT;
					state->state = WAITOPERAND;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '('))
				{
					state->count++;
					(state->buf)++;
					state->state = WAITOPERAND;
					return PT_OPEN;
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
					/* We rely on the tsvector parser to parse the value for us */
					reset_tsvector_parser(state->valstate, state->buf);
					if (gettoken_tsvector(state->valstate, strval, lenval, NULL, NULL, &state->buf))
					{
						state->buf = get_weight(state->buf, weight);
						state->state = WAITOPERATOR;
						return PT_VAL;
					}
					else if (state->state == WAITFIRSTOPERAND)
						return PT_END;
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("no operand in tsearch query: \"%s\"",
										state->buffer)));
				}
				break;
			case WAITOPERATOR:
				if (t_iseq(state->buf, '&'))
				{
					state->state = WAITOPERAND;
					*operator = OP_AND;
					(state->buf)++;
					return PT_OPR;
				}
				if (t_iseq(state->buf, '|'))
				{
					state->state = WAITOPERAND;
					*operator = OP_OR;
					(state->buf)++;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, ')'))
				{
					(state->buf)++;
					state->count--;
					return (state->count < 0) ? PT_ERR : PT_CLOSE;
				}
				else if (*(state->buf) == '\0')
					return (state->count) ? PT_ERR : PT_END;
				else if (!t_isspace(state->buf))
					return PT_ERR;
				break;
			case WAITSINGLEOPERAND:
				if (*(state->buf) == '\0')
					return PT_END;
				*strval = state->buf;
				*lenval = strlen(state->buf);
				state->buf += strlen(state->buf);
				state->count++;
				return PT_VAL;
			default:
				return PT_ERR;
				break;
		}
		state->buf += pg_mblen(state->buf);
	}
	return PT_END;
}

/*
 * Push an operator to state->polstr
 */
void
pushOperator(TSQueryParserState state, int8 oper)
{
	QueryOperator *tmp;

	Assert(oper == OP_NOT || oper == OP_AND || oper == OP_OR);
	
	tmp = (QueryOperator *) palloc(sizeof(QueryOperator));
	tmp->type = QI_OPR;
	tmp->oper = oper;
	/* left is filled in later with findoprnd */

	state->polstr = lcons(tmp, state->polstr);
}

static void
pushValue_internal(TSQueryParserState state, pg_crc32 valcrc, int distance, int lenval, int weight)
{
	QueryOperand *tmp;

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

	tmp = (QueryOperand *) palloc(sizeof(QueryOperand));
	tmp->type = QI_VAL;
	tmp->weight = weight;
	tmp->valcrc = (int32) valcrc;
	tmp->length = lenval;
	tmp->distance = distance;

	state->polstr = lcons(tmp, state->polstr);
}

/*
 * Push an operand to state->polstr.
 *
 * strval must point to a string equal to state->curop. lenval is the length
 * of the string.
 */
void
pushValue(TSQueryParserState state, char *strval, int lenval, int2 weight)
{
	pg_crc32	valcrc;

	if (lenval >= MAXSTRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("word is too long in tsearch query: \"%s\"",
						state->buffer)));

	INIT_CRC32(valcrc);
	COMP_CRC32(valcrc, strval, lenval);
	FIN_CRC32(valcrc);
	pushValue_internal(state, valcrc, state->curop - state->op, lenval, weight);

	/* append the value string to state.op, enlarging buffer if needed first */
	while (state->curop - state->op + lenval + 1 >= state->lenop)
	{
		int	used = state->curop - state->op;

		state->lenop *= 2;
		state->op = (char *) repalloc((void *) state->op, state->lenop);
		state->curop = state->op + used;
	}
	memcpy((void *) state->curop, (void *) strval, lenval);
	state->curop += lenval;
	*(state->curop) = '\0';
	state->curop++;
	state->sumlen += lenval + 1 /* \0 */ ;
}


/*
 * Push a stopword placeholder to state->polstr
 */
void
pushStop(TSQueryParserState state)
{
	QueryOperand *tmp;

	tmp = (QueryOperand *) palloc(sizeof(QueryOperand));
	tmp->type = QI_VALSTOP;

	state->polstr = lcons(tmp, state->polstr);
}


#define STACKDEPTH	32

/*
 * Make polish (prefix) notation of query.
 *
 * See parse_tsquery for explanation of pushval.
 */
static void
makepol(TSQueryParserState state, 
		PushFunction pushval,
		void *opaque)
{
	int8		operator = 0;
	ts_tokentype type;
	int			lenval = 0;
	char	   *strval = NULL;
	int8		opstack[STACKDEPTH];
	int			lenstack = 0;
	int16		weight = 0;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	while ((type = gettoken_query(state, &operator, &lenval, &strval, &weight)) != PT_END)
	{
		switch (type)
		{
			case PT_VAL:
				pushval(opaque, state, strval, lenval, weight);
				while (lenstack && (opstack[lenstack - 1] == OP_AND ||
									opstack[lenstack - 1] == OP_NOT))
				{
					lenstack--;
					pushOperator(state, opstack[lenstack]);
				}
				break;
			case PT_OPR:
				if (lenstack && operator == OP_OR)
					pushOperator(state, OP_OR);
				else
				{
					if (lenstack == STACKDEPTH)			/* internal error */
						elog(ERROR, "tsquery stack too small");
					opstack[lenstack] = operator;
					lenstack++;
				}
				break;
			case PT_OPEN:
				makepol(state, pushval, opaque);

				if (lenstack && (opstack[lenstack - 1] == OP_AND ||
								 opstack[lenstack - 1] == OP_NOT))
				{
					lenstack--;
					pushOperator(state, opstack[lenstack]);
				}
				break;
			case PT_CLOSE:
				while (lenstack)
				{
					lenstack--;
					pushOperator(state, opstack[lenstack]);
				};
				return;
			case PT_ERR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsearch query: \"%s\"",
								state->buffer)));
		}
	}
	while (lenstack)
	{
		lenstack--;
		pushOperator(state, opstack[lenstack]);
	}
}

/*
 * Fills in the left-fields previously left unfilled. The input
 * QueryItems must be in polish (prefix) notation. 
 */
static void
findoprnd(QueryItem *ptr, int *pos)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (ptr[*pos].type == QI_VAL ||
		ptr[*pos].type == QI_VALSTOP) /* need to handle VALSTOP here,
									   * they haven't been cleansed
									   * away yet.
									   */
	{
		(*pos)++;
	}
	else 
	{
		Assert(ptr[*pos].type == QI_OPR);

		if (ptr[*pos].operator.oper == OP_NOT)
		{
			ptr[*pos].operator.left = 1;
			(*pos)++;
			findoprnd(ptr, pos);
		}
		else
		{
			QueryOperator  *curitem = &ptr[*pos].operator;
			int	tmp = *pos;

			Assert(curitem->oper == OP_AND || curitem->oper == OP_OR);

			(*pos)++;
			findoprnd(ptr, pos);
			curitem->left = *pos - tmp;
			findoprnd(ptr, pos);
		}
	}
}

/*
 * Each value (operand) in the query is be passed to pushval. pushval can
 * transform the simple value to an arbitrarily complex expression using
 * pushValue and pushOperator. It must push a single value with pushValue,
 * a complete expression with all operands, or a a stopword placeholder
 * with pushStop, otherwise the prefix notation representation will be broken,
 * having an operator with no operand.
 *
 * opaque is passed on to pushval as is, pushval can use it to store its 
 * private state.
 *
 * The returned query might contain QI_STOPVAL nodes. The caller is responsible
 * for cleaning them up (with clean_fakeval)
 */
TSQuery
parse_tsquery(char *buf, 
			  PushFunction pushval,
			  void *opaque,
			  bool isplain)
{
	struct TSQueryParserStateData state;
	int			i;
	TSQuery		query;
	int			commonlen;
	QueryItem  *ptr;
	int			pos = 0;
	ListCell   *cell;

	/* init state */
	state.buffer = buf;
	state.buf = buf;
	state.state = (isplain) ? WAITSINGLEOPERAND : WAITFIRSTOPERAND;
	state.count = 0;
	state.polstr = NIL;

	/* init value parser's state */
	state.valstate = init_tsvector_parser(NULL, true);

	/* init list of operand */
	state.sumlen = 0;
	state.lenop = 64;
	state.curop = state.op = (char *) palloc(state.lenop);
	*(state.curop) = '\0';

	/* parse query & make polish notation (postfix, but in reverse order) */
	makepol(&state, pushval, opaque);

	close_tsvector_parser(state.valstate);

	if (list_length(state.polstr) == 0)
	{
		ereport(NOTICE,
				(errmsg("tsearch query doesn't contain lexeme(s): \"%s\"",
						state.buffer)));
		query = (TSQuery) palloc(HDRSIZETQ);
		SET_VARSIZE(query, HDRSIZETQ);
		query->size = 0;
		return query;
	}

	/* Pack the QueryItems in the final TSQuery struct to return to caller */
	commonlen = COMPUTESIZE(list_length(state.polstr), state.sumlen);
	query = (TSQuery) palloc0(commonlen);
	SET_VARSIZE(query, commonlen);
	query->size = list_length(state.polstr);
	ptr = GETQUERY(query);

	/* Copy QueryItems to TSQuery */
	i = 0;
	foreach(cell, state.polstr)
	{
		QueryItem *item = (QueryItem *) lfirst(cell);

		switch(item->type)
		{
			case QI_VAL:
				memcpy(&ptr[i], item, sizeof(QueryOperand));
				break;
			case QI_VALSTOP:
				ptr[i].type = QI_VALSTOP;
				break;
			case QI_OPR:
				memcpy(&ptr[i], item, sizeof(QueryOperator));
				break;
			default:
				elog(ERROR, "unknown QueryItem type %d", item->type);
		}
		i++;
	}

	/* Copy all the operand strings to TSQuery */
	memcpy((void *) GETOPERAND(query), (void *) state.op, state.sumlen);
	pfree(state.op);

	/* Set left operand pointers for every operator. */
	pos = 0;
	findoprnd(ptr, &pos);

	return query;
}

static void
pushval_asis(void *opaque, TSQueryParserState state, char *strval, int lenval,
			 int16 weight)
{
	pushValue(state, strval, lenval, weight);
}

/*
 * in without morphology
 */
Datum
tsqueryin(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);

	pg_verifymbstr(in, strlen(in), false);

	PG_RETURN_TSQUERY(parse_tsquery(in, pushval_asis, NULL, false));
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
	int			buflen;
} INFIX;

/* Makes sure inf->buf is large enough for adding 'addsize' bytes */
#define RESIZEBUF(inf, addsize) \
while( ( (inf)->cur - (inf)->buf ) + (addsize) + 1 >= (inf)->buflen ) \
{ \
	int len = (inf)->cur - (inf)->buf; \
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
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (in->curpol->type == QI_VAL)
	{
		QueryOperand *curpol = &in->curpol->operand;
		char	   *op = in->op + curpol->distance;
		int			clen;

		RESIZEBUF(in, curpol->length * (pg_database_encoding_max_length() + 1) + 2 + 5);
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
		if (curpol->weight)
		{
			*(in->cur) = ':';
			in->cur++;
			if (curpol->weight & (1 << 3))
			{
				*(in->cur) = 'A';
				in->cur++;
			}
			if (curpol->weight & (1 << 2))
			{
				*(in->cur) = 'B';
				in->cur++;
			}
			if (curpol->weight & (1 << 1))
			{
				*(in->cur) = 'C';
				in->cur++;
			}
			if (curpol->weight & 1)
			{
				*(in->cur) = 'D';
				in->cur++;
			}
		}
		*(in->cur) = '\0';
		in->curpol++;
	}
	else if (in->curpol->operator.oper == OP_NOT)
	{
		bool		isopr = false;

		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol++;

		if (in->curpol->type == QI_OPR)
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
		int8		op = in->curpol->operator.oper;
		INFIX		nrm;

		in->curpol++;
		if (op == OP_OR && !first)
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
		switch(op)
		{
			case OP_OR:
				sprintf(in->cur, " | %s", nrm.buf);
				break;
			case OP_AND:
				sprintf(in->cur, " & %s", nrm.buf);
				break;
			default:
				/* OP_NOT is handled in above if-branch*/
				elog(ERROR, "unexpected operator type %d", op);
		}
		in->cur = strchr(in->cur, '\0');
		pfree(nrm.buf);

		if (op == OP_OR && !first)
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
		pq_sendint(&buf, item->type, sizeof(item->type));

		switch(item->type)
		{
			case QI_VAL:
				pq_sendint(&buf, item->operand.weight, sizeof(item->operand.weight));
				pq_sendint(&buf, item->operand.valcrc, sizeof(item->operand.valcrc));
				pq_sendint(&buf, item->operand.length, sizeof(int16));
				/* istrue flag is just for temporary use in tsrank.c/Cover,
				 * so we don't need to transfer that */
				break;
			case QI_OPR:
				pq_sendint(&buf, item->operator.oper, sizeof(item->operator.oper));
				if (item->operator.oper != OP_NOT)
					pq_sendint(&buf, item->operator.left, sizeof(item->operator.left));
				break;
			default:
				elog(ERROR, "unknown tsquery node type %d", item->type);
		}
		item++;
	}

	item = GETQUERY(query);
	for (i = 0; i < query->size; i++)
	{
		if (item->type == QI_VAL)
			pq_sendbytes(&buf, GETOPERAND(query) + item->operand.distance, item->operand.length);
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
				len;
	QueryItem  *item;
	int			datalen = 0;
	char	   *ptr;

	size = pq_getmsgint(buf, sizeof(uint32));
	if (size < 0 || size > (MaxAllocSize / sizeof(QueryItem)))
		elog(ERROR, "invalid size of tsquery");

	len = HDRSIZETQ + sizeof(QueryItem) * size;

	query = (TSQuery) palloc(len);
	query->size = size;
	item = GETQUERY(query);

	for (i = 0; i < size; i++)
	{
		item->type = (int8) pq_getmsgint(buf, sizeof(int8));

		switch(item->type)
		{
			case QI_VAL:
				item->operand.weight = (int8) pq_getmsgint(buf, sizeof(int8));
				item->operand.valcrc = (int32) pq_getmsgint(buf, sizeof(int32));
				item->operand.length = pq_getmsgint(buf, sizeof(int16));

				/*
				 * Check that datalen doesn't grow too large. Without the
				 * check, a malicious client could induce a buffer overflow
				 * by sending a tsquery whose size exceeds 2GB. datalen
				 * would overflow, we would allocate a too small buffer below,
				 * and overflow the buffer. Because operand.length is a 20-bit
				 * field, adding one such value to datalen must exceed
				 * MaxAllocSize before wrapping over the 32-bit datalen field,
				 * so this check will protect from it.
				 */
				if (datalen > MAXSTRLEN)
					elog(ERROR, "invalid tsquery; total operand length exceeded");

				/* We can calculate distance from datalen, no need to send it
				 * through the wire. If we did, we would have to check that
				 * it's valid anyway.
				 */
				item->operand.distance = datalen;

				datalen += item->operand.length + 1;		/* \0 */

				break;
			case QI_OPR:
				item->operator.oper = (int8) pq_getmsgint(buf, sizeof(int8));
				if (item->operator.oper != OP_NOT &&
					item->operator.oper != OP_OR &&
					item->operator.oper != OP_AND)
					elog(ERROR, "unknown operator type %d", (int) item->operator.oper);
				if(item->operator.oper != OP_NOT)
				{
					item->operator.left = (int16) pq_getmsgint(buf, sizeof(int16));
					/*
					 * Sanity checks
					 */
					if (item->operator.left <= 0 || i + item->operator.left >= size)
						elog(ERROR, "invalid pointer to left operand");

					/* XXX: Though there's no way to construct a TSQuery that's
					 * not in polish notation, we don't enforce that for
					 * queries received from client in binary mode. Is there
					 * anything that relies on it?
					 *
					 * XXX: The tree could be malformed in other ways too,
					 * a node could have two parents, for example.
					 */
				}

				if (i == size - 1)
					elog(ERROR, "invalid pointer to right operand");
				break;
			default:
				elog(ERROR, "unknown tsquery node type %d", item->type);
		}

		item++;
	}

	query = (TSQuery) repalloc(query, len + datalen);

	item = GETQUERY(query);
	ptr = GETOPERAND(query);
	for (i = 0; i < size; i++)
	{
		if (item->type == QI_VAL)
		{
			memcpy(ptr,
				   pq_getmsgbytes(buf, item->operand.length),
				   item->operand.length);
			ptr += item->operand.length;
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
	int			len;

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
