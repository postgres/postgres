/*
 * IO definitions for query_txt and mquery_txt. This type
 * are identical, but for parsing mquery_txt used parser for text
 * and also morphology is used.
 * Internal structure:
 * query tree, then string with original value.
 * Query tree with plain view. It's means that in array of nodes
 * right child is always next and left position = item+item->left
 * Teodor Sigaev <teodor@stack.net>
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"

#include "txtidx.h"
#include "crc32.h"
#include "query.h"
#include "morph.h"
#include "rewrite.h"

#include "deflex.h"
#include "parser.h"

PG_FUNCTION_INFO_V1(mqtxt_in);
Datum		mqtxt_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(qtxt_in);
Datum		qtxt_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(qtxt_out);
Datum		qtxt_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(execqtxt);
Datum		execqtxt(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(rexecqtxt);
Datum		rexecqtxt(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(querytree);
Datum		querytree(PG_FUNCTION_ARGS);

#define END			0
#define ERR			1
#define VAL			2
#define OPR			3
#define OPEN		4
#define CLOSE		5
#define VALTRUE		6			/* for stop words */
#define VALFALSE	7

/* parser's states */
#define WAITOPERAND 1
#define WAITOPERATOR	2

/*
 * node of query tree, also used
 * for storing polish notation in parser
 */
typedef struct NODE
{
	int4		type;
	int4		val;
	int2		distance;
	int2		length;
	struct NODE *next;
}	NODE;

typedef struct
{
	char	   *buf;
	int4		state;
	int4		count;
	/* reverse polish notation in list (for temporary usage) */
	NODE	   *str;
	/* number in str */
	int4		num;

	/* user-friendly operand */
	int4		lenop;
	int4		sumlen;
	char	   *op;
	char	   *curop;

	/* state for value's parser */
	TI_IN_STATE valstate;
}	QPRS_STATE;

/*
 * get token from query string
 */
static int4
gettoken_query(QPRS_STATE * state, int4 *val, int4 *lenval, char **strval)
{
	while (1)
	{
		switch (state->state)
		{
			case WAITOPERAND:
				if (*(state->buf) == '!')
				{
					(state->buf)++;
					*val = (int4) '!';
					return OPR;
				}
				else if (*(state->buf) == '(')
				{
					state->count++;
					(state->buf)++;
					return OPEN;
				}
				else if (*(state->buf) != ' ')
				{
					state->valstate.prsbuf = state->buf;
					state->state = WAITOPERATOR;
					if (gettoken_txtidx(&(state->valstate)))
					{
						*strval = state->valstate.word;
						*lenval = state->valstate.curpos - state->valstate.word;
						state->buf = state->valstate.prsbuf;
						return VAL;
					}
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("no operand")));
				}
				break;
			case WAITOPERATOR:
				if (*(state->buf) == '&' || *(state->buf) == '|')
				{
					state->state = WAITOPERAND;
					*val = (int4) *(state->buf);
					(state->buf)++;
					return OPR;
				}
				else if (*(state->buf) == ')')
				{
					(state->buf)++;
					state->count--;
					return (state->count < 0) ? ERR : CLOSE;
				}
				else if (*(state->buf) == '\0')
					return (state->count) ? ERR : END;
				else if (*(state->buf) != ' ')
					return ERR;
				break;
			default:
				return ERR;
				break;
		}
		(state->buf)++;
	}
	return END;
}

/*
 * push new one in polish notation reverse view
 */
static void
pushquery(QPRS_STATE * state, int4 type, int4 val, int4 distance, int4 lenval)
{
	NODE	   *tmp = (NODE *) palloc(sizeof(NODE));

	tmp->type = type;
	tmp->val = val;
	if (distance > 0xffff)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("value is too big")));
	if (lenval > 0xffff)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("operand is too long")));
	tmp->distance = distance;
	tmp->length = lenval;
	tmp->next = state->str;
	state->str = tmp;
	state->num++;
}

/*
 * This function is used for query_txt parsing
 */
static void
pushval_asis(QPRS_STATE * state, int type, char *strval, int lenval)
{
	if (lenval > 0xffff)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("word is too long")));

	pushquery(state, type, crc32_sz((uint8 *) strval, lenval),
			  state->curop - state->op, lenval);

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
	state->sumlen += lenval + 1;
	return;
}

/*
 * This function is used for mquery_txt parsing
 */
static void
pushval_morph(QPRS_STATE * state, int typeval, char *strval, int lenval)
{
	int4		type,
				lenlemm;
	int4		count = 0;
	char	   *lemm;

	start_parse_str(strval, lenval);
	while ((type = tsearch_yylex()) != 0)
	{
		if (tokenlen > 0xffff)
		{
			end_parse();
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("word is too long")));
		}
		lenlemm = tokenlen;
		lemm = lemmatize(token, &lenlemm, type);
		if (lemm)
		{
			if ( lemm==token ) {
				char *ptrs=token,*ptrd;
				ptrd = lemm = palloc(lenlemm+1);
				while(ptrs-token<lenlemm) {
					*ptrd = tolower((unsigned char) *ptrs);
					ptrs++;
					ptrd++;
				}
				*ptrd='\0';
			}	
			pushval_asis(state, VAL, lemm, lenlemm);
			pfree(lemm);
		}
		else
			pushval_asis(state, VALTRUE, 0, 0);
		if (count)
			pushquery(state, OPR, (int4) '&', 0, 0);
		count++;
	}
	end_parse();
}

#define STACKDEPTH	32
/*
 * make polish notation of query
 */
static int4
makepol(QPRS_STATE * state, void (*pushval) (QPRS_STATE *, int, char *, int))
{
	int4		val,
				type;
	int4		lenval;
	char	   *strval;
	int4		stack[STACKDEPTH];
	int4		lenstack = 0;

	while ((type = gettoken_query(state, &val, &lenval, &strval)) != END)
	{
		switch (type)
		{
			case VAL:
				(*pushval) (state, VAL, strval, lenval);
				while (lenstack && (stack[lenstack - 1] == (int4) '&' ||
									stack[lenstack - 1] == (int4) '!'))
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack], 0, 0);
				}
				break;
			case OPR:
				if (lenstack && val == (int4) '|')
					pushquery(state, OPR, val, 0, 0);
				else
				{
					if (lenstack == STACKDEPTH)
						/* internal error */
						elog(ERROR, "stack too short");
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
					pushquery(state, OPR, stack[lenstack], 0, 0);
				}
				break;
			case CLOSE:
				while (lenstack)
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack], 0, 0);
				};
				return END;
				break;
			case ERR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));

				return ERR;

		}
	}
	while (lenstack)
	{
		lenstack--;
		pushquery(state, OPR, stack[lenstack], 0, 0);
	};
	return END;
}

typedef struct
{
	WordEntry  *arrb;
	WordEntry  *arre;
	char	   *values;
	char	   *operand;
}	CHKVAL;

/*
 * compare 2 string values
 */
static int4
ValCompare(CHKVAL * chkval, WordEntry * ptr, ITEM * item)
{
	if (ptr->len == item->length)
		return strncmp(
					   &(chkval->values[ptr->pos]),
					   &(chkval->operand[item->distance]),
					   item->length);

	return (ptr->len > item->length) ? 1 : -1;
}

/*
 * is there value 'val' in array or not ?
 */
static bool
checkcondition_str(void *checkval, ITEM * val)
{
	WordEntry  *StopLow = ((CHKVAL *) checkval)->arrb;
	WordEntry  *StopHigh = ((CHKVAL *) checkval)->arre;
	WordEntry  *StopMiddle;
	int			difference;

	/* Loop invariant: StopLow <= val < StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = ValCompare((CHKVAL *) checkval, StopMiddle, val);
		if (difference == 0)
			return (true);
		else if (difference < 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	return (false);
}

/*
 * check for boolean condition
 */
bool
execute(ITEM * curitem, void *checkval, bool calcnot, bool (*chkcond) (void *checkval, ITEM * val))
{
	if (curitem->type == VAL)
		return (*chkcond) (checkval, curitem);
	else if (curitem->val == (int4) '!')
	{
		return (calcnot) ?
			((execute(curitem + 1, checkval, calcnot, chkcond)) ? false : true)
			: true;
	}
	else if (curitem->val == (int4) '&')
	{
		if (execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return execute(curitem + 1, checkval, calcnot, chkcond);
		else
			return false;
	}
	else
	{							/* |-operator */
		if (execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return true;
		else
			return execute(curitem + 1, checkval, calcnot, chkcond);
	}
	return false;
}

/*
 * boolean operations
 */
Datum
rexecqtxt(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(
							   execqtxt,
							   PG_GETARG_DATUM(1),
							   PG_GETARG_DATUM(0)
		);
}

Datum
execqtxt(PG_FUNCTION_ARGS)
{
	txtidx	   *val = (txtidx *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
	CHKVAL		chkval;
	bool		result;

	if (!val->size || !query->size)
	{
		PG_FREE_IF_COPY(val, 0);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(false);
	}

	chkval.arrb = ARRPTR(val);
	chkval.arre = chkval.arrb + val->size;
	chkval.values = STRPTR(val);
	chkval.operand = GETOPERAND(query);
	result = execute(
					 GETQUERY(query),
					 &chkval,
					 true,
					 checkcondition_str
		);

	PG_FREE_IF_COPY(val, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

/*
 * find left operand in polish notation view
 */
static void
findoprnd(ITEM * ptr, int4 *pos)
{
#ifdef BS_DEBUG
	elog(DEBUG4, (ptr[*pos].type == OPR) ?
		 "%d  %c" : "%d  %d", *pos, ptr[*pos].val);
#endif
	if (ptr[*pos].type == VAL || ptr[*pos].type == VALTRUE)
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
		ITEM	   *curitem = &ptr[*pos];
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
static QUERYTYPE *
queryin(char *buf, void (*pushval) (QPRS_STATE *, int, char *, int))
{
	QPRS_STATE	state;
	int4		i;
	QUERYTYPE  *query;
	int4		commonlen;
	ITEM	   *ptr;
	NODE	   *tmp;
	int4		pos = 0;

#ifdef BS_DEBUG
	char		pbuf[16384],
			   *cur;
#endif

	/* init state */
	state.buf = buf;
	state.state = WAITOPERAND;
	state.count = 0;
	state.num = 0;
	state.str = NULL;

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
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("empty query")));

	/* make finish struct */
	commonlen = COMPUTESIZE(state.num, state.sumlen);
	query = (QUERYTYPE *) palloc(commonlen);
	query->len = commonlen;
	query->size = state.num;
	ptr = GETQUERY(query);

	/* set item in polish notation */
	for (i = 0; i < state.num; i++)
	{
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

#ifdef BS_DEBUG
	cur = pbuf;
	*cur = '\0';
	for (i = 0; i < query->size; i++)
	{
		if (ptr[i].type == OPR)
			sprintf(cur, "%c(%d) ", ptr[i].val, ptr[i].left);
		else
			sprintf(cur, "%d(%s) ", ptr[i].val, GETOPERAND(query) + ptr[i].distance);
		cur = strchr(cur, '\0');
	}
	elog(DEBUG4, "POR: %s", pbuf);
#endif

	return query;
}

/*
 * in without morphology
 */
Datum
qtxt_in(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(queryin((char *) PG_GETARG_POINTER(0), pushval_asis));
}

/*
 * in with morphology
 */
Datum
mqtxt_in(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query;
	ITEM	   *res;
	int4		len;

#ifdef BS_DEBUG
	ITEM	   *ptr;
	int4		i;
	char		pbuf[16384],
			   *cur;
#endif
	initmorph();
	query = queryin((char *) PG_GETARG_POINTER(0), pushval_morph);
	res = clean_fakeval(GETQUERY(query), &len);
	if (!res)
	{
		query->len = HDRSIZEQT;
		query->size = 0;
		PG_RETURN_POINTER(query);
	}
	memcpy((void *) GETQUERY(query), (void *) res, len * sizeof(ITEM));
#ifdef BS_DEBUG
	cur = pbuf;
	*cur = '\0';
	ptr = GETQUERY(query);
	for (i = 0; i < len; i++)
	{
		if (ptr[i].type == OPR)
			sprintf(cur, "%c(%d) ", ptr[i].val, ptr[i].left);
		else
			sprintf(cur, "%d(%s) ", ptr[i].val, GETOPERAND(query) + ptr[i].distance);
		cur = strchr(cur, '\0');
	}
	elog(DEBUG4, "POR: %s", pbuf);
#endif
	pfree(res);
	PG_RETURN_POINTER(query);
}


/*
 * out function
 */
typedef struct
{
	ITEM	   *curpol;
	char	   *buf;
	char	   *cur;
	char	   *op;
	int4		buflen;
}	INFIX;

#define RESIZEBUF(inf,addsize) \
while( ( inf->cur - inf->buf ) + addsize + 1 >= inf->buflen ) \
{ \
	int4 len = inf->cur - inf->buf; \
	inf->buflen *= 2; \
	inf->buf = (char*) repalloc( (void*)inf->buf, inf->buflen ); \
	inf->cur = inf->buf + len; \
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

		RESIZEBUF(in, in->curpol->length * 2 + 2);
		*(in->cur) = '\'';
		in->cur++;
		while (*op)
		{
			if (*op == '\'')
			{
				*(in->cur) = '\\';
				in->cur++;
			}
			*(in->cur) = *op;
			op++;
			in->cur++;
		}
		*(in->cur) = '\'';
		in->cur++;
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
qtxt_out(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
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
	PG_RETURN_POINTER(nrm.buf);
}

/*
 * debug function, used only for view query
 * which will be executed in non-leaf pages in index
 */
Datum
querytree(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	INFIX		nrm;
	text	   *res;
	ITEM	   *q;
	int4		len;


	if (query->size == 0)
	{
		res = (text *) palloc(VARHDRSZ);
		VARATT_SIZEP(res) = VARHDRSZ;
		PG_RETURN_POINTER(res);
	}

	q = clean_NOT(GETQUERY(query), &len);

	if (!q)
	{
		res = (text *) palloc(1 + VARHDRSZ);
		VARATT_SIZEP(res) = 1 + VARHDRSZ;
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
		VARATT_SIZEP(res) = nrm.cur - nrm.buf + VARHDRSZ;
		strncpy(VARDATA(res), nrm.buf, nrm.cur - nrm.buf);
		pfree(q);
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_POINTER(res);
}

#include "parser.c"
