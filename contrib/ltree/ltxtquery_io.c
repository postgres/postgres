/*
 * txtquery io
 * Teodor Sigaev <teodor@stack.net>
 * contrib/ltree/ltxtquery_io.c
 */
#include "postgres.h"

#include <ctype.h>

#include "crc32.h"
#include "ltree.h"
#include "miscadmin.h"

PG_FUNCTION_INFO_V1(ltxtq_in);
PG_FUNCTION_INFO_V1(ltxtq_out);


/* parser's states */
#define WAITOPERAND 1
#define INOPERAND 2
#define WAITOPERATOR	3

/*
 * node of query tree, also used
 * for storing polish notation in parser
 */
typedef struct NODE
{
	int32		type;
	int32		val;
	int16		distance;
	int16		length;
	uint16		flag;
	struct NODE *next;
} NODE;

typedef struct
{
	char	   *buf;
	int32		state;
	int32		count;
	/* reverse polish notation in list (for temporary usage) */
	NODE	   *str;
	/* number in str */
	int32		num;

	/* user-friendly operand */
	int32		lenop;
	int32		sumlen;
	char	   *op;
	char	   *curop;
} QPRS_STATE;

/*
 * get token from query string
 */
static int32
gettoken_query(QPRS_STATE *state, int32 *val, int32 *lenval, char **strval, uint16 *flag)
{
	int			charlen;

	for (;;)
	{
		charlen = pg_mblen(state->buf);

		switch (state->state)
		{
			case WAITOPERAND:
				if (charlen == 1 && t_iseq(state->buf, '!'))
				{
					(state->buf)++;
					*val = (int32) '!';
					return OPR;
				}
				else if (charlen == 1 && t_iseq(state->buf, '('))
				{
					state->count++;
					(state->buf)++;
					return OPEN;
				}
				else if (ISALNUM(state->buf))
				{
					state->state = INOPERAND;
					*strval = state->buf;
					*lenval = charlen;
					*flag = 0;
				}
				else if (!t_isspace(state->buf))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("operand syntax error")));
				break;
			case INOPERAND:
				if (ISALNUM(state->buf))
				{
					if (*flag)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("modificators syntax error")));
					*lenval += charlen;
				}
				else if (charlen == 1 && t_iseq(state->buf, '%'))
					*flag |= LVAR_SUBLEXEME;
				else if (charlen == 1 && t_iseq(state->buf, '@'))
					*flag |= LVAR_INCASE;
				else if (charlen == 1 && t_iseq(state->buf, '*'))
					*flag |= LVAR_ANYEND;
				else
				{
					state->state = WAITOPERATOR;
					return VAL;
				}
				break;
			case WAITOPERATOR:
				if (charlen == 1 && (t_iseq(state->buf, '&') || t_iseq(state->buf, '|')))
				{
					state->state = WAITOPERAND;
					*val = (int32) *(state->buf);
					(state->buf)++;
					return OPR;
				}
				else if (charlen == 1 && t_iseq(state->buf, ')'))
				{
					(state->buf)++;
					state->count--;
					return (state->count < 0) ? ERR : CLOSE;
				}
				else if (*(state->buf) == '\0')
					return (state->count) ? ERR : END;
				else if (charlen == 1 && !t_iseq(state->buf, ' '))
					return ERR;
				break;
			default:
				return ERR;
				break;
		}

		state->buf += charlen;
	}
}

/*
 * push new one in polish notation reverse view
 */
static void
pushquery(QPRS_STATE *state, int32 type, int32 val, int32 distance, int32 lenval, uint16 flag)
{
	NODE	   *tmp = (NODE *) palloc(sizeof(NODE));

	tmp->type = type;
	tmp->val = val;
	tmp->flag = flag;
	if (distance > 0xffff)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("value is too big")));
	if (lenval > 0xff)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
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
pushval_asis(QPRS_STATE *state, int type, char *strval, int lenval, uint16 flag)
{
	if (lenval > 0xffff)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("word is too long")));

	pushquery(state, type, ltree_crc32_sz(strval, lenval),
			  state->curop - state->op, lenval, flag);

	while (state->curop - state->op + lenval + 1 >= state->lenop)
	{
		int32		tmp = state->curop - state->op;

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

#define STACKDEPTH		32
/*
 * make polish notaion of query
 */
static int32
makepol(QPRS_STATE *state)
{
	int32		val = 0,
				type;
	int32		lenval = 0;
	char	   *strval = NULL;
	int32		stack[STACKDEPTH];
	int32		lenstack = 0;
	uint16		flag = 0;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	while ((type = gettoken_query(state, &val, &lenval, &strval, &flag)) != END)
	{
		switch (type)
		{
			case VAL:
				pushval_asis(state, VAL, strval, lenval, flag);
				while (lenstack && (stack[lenstack - 1] == (int32) '&' ||
									stack[lenstack - 1] == (int32) '!'))
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack], 0, 0, 0);
				}
				break;
			case OPR:
				if (lenstack && val == (int32) '|')
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
				if (makepol(state) == ERR)
					return ERR;
				while (lenstack && (stack[lenstack - 1] == (int32) '&' ||
									stack[lenstack - 1] == (int32) '!'))
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

static void
findoprnd(ITEM *ptr, int32 *pos)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (ptr[*pos].type == VAL || ptr[*pos].type == VALTRUE)
	{
		ptr[*pos].left = 0;
		(*pos)++;
	}
	else if (ptr[*pos].val == (int32) '!')
	{
		ptr[*pos].left = 1;
		(*pos)++;
		findoprnd(ptr, pos);
	}
	else
	{
		ITEM	   *curitem = &ptr[*pos];
		int32		tmp = *pos;

		(*pos)++;
		findoprnd(ptr, pos);
		curitem->left = *pos - tmp;
		findoprnd(ptr, pos);
	}
}


/*
 * input
 */
static ltxtquery *
queryin(char *buf)
{
	QPRS_STATE	state;
	int32		i;
	ltxtquery  *query;
	int32		commonlen;
	ITEM	   *ptr;
	NODE	   *tmp;
	int32		pos = 0;

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

	/* init list of operand */
	state.sumlen = 0;
	state.lenop = 64;
	state.curop = state.op = (char *) palloc(state.lenop);
	*(state.curop) = '\0';

	/* parse query & make polish notation (postfix, but in reverse order) */
	makepol(&state);
	if (!state.num)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error"),
				 errdetail("Empty query.")));

	if (LTXTQUERY_TOO_BIG(state.num, state.sumlen))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("ltxtquery is too large")));
	commonlen = COMPUTESIZE(state.num, state.sumlen);

	query = (ltxtquery *) palloc0(commonlen);
	SET_VARSIZE(query, commonlen);
	query->size = state.num;
	ptr = GETQUERY(query);

	/* set item in polish notation */
	for (i = 0; i < state.num; i++)
	{
		ptr[i].type = state.str->type;
		ptr[i].val = state.str->val;
		ptr[i].distance = state.str->distance;
		ptr[i].length = state.str->length;
		ptr[i].flag = state.str->flag;
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
ltxtq_in(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(queryin((char *) PG_GETARG_POINTER(0)));
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
	int32		buflen;
} INFIX;

#define RESIZEBUF(inf,addsize) \
while( ( (inf)->cur - (inf)->buf ) + (addsize) + 1 >= (inf)->buflen ) \
{ \
	int32 len = (inf)->cur - (inf)->buf; \
	(inf)->buflen *= 2; \
	(inf)->buf = (char*) repalloc( (void*)(inf)->buf, (inf)->buflen ); \
	(inf)->cur = (inf)->buf + len; \
}

/*
 * recursive walk on tree and print it in
 * infix (human-readable) view
 */
static void
infix(INFIX *in, bool first)
{
	/* since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (in->curpol->type == VAL)
	{
		char	   *op = in->op + in->curpol->distance;

		RESIZEBUF(in, in->curpol->length * 2 + 5);
		while (*op)
		{
			*(in->cur) = *op;
			op++;
			in->cur++;
		}
		if (in->curpol->flag & LVAR_SUBLEXEME)
		{
			*(in->cur) = '%';
			in->cur++;
		}
		if (in->curpol->flag & LVAR_INCASE)
		{
			*(in->cur) = '@';
			in->cur++;
		}
		if (in->curpol->flag & LVAR_ANYEND)
		{
			*(in->cur) = '*';
			in->cur++;
		}
		*(in->cur) = '\0';
		in->curpol++;
	}
	else if (in->curpol->val == (int32) '!')
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
		int32		op = in->curpol->val;
		INFIX		nrm;

		in->curpol++;
		if (op == (int32) '|' && !first)
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

		if (op == (int32) '|' && !first)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
}

Datum
ltxtq_out(PG_FUNCTION_ARGS)
{
	ltxtquery  *query = PG_GETARG_LTXTQUERY(0);
	INFIX		nrm;

	if (query->size == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error"),
				 errdetail("Empty query.")));

	nrm.curpol = GETQUERY(query);
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
	*(nrm.cur) = '\0';
	nrm.op = GETOPERAND(query);
	infix(&nrm, true);

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_POINTER(nrm.buf);
}
