/*
 * IO definitions for tsquery and mtsquery. This type
 * are identical, but for parsing mtsquery used parser for text
 * and also morphology is used.
 * Internal structure:
 * query tree, then string with original value.
 * Query tree with plain view. It's means that in array of nodes
 * right child is always next and left position = item+item->left
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <float.h>
#include <ctype.h>

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"

#include "ts_cfg.h"
#include "tsvector.h"
#include "crc32.h"
#include "query.h"
#include "rewrite.h"
#include "common.h"


PG_FUNCTION_INFO_V1(tsquery_in);
Datum		tsquery_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsquery_out);
Datum		tsquery_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(exectsq);
Datum		exectsq(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(rexectsq);
Datum		rexectsq(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(tsquerytree);
Datum		tsquerytree(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(to_tsquery);
Datum		to_tsquery(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(to_tsquery_name);
Datum		to_tsquery_name(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(to_tsquery_current);
Datum		to_tsquery_current(PG_FUNCTION_ARGS);

/* parser's states */
#define WAITOPERAND 1
#define WAITOPERATOR	2

/*
 * node of query tree, also used
 * for storing polish notation in parser
 */
typedef struct NODE
{
	int2		weight;
	int2		type;
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
	/* reverse polish notation in list (for temprorary usage) */
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

	/* tscfg */
	int			cfg_id;
}	QPRS_STATE;

static char *
get_weight(char *buf, int2 *weight)
{
	*weight = 0;

	if (*buf != ':')
		return buf;

	buf++;
	while (*buf)
	{
		switch (tolower(*buf))
		{
			case 'a':
				*weight |= 1 << 3;
				break;
			case 'b':
				*weight |= 1 << 2;
				break;
			case 'c':
				*weight |= 1 << 1;
				break;
			case 'd':
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
gettoken_query(QPRS_STATE * state, int4 *val, int4 *lenval, char **strval, int2 *weight)
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
				else if (*(state->buf) == ':')
				{
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("error at start of operand")));
				}
				else if (*(state->buf) != ' ')
				{
					state->valstate.prsbuf = state->buf;
					state->state = WAITOPERATOR;
					if (gettoken_tsvector(&(state->valstate)))
					{
						*strval = state->valstate.word;
						*lenval = state->valstate.curpos - state->valstate.word;
						state->buf = get_weight(state->valstate.prsbuf, weight);
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
pushquery(QPRS_STATE * state, int4 type, int4 val, int4 distance, int4 lenval, int2 weight)
{
	NODE	   *tmp = (NODE *) palloc(sizeof(NODE));

	tmp->weight = weight;
	tmp->type = type;
	tmp->val = val;
	if (distance >= MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("value is too big")));
	if (lenval >= MAXSTRLEN)
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
 * This function is used for tsquery parsing
 */
static void
pushval_asis(QPRS_STATE * state, int type, char *strval, int lenval, int2 weight)
{
	if (lenval >= MAXSTRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("word is too long")));

	pushquery(state, type, crc32_sz((uint8 *) strval, lenval),
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
	state->sumlen += lenval + 1;
	return;
}

/*
 * This function is used for morph parsing
 */
static void
pushval_morph(QPRS_STATE * state, int typeval, char *strval, int lenval, int2 weight)
{
	int4		count = 0;
	PRSTEXT		prs;

	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (WORD *) palloc(sizeof(WORD) * prs.lenwords);

	parsetext_v2(findcfg(state->cfg_id), &prs, strval, lenval);

	for (count = 0; count < prs.curwords; count++)
	{
		pushval_asis(state, VAL, prs.words[count].word, prs.words[count].len, weight);
		pfree(prs.words[count].word);
		if (count)
			pushquery(state, OPR, (int4) '&', 0, 0, 0);
	}
	pfree(prs.words);

	/* XXX */
	if (prs.curwords == 0)
		pushval_asis(state, VALSTOP, 0, 0, 0);
}

#define STACKDEPTH	32
/*
 * make polish notaion of query
 */
static int4
makepol(QPRS_STATE * state, void (*pushval) (QPRS_STATE *, int, char *, int, int2))
{
	int4		val,
				type;
	int4		lenval;
	char	   *strval;
	int4		stack[STACKDEPTH];
	int4		lenstack = 0;
	int2		weight;

	while ((type = gettoken_query(state, &val, &lenval, &strval, &weight)) != END)
	{
		switch (type)
		{
			case VAL:
				(*pushval) (state, VAL, strval, lenval, weight);
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
						 errmsg("syntax error")));
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
 * check weight info
 */
static bool
checkclass_str(CHKVAL * chkval, WordEntry * val, ITEM * item)
{
	WordEntryPos *ptr = (WordEntryPos *) (chkval->values + val->pos + SHORTALIGN(val->len) + sizeof(uint16));
	uint16		len = *((uint16 *) (chkval->values + val->pos + SHORTALIGN(val->len)));

	while (len--)
	{
		if (item->weight & (1 << ptr->weight))
			return true;
		ptr++;
	}
	return false;
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
			return (val->weight && StopMiddle->haspos) ?
				checkclass_str((CHKVAL *) checkval, StopMiddle, val) : true;
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
TS_execute(ITEM * curitem, void *checkval, bool calcnot, bool (*chkcond) (void *checkval, ITEM * val))
{
	if (curitem->type == VAL)
		return (*chkcond) (checkval, curitem);
	else if (curitem->val == (int4) '!')
	{
		return (calcnot) ?
			((TS_execute(curitem + 1, checkval, calcnot, chkcond)) ? false : true)
			: true;
	}
	else if (curitem->val == (int4) '&')
	{
		if (TS_execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return TS_execute(curitem + 1, checkval, calcnot, chkcond);
		else
			return false;
	}
	else
	{							/* |-operator */
		if (TS_execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return true;
		else
			return TS_execute(curitem + 1, checkval, calcnot, chkcond);
	}
	return false;
}

/*
 * boolean operations
 */
Datum
rexectsq(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(
							   exectsq,
							   PG_GETARG_DATUM(1),
							   PG_GETARG_DATUM(0)
		);
}

Datum
exectsq(PG_FUNCTION_ARGS)
{
	tsvector   *val = (tsvector *) DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
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
	result = TS_execute(
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
	elog(DEBUG3, (ptr[*pos].type == OPR) ?
		 "%d  %c" : "%d  %d", *pos, ptr[*pos].val);
#endif
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
			queryin(char *buf, void (*pushval) (QPRS_STATE *, int, char *, int, int2), int cfg_id)
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
	elog(DEBUG3, "POR: %s", pbuf);
#endif

	return query;
}

/*
 * in without morphology
 */
Datum
tsquery_in(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(queryin((char *) PG_GETARG_POINTER(0), pushval_asis, 0));
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

		RESIZEBUF(in, in->curpol->length * 2 + 2 + 5);
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
tsquery_out(PG_FUNCTION_ARGS)
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
tsquerytree(PG_FUNCTION_ARGS)
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

	q = clean_NOT_v2(GETQUERY(query), &len);

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

Datum
to_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(1);
	char	   *str;
	QUERYTYPE  *query;
	ITEM	   *res;
	int4		len;

	str = text2char(in);
	PG_FREE_IF_COPY(in, 1);

	query = queryin(str, pushval_morph, PG_GETARG_INT32(0));
	res = clean_fakeval_v2(GETQUERY(query), &len);
	if (!res)
	{
		query->len = HDRSIZEQT;
		query->size = 0;
		PG_RETURN_POINTER(query);
	}
	memcpy((void *) GETQUERY(query), (void *) res, len * sizeof(ITEM));
	pfree(res);
	PG_RETURN_POINTER(query);
}

Datum
to_tsquery_name(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_P(0);
	Datum		res = DirectFunctionCall2(to_tsquery,
										Int32GetDatum(name2id_cfg(name)),
										  PG_GETARG_DATUM(1));

	PG_FREE_IF_COPY(name, 0);
	PG_RETURN_DATUM(res);
}

Datum
to_tsquery_current(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(to_tsquery,
										Int32GetDatum(get_currcfg()),
										PG_GETARG_DATUM(0)));
}
