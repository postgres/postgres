/*-------------------------------------------------------------------------
 *
 * tsquery.c
 *	  I/O functions for tsquery
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"
#include "varatt.h"

/* FTS operator priorities, see ts_type.h */
const int	tsearch_op_priority[OP_COUNT] =
{
	4,							/* OP_NOT */
	2,							/* OP_AND */
	1,							/* OP_OR */
	3							/* OP_PHRASE */
};

/*
 * parser's states
 */
typedef enum
{
	WAITOPERAND = 1,
	WAITOPERATOR = 2,
	WAITFIRSTOPERAND = 3,
} ts_parserstate;

/*
 * token types for parsing
 */
typedef enum
{
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
 * All arguments except "state" are output arguments.
 *
 * If return value is PT_OPR, then *operator is filled with an OP_* code
 * and *weight will contain a distance value in case of phrase operator.
 *
 * If return value is PT_VAL, then *lenval, *strval, *weight, and *prefix
 * are filled.
 *
 * If PT_ERR is returned then a soft error has occurred.  If state->escontext
 * isn't already filled then this should be reported as a generic parse error.
 */
typedef ts_tokentype (*ts_tokenizer) (TSQueryParserState state, int8 *operator,
									  int *lenval, char **strval,
									  int16 *weight, bool *prefix);

struct TSQueryParserStateData
{
	/* Tokenizer used for parsing tsquery */
	ts_tokenizer gettoken;

	/* State of tokenizer function */
	char	   *buffer;			/* entire string we are scanning */
	char	   *buf;			/* current scan point */
	int			count;			/* nesting count, incremented by (,
								 * decremented by ) */
	ts_parserstate state;

	/* polish (prefix) notation in list, filled in by push* functions */
	List	   *polstr;

	/*
	 * Strings from operands are collected in op. curop is a pointer to the
	 * end of used space of op.
	 */
	char	   *op;
	char	   *curop;
	int			lenop;			/* allocated size of op */
	int			sumlen;			/* used size of op */

	/* state for value's parser */
	TSVectorParseState valstate;

	/* context object for soft errors - must match valstate's escontext */
	Node	   *escontext;
};

/*
 * subroutine to parse the modifiers (weight and prefix flag currently)
 * part, like ':AB*' of a query.
 */
static char *
get_modifiers(char *buf, int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

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
			case '*':
				*prefix = true;
				break;
			default:
				return buf;
		}
		buf++;
	}

	return buf;
}

/*
 * Parse phrase operator. The operator
 * may take the following forms:
 *
 *		a <N> b (distance is exactly N lexemes)
 *		a <-> b (default distance = 1)
 *
 * The buffer should begin with '<' char
 */
static bool
parse_phrase_operator(TSQueryParserState pstate, int16 *distance)
{
	enum
	{
		PHRASE_OPEN = 0,
		PHRASE_DIST,
		PHRASE_CLOSE,
		PHRASE_FINISH
	}			state = PHRASE_OPEN;
	char	   *ptr = pstate->buf;
	char	   *endptr;
	long		l = 1;			/* default distance */

	while (*ptr)
	{
		switch (state)
		{
			case PHRASE_OPEN:
				if (t_iseq(ptr, '<'))
				{
					state = PHRASE_DIST;
					ptr++;
				}
				else
					return false;
				break;

			case PHRASE_DIST:
				if (t_iseq(ptr, '-'))
				{
					state = PHRASE_CLOSE;
					ptr++;
					continue;
				}

				if (!isdigit((unsigned char) *ptr))
					return false;

				errno = 0;
				l = strtol(ptr, &endptr, 10);
				if (ptr == endptr)
					return false;
				else if (errno == ERANGE || l < 0 || l > MAXENTRYPOS)
					ereturn(pstate->escontext, false,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("distance in phrase operator must be an integer value between zero and %d inclusive",
									MAXENTRYPOS)));
				else
				{
					state = PHRASE_CLOSE;
					ptr = endptr;
				}
				break;

			case PHRASE_CLOSE:
				if (t_iseq(ptr, '>'))
				{
					state = PHRASE_FINISH;
					ptr++;
				}
				else
					return false;
				break;

			case PHRASE_FINISH:
				*distance = (int16) l;
				pstate->buf = ptr;
				return true;
		}
	}

	return false;
}

/*
 * Parse OR operator used in websearch_to_tsquery(), returns true if we
 * believe that "OR" literal could be an operator OR
 */
static bool
parse_or_operator(TSQueryParserState pstate)
{
	char	   *ptr = pstate->buf;

	/* it should begin with "OR" literal */
	if (pg_strncasecmp(ptr, "or", 2) != 0)
		return false;

	ptr += 2;

	/*
	 * it shouldn't be a part of any word but somewhere later it should be
	 * some operand
	 */
	if (*ptr == '\0')			/* no operand */
		return false;

	/* it shouldn't be a part of any word */
	if (t_iseq(ptr, '-') || t_iseq(ptr, '_') || t_isalnum(ptr))
		return false;

	for (;;)
	{
		ptr += pg_mblen(ptr);

		if (*ptr == '\0')		/* got end of string without operand */
			return false;

		/*
		 * Suppose, we found an operand, but could be a not correct operand.
		 * So we still treat OR literal as operation with possibly incorrect
		 * operand and will not search it as lexeme
		 */
		if (!isspace((unsigned char) *ptr))
			break;
	}

	pstate->buf += 2;
	return true;
}

static ts_tokentype
gettoken_query_standard(TSQueryParserState state, int8 *operator,
						int *lenval, char **strval,
						int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	while (true)
	{
		switch (state->state)
		{
			case WAITFIRSTOPERAND:
			case WAITOPERAND:
				if (t_iseq(state->buf, '!'))
				{
					state->buf++;
					state->state = WAITOPERAND;
					*operator = OP_NOT;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '('))
				{
					state->buf++;
					state->state = WAITOPERAND;
					state->count++;
					return PT_OPEN;
				}
				else if (t_iseq(state->buf, ':'))
				{
					/* generic syntax error message is fine */
					return PT_ERR;
				}
				else if (!isspace((unsigned char) *state->buf))
				{
					/*
					 * We rely on the tsvector parser to parse the value for
					 * us
					 */
					reset_tsvector_parser(state->valstate, state->buf);
					if (gettoken_tsvector(state->valstate, strval, lenval,
										  NULL, NULL, &state->buf))
					{
						state->buf = get_modifiers(state->buf, weight, prefix);
						state->state = WAITOPERATOR;
						return PT_VAL;
					}
					else if (SOFT_ERROR_OCCURRED(state->escontext))
					{
						/* gettoken_tsvector reported a soft error */
						return PT_ERR;
					}
					else if (state->state == WAITFIRSTOPERAND)
					{
						return PT_END;
					}
					else
						ereturn(state->escontext, PT_ERR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("no operand in tsquery: \"%s\"",
										state->buffer)));
				}
				break;

			case WAITOPERATOR:
				if (t_iseq(state->buf, '&'))
				{
					state->buf++;
					state->state = WAITOPERAND;
					*operator = OP_AND;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '|'))
				{
					state->buf++;
					state->state = WAITOPERAND;
					*operator = OP_OR;
					return PT_OPR;
				}
				else if (parse_phrase_operator(state, weight))
				{
					/* weight var is used as storage for distance */
					state->state = WAITOPERAND;
					*operator = OP_PHRASE;
					return PT_OPR;
				}
				else if (SOFT_ERROR_OCCURRED(state->escontext))
				{
					/* parse_phrase_operator reported a soft error */
					return PT_ERR;
				}
				else if (t_iseq(state->buf, ')'))
				{
					state->buf++;
					state->count--;
					return (state->count < 0) ? PT_ERR : PT_CLOSE;
				}
				else if (*state->buf == '\0')
				{
					return (state->count) ? PT_ERR : PT_END;
				}
				else if (!isspace((unsigned char) *state->buf))
				{
					return PT_ERR;
				}
				break;
		}

		state->buf += pg_mblen(state->buf);
	}
}

static ts_tokentype
gettoken_query_websearch(TSQueryParserState state, int8 *operator,
						 int *lenval, char **strval,
						 int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	while (true)
	{
		switch (state->state)
		{
			case WAITFIRSTOPERAND:
			case WAITOPERAND:
				if (t_iseq(state->buf, '-'))
				{
					state->buf++;
					state->state = WAITOPERAND;

					*operator = OP_NOT;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '"'))
				{
					/* Everything in quotes is processed as a single token */

					/* skip opening quote */
					state->buf++;
					*strval = state->buf;

					/* iterate to the closing quote or end of the string */
					while (*state->buf != '\0' && !t_iseq(state->buf, '"'))
						state->buf++;
					*lenval = state->buf - *strval;

					/* skip closing quote if not end of the string */
					if (*state->buf != '\0')
						state->buf++;

					state->state = WAITOPERATOR;
					state->count++;
					return PT_VAL;
				}
				else if (ISOPERATOR(state->buf))
				{
					/* ignore, else gettoken_tsvector() will raise an error */
					state->buf++;
					state->state = WAITOPERAND;
					continue;
				}
				else if (!isspace((unsigned char) *state->buf))
				{
					/*
					 * We rely on the tsvector parser to parse the value for
					 * us
					 */
					reset_tsvector_parser(state->valstate, state->buf);
					if (gettoken_tsvector(state->valstate, strval, lenval,
										  NULL, NULL, &state->buf))
					{
						state->state = WAITOPERATOR;
						return PT_VAL;
					}
					else if (SOFT_ERROR_OCCURRED(state->escontext))
					{
						/* gettoken_tsvector reported a soft error */
						return PT_ERR;
					}
					else if (state->state == WAITFIRSTOPERAND)
					{
						return PT_END;
					}
					else
					{
						/* finally, we have to provide an operand */
						pushStop(state);
						return PT_END;
					}
				}
				break;

			case WAITOPERATOR:
				if (*state->buf == '\0')
				{
					return PT_END;
				}
				else if (parse_or_operator(state))
				{
					state->state = WAITOPERAND;
					*operator = OP_OR;
					return PT_OPR;
				}
				else if (ISOPERATOR(state->buf))
				{
					/* ignore other operators in this state too */
					state->buf++;
					continue;
				}
				else if (!isspace((unsigned char) *state->buf))
				{
					/* insert implicit AND between operands */
					state->state = WAITOPERAND;
					*operator = OP_AND;
					return PT_OPR;
				}
				break;
		}

		state->buf += pg_mblen(state->buf);
	}
}

static ts_tokentype
gettoken_query_plain(TSQueryParserState state, int8 *operator,
					 int *lenval, char **strval,
					 int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	if (*state->buf == '\0')
		return PT_END;

	*strval = state->buf;
	*lenval = strlen(state->buf);
	state->buf += *lenval;
	state->count++;
	return PT_VAL;
}

/*
 * Push an operator to state->polstr
 */
void
pushOperator(TSQueryParserState state, int8 oper, int16 distance)
{
	QueryOperator *tmp;

	Assert(oper == OP_NOT || oper == OP_AND || oper == OP_OR || oper == OP_PHRASE);

	tmp = (QueryOperator *) palloc0(sizeof(QueryOperator));
	tmp->type = QI_OPR;
	tmp->oper = oper;
	tmp->distance = (oper == OP_PHRASE) ? distance : 0;
	/* left is filled in later with findoprnd */

	state->polstr = lcons(tmp, state->polstr);
}

static void
pushValue_internal(TSQueryParserState state, pg_crc32 valcrc, int distance, int lenval, int weight, bool prefix)
{
	QueryOperand *tmp;

	if (distance >= MAXSTRPOS)
		ereturn(state->escontext,,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("value is too big in tsquery: \"%s\"",
						state->buffer)));
	if (lenval >= MAXSTRLEN)
		ereturn(state->escontext,,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("operand is too long in tsquery: \"%s\"",
						state->buffer)));

	tmp = (QueryOperand *) palloc0(sizeof(QueryOperand));
	tmp->type = QI_VAL;
	tmp->weight = weight;
	tmp->prefix = prefix;
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
pushValue(TSQueryParserState state, char *strval, int lenval, int16 weight, bool prefix)
{
	pg_crc32	valcrc;

	if (lenval >= MAXSTRLEN)
		ereturn(state->escontext,,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("word is too long in tsquery: \"%s\"",
						state->buffer)));

	INIT_LEGACY_CRC32(valcrc);
	COMP_LEGACY_CRC32(valcrc, strval, lenval);
	FIN_LEGACY_CRC32(valcrc);
	pushValue_internal(state, valcrc, state->curop - state->op, lenval, weight, prefix);

	/* append the value string to state.op, enlarging buffer if needed first */
	while (state->curop - state->op + lenval + 1 >= state->lenop)
	{
		int			used = state->curop - state->op;

		state->lenop *= 2;
		state->op = (char *) repalloc(state->op, state->lenop);
		state->curop = state->op + used;
	}
	memcpy(state->curop, strval, lenval);
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

	tmp = (QueryOperand *) palloc0(sizeof(QueryOperand));
	tmp->type = QI_VALSTOP;

	state->polstr = lcons(tmp, state->polstr);
}


#define STACKDEPTH	32

typedef struct OperatorElement
{
	int8		op;
	int16		distance;
} OperatorElement;

static void
pushOpStack(OperatorElement *stack, int *lenstack, int8 op, int16 distance)
{
	if (*lenstack == STACKDEPTH)	/* internal error */
		elog(ERROR, "tsquery stack too small");

	stack[*lenstack].op = op;
	stack[*lenstack].distance = distance;

	(*lenstack)++;
}

static void
cleanOpStack(TSQueryParserState state,
			 OperatorElement *stack, int *lenstack, int8 op)
{
	int			opPriority = OP_PRIORITY(op);

	while (*lenstack)
	{
		/* NOT is right associative unlike to others */
		if ((op != OP_NOT && opPriority > OP_PRIORITY(stack[*lenstack - 1].op)) ||
			(op == OP_NOT && opPriority >= OP_PRIORITY(stack[*lenstack - 1].op)))
			break;

		(*lenstack)--;
		pushOperator(state, stack[*lenstack].op,
					 stack[*lenstack].distance);
	}
}

/*
 * Make polish (prefix) notation of query.
 *
 * See parse_tsquery for explanation of pushval.
 */
static void
makepol(TSQueryParserState state,
		PushFunction pushval,
		Datum opaque)
{
	int8		operator = 0;
	ts_tokentype type;
	int			lenval = 0;
	char	   *strval = NULL;
	OperatorElement opstack[STACKDEPTH];
	int			lenstack = 0;
	int16		weight = 0;
	bool		prefix;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	while ((type = state->gettoken(state, &operator,
								   &lenval, &strval,
								   &weight, &prefix)) != PT_END)
	{
		switch (type)
		{
			case PT_VAL:
				pushval(opaque, state, strval, lenval, weight, prefix);
				break;
			case PT_OPR:
				cleanOpStack(state, opstack, &lenstack, operator);
				pushOpStack(opstack, &lenstack, operator, weight);
				break;
			case PT_OPEN:
				makepol(state, pushval, opaque);
				break;
			case PT_CLOSE:
				cleanOpStack(state, opstack, &lenstack, OP_OR /* lowest */ );
				return;
			case PT_ERR:
			default:
				/* don't overwrite a soft error saved by gettoken function */
				if (!SOFT_ERROR_OCCURRED(state->escontext))
					errsave(state->escontext,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsquery: \"%s\"",
									state->buffer)));
				return;
		}
		/* detect soft error in pushval or recursion */
		if (SOFT_ERROR_OCCURRED(state->escontext))
			return;
	}

	cleanOpStack(state, opstack, &lenstack, OP_OR /* lowest */ );
}

static void
findoprnd_recurse(QueryItem *ptr, uint32 *pos, int nnodes, bool *needcleanup)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (*pos >= nnodes)
		elog(ERROR, "malformed tsquery: operand not found");

	if (ptr[*pos].type == QI_VAL)
	{
		(*pos)++;
	}
	else if (ptr[*pos].type == QI_VALSTOP)
	{
		*needcleanup = true;	/* we'll have to remove stop words */
		(*pos)++;
	}
	else
	{
		Assert(ptr[*pos].type == QI_OPR);

		if (ptr[*pos].qoperator.oper == OP_NOT)
		{
			ptr[*pos].qoperator.left = 1;	/* fixed offset */
			(*pos)++;

			/* process the only argument */
			findoprnd_recurse(ptr, pos, nnodes, needcleanup);
		}
		else
		{
			QueryOperator *curitem = &ptr[*pos].qoperator;
			int			tmp = *pos; /* save current position */

			Assert(curitem->oper == OP_AND ||
				   curitem->oper == OP_OR ||
				   curitem->oper == OP_PHRASE);

			(*pos)++;

			/* process RIGHT argument */
			findoprnd_recurse(ptr, pos, nnodes, needcleanup);

			curitem->left = *pos - tmp; /* set LEFT arg's offset */

			/* process LEFT argument */
			findoprnd_recurse(ptr, pos, nnodes, needcleanup);
		}
	}
}


/*
 * Fill in the left-fields previously left unfilled.
 * The input QueryItems must be in polish (prefix) notation.
 * Also, set *needcleanup to true if there are any QI_VALSTOP nodes.
 */
static void
findoprnd(QueryItem *ptr, int size, bool *needcleanup)
{
	uint32		pos;

	*needcleanup = false;
	pos = 0;
	findoprnd_recurse(ptr, &pos, size, needcleanup);

	if (pos != size)
		elog(ERROR, "malformed tsquery: extra nodes");
}


/*
 * Parse the tsquery stored in "buf".
 *
 * Each value (operand) in the query is passed to pushval. pushval can
 * transform the simple value to an arbitrarily complex expression using
 * pushValue and pushOperator. It must push a single value with pushValue,
 * a complete expression with all operands, or a stopword placeholder
 * with pushStop, otherwise the prefix notation representation will be broken,
 * having an operator with no operand.
 *
 * opaque is passed on to pushval as is, pushval can use it to store its
 * private state.
 *
 * The pushval function can record soft errors via escontext.
 * Callers must check SOFT_ERROR_OCCURRED to detect that.
 *
 * A bitmask of flags (see ts_utils.h) and an error context object
 * can be provided as well.  If a soft error occurs, NULL is returned.
 */
TSQuery
parse_tsquery(char *buf,
			  PushFunction pushval,
			  Datum opaque,
			  int flags,
			  Node *escontext)
{
	struct TSQueryParserStateData state;
	int			i;
	TSQuery		query;
	int			commonlen;
	QueryItem  *ptr;
	ListCell   *cell;
	bool		noisy;
	bool		needcleanup;
	int			tsv_flags = P_TSV_OPR_IS_DELIM | P_TSV_IS_TSQUERY;

	/* plain should not be used with web */
	Assert((flags & (P_TSQ_PLAIN | P_TSQ_WEB)) != (P_TSQ_PLAIN | P_TSQ_WEB));

	/* select suitable tokenizer */
	if (flags & P_TSQ_PLAIN)
		state.gettoken = gettoken_query_plain;
	else if (flags & P_TSQ_WEB)
	{
		state.gettoken = gettoken_query_websearch;
		tsv_flags |= P_TSV_IS_WEB;
	}
	else
		state.gettoken = gettoken_query_standard;

	/* emit nuisance NOTICEs only if not doing soft errors */
	noisy = !(escontext && IsA(escontext, ErrorSaveContext));

	/* init state */
	state.buffer = buf;
	state.buf = buf;
	state.count = 0;
	state.state = WAITFIRSTOPERAND;
	state.polstr = NIL;
	state.escontext = escontext;

	/* init value parser's state */
	state.valstate = init_tsvector_parser(state.buffer, tsv_flags, escontext);

	/* init list of operand */
	state.sumlen = 0;
	state.lenop = 64;
	state.curop = state.op = (char *) palloc(state.lenop);
	*(state.curop) = '\0';

	/* parse query & make polish notation (postfix, but in reverse order) */
	makepol(&state, pushval, opaque);

	close_tsvector_parser(state.valstate);

	if (SOFT_ERROR_OCCURRED(escontext))
		return NULL;

	if (state.polstr == NIL)
	{
		if (noisy)
			ereport(NOTICE,
					(errmsg("text-search query doesn't contain lexemes: \"%s\"",
							state.buffer)));
		query = (TSQuery) palloc(HDRSIZETQ);
		SET_VARSIZE(query, HDRSIZETQ);
		query->size = 0;
		return query;
	}

	if (TSQUERY_TOO_BIG(list_length(state.polstr), state.sumlen))
		ereturn(escontext, NULL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("tsquery is too large")));
	commonlen = COMPUTESIZE(list_length(state.polstr), state.sumlen);

	/* Pack the QueryItems in the final TSQuery struct to return to caller */
	query = (TSQuery) palloc0(commonlen);
	SET_VARSIZE(query, commonlen);
	query->size = list_length(state.polstr);
	ptr = GETQUERY(query);

	/* Copy QueryItems to TSQuery */
	i = 0;
	foreach(cell, state.polstr)
	{
		QueryItem  *item = (QueryItem *) lfirst(cell);

		switch (item->type)
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
				elog(ERROR, "unrecognized QueryItem type: %d", item->type);
		}
		i++;
	}

	/* Copy all the operand strings to TSQuery */
	memcpy(GETOPERAND(query), state.op, state.sumlen);
	pfree(state.op);

	/*
	 * Set left operand pointers for every operator.  While we're at it,
	 * detect whether there are any QI_VALSTOP nodes.
	 */
	findoprnd(ptr, query->size, &needcleanup);

	/*
	 * If there are QI_VALSTOP nodes, delete them and simplify the tree.
	 */
	if (needcleanup)
		query = cleanup_tsquery_stopwords(query, noisy);

	return query;
}

static void
pushval_asis(Datum opaque, TSQueryParserState state, char *strval, int lenval,
			 int16 weight, bool prefix)
{
	pushValue(state, strval, lenval, weight, prefix);
}

/*
 * in without morphology
 */
Datum
tsqueryin(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;

	PG_RETURN_TSQUERY(parse_tsquery(in,
									pushval_asis,
									PointerGetDatum(NULL),
									0,
									escontext));
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
 * recursively traverse the tree and
 * print it in infix (human-readable) form
 */
static void
infix(INFIX *in, int parentPriority, bool rightPhraseOp)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (in->curpol->type == QI_VAL)
	{
		QueryOperand *curpol = &in->curpol->qoperand;
		char	   *op = in->op + curpol->distance;
		int			clen;

		RESIZEBUF(in, curpol->length * (pg_database_encoding_max_length() + 1) + 2 + 6);
		*(in->cur) = '\'';
		in->cur++;
		while (*op)
		{
			if (t_iseq(op, '\''))
			{
				*(in->cur) = '\'';
				in->cur++;
			}
			else if (t_iseq(op, '\\'))
			{
				*(in->cur) = '\\';
				in->cur++;
			}
			COPYCHAR(in->cur, op);

			clen = pg_mblen(op);
			op += clen;
			in->cur += clen;
		}
		*(in->cur) = '\'';
		in->cur++;
		if (curpol->weight || curpol->prefix)
		{
			*(in->cur) = ':';
			in->cur++;
			if (curpol->prefix)
			{
				*(in->cur) = '*';
				in->cur++;
			}
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
	else if (in->curpol->qoperator.oper == OP_NOT)
	{
		int			priority = QO_PRIORITY(in->curpol);

		if (priority < parentPriority)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}
		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol++;

		infix(in, priority, false);
		if (priority < parentPriority)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
	else
	{
		int8		op = in->curpol->qoperator.oper;
		int			priority = QO_PRIORITY(in->curpol);
		int16		distance = in->curpol->qoperator.distance;
		INFIX		nrm;
		bool		needParenthesis = false;

		in->curpol++;
		if (priority < parentPriority ||
		/* phrase operator depends on order */
			(op == OP_PHRASE && rightPhraseOp))
		{
			needParenthesis = true;
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}

		nrm.curpol = in->curpol;
		nrm.op = in->op;
		nrm.buflen = 16;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);

		/* get right operand */
		infix(&nrm, priority, (op == OP_PHRASE));

		/* get & print left operand */
		in->curpol = nrm.curpol;
		infix(in, priority, false);

		/* print operator & right operand */
		RESIZEBUF(in, 3 + (2 + 10 /* distance */ ) + (nrm.cur - nrm.buf));
		switch (op)
		{
			case OP_OR:
				sprintf(in->cur, " | %s", nrm.buf);
				break;
			case OP_AND:
				sprintf(in->cur, " & %s", nrm.buf);
				break;
			case OP_PHRASE:
				if (distance != 1)
					sprintf(in->cur, " <%d> %s", distance, nrm.buf);
				else
					sprintf(in->cur, " <-> %s", nrm.buf);
				break;
			default:
				/* OP_NOT is handled in above if-branch */
				elog(ERROR, "unrecognized operator type: %d", op);
		}
		in->cur = strchr(in->cur, '\0');
		pfree(nrm.buf);

		if (needParenthesis)
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
	infix(&nrm, -1 /* lowest priority */ , false);

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_CSTRING(nrm.buf);
}

/*
 * Binary Input / Output functions. The binary format is as follows:
 *
 * uint32	 number of operators/operands in the query
 *
 * Followed by the operators and operands, in prefix notation. For each
 * operand:
 *
 * uint8	type, QI_VAL
 * uint8	weight
 *			operand text in client encoding, null-terminated
 * uint8	prefix
 *
 * For each operator:
 * uint8	type, QI_OPR
 * uint8	operator, one of OP_AND, OP_PHRASE OP_OR, OP_NOT.
 * uint16	distance (only for OP_PHRASE)
 */
Datum
tsquerysend(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	StringInfoData buf;
	int			i;
	QueryItem  *item = GETQUERY(query);

	pq_begintypsend(&buf);

	pq_sendint32(&buf, query->size);
	for (i = 0; i < query->size; i++)
	{
		pq_sendint8(&buf, item->type);

		switch (item->type)
		{
			case QI_VAL:
				pq_sendint8(&buf, item->qoperand.weight);
				pq_sendint8(&buf, item->qoperand.prefix);
				pq_sendstring(&buf, GETOPERAND(query) + item->qoperand.distance);
				break;
			case QI_OPR:
				pq_sendint8(&buf, item->qoperator.oper);
				if (item->qoperator.oper == OP_PHRASE)
					pq_sendint16(&buf, item->qoperator.distance);
				break;
			default:
				elog(ERROR, "unrecognized tsquery node type: %d", item->type);
		}
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
				len;
	QueryItem  *item;
	int			datalen;
	char	   *ptr;
	uint32		size;
	const char **operands;
	bool		needcleanup;

	size = pq_getmsgint(buf, sizeof(uint32));
	if (size > (MaxAllocSize / sizeof(QueryItem)))
		elog(ERROR, "invalid size of tsquery");

	/* Allocate space to temporarily hold operand strings */
	operands = palloc(size * sizeof(char *));

	/* Allocate space for all the QueryItems. */
	len = HDRSIZETQ + sizeof(QueryItem) * size;
	query = (TSQuery) palloc0(len);
	query->size = size;
	item = GETQUERY(query);

	datalen = 0;
	for (i = 0; i < size; i++)
	{
		item->type = (int8) pq_getmsgint(buf, sizeof(int8));

		if (item->type == QI_VAL)
		{
			size_t		val_len;	/* length after recoding to server
									 * encoding */
			uint8		weight;
			uint8		prefix;
			const char *val;
			pg_crc32	valcrc;

			weight = (uint8) pq_getmsgint(buf, sizeof(uint8));
			prefix = (uint8) pq_getmsgint(buf, sizeof(uint8));
			val = pq_getmsgstring(buf);
			val_len = strlen(val);

			/* Sanity checks */

			if (weight > 0xF)
				elog(ERROR, "invalid tsquery: invalid weight bitmap");

			if (val_len > MAXSTRLEN)
				elog(ERROR, "invalid tsquery: operand too long");

			if (datalen > MAXSTRPOS)
				elog(ERROR, "invalid tsquery: total operand length exceeded");

			/* Looks valid. */

			INIT_LEGACY_CRC32(valcrc);
			COMP_LEGACY_CRC32(valcrc, val, val_len);
			FIN_LEGACY_CRC32(valcrc);

			item->qoperand.weight = weight;
			item->qoperand.prefix = (prefix) ? true : false;
			item->qoperand.valcrc = (int32) valcrc;
			item->qoperand.length = val_len;
			item->qoperand.distance = datalen;

			/*
			 * Operand strings are copied to the final struct after this loop;
			 * here we just collect them to an array
			 */
			operands[i] = val;

			datalen += val_len + 1; /* + 1 for the '\0' terminator */
		}
		else if (item->type == QI_OPR)
		{
			int8		oper;

			oper = (int8) pq_getmsgint(buf, sizeof(int8));
			if (oper != OP_NOT && oper != OP_OR && oper != OP_AND && oper != OP_PHRASE)
				elog(ERROR, "invalid tsquery: unrecognized operator type %d",
					 (int) oper);
			if (i == size - 1)
				elog(ERROR, "invalid pointer to right operand");

			item->qoperator.oper = oper;
			if (oper == OP_PHRASE)
				item->qoperator.distance = (int16) pq_getmsgint(buf, sizeof(int16));
		}
		else
			elog(ERROR, "unrecognized tsquery node type: %d", item->type);

		item++;
	}

	/* Enlarge buffer to make room for the operand values. */
	query = (TSQuery) repalloc(query, len + datalen);
	item = GETQUERY(query);
	ptr = GETOPERAND(query);

	/*
	 * Fill in the left-pointers. Checks that the tree is well-formed as a
	 * side-effect.
	 */
	findoprnd(item, size, &needcleanup);

	/* Can't have found any QI_VALSTOP nodes */
	Assert(!needcleanup);

	/* Copy operands to output struct */
	for (i = 0; i < size; i++)
	{
		if (item->type == QI_VAL)
		{
			memcpy(ptr, operands[i], item->qoperand.length + 1);
			ptr += item->qoperand.length + 1;
		}
		item++;
	}

	pfree(operands);

	Assert(ptr - GETOPERAND(query) == datalen);

	SET_VARSIZE(query, len + datalen);

	PG_RETURN_TSQUERY(query);
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
		res = cstring_to_text("T");
	}
	else
	{
		nrm.curpol = q;
		nrm.buflen = 32;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
		*(nrm.cur) = '\0';
		nrm.op = GETOPERAND(query);
		infix(&nrm, -1, false);
		res = cstring_to_text_with_len(nrm.buf, nrm.cur - nrm.buf);
		pfree(q);
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_TEXT_P(res);
}
