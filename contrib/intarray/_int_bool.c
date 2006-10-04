#include "_int.h"

PG_FUNCTION_INFO_V1(bqarr_in);
PG_FUNCTION_INFO_V1(bqarr_out);
Datum		bqarr_in(PG_FUNCTION_ARGS);
Datum		bqarr_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(boolop);
Datum		boolop(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(rboolop);
Datum		rboolop(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(querytree);
Datum		querytree(PG_FUNCTION_ARGS);


#define END		0
#define ERR		1
#define VAL		2
#define OPR		3
#define OPEN	4
#define CLOSE	5

/* parser's states */
#define WAITOPERAND 1
#define WAITENDOPERAND	2
#define WAITOPERATOR	3

/*
 * node of query tree, also used
 * for storing polish notation in parser
 */
typedef struct NODE
{
	int4		type;
	int4		val;
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
}	WORKSTATE;

/*
 * get token from query string
 */
static int4
gettoken(WORKSTATE * state, int4 *val)
{
	char		nnn[16],
			   *curnnn;

	*val = 0;					/* default result */

	curnnn = nnn;
	while (1)
	{
		switch (state->state)
		{
			case WAITOPERAND:
				curnnn = nnn;
				if ((*(state->buf) >= '0' && *(state->buf) <= '9') ||
					*(state->buf) == '-')
				{
					state->state = WAITENDOPERAND;
					*curnnn = *(state->buf);
					curnnn++;
				}
				else if (*(state->buf) == '!')
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
					return ERR;
				break;
			case WAITENDOPERAND:
				if (*(state->buf) >= '0' && *(state->buf) <= '9')
				{
					*curnnn = *(state->buf);
					curnnn++;
				}
				else
				{
					*curnnn = '\0';
					*val = (int4) atoi(nnn);
					state->state = WAITOPERATOR;
					return (state->count && *(state->buf) == '\0')
						? ERR : VAL;
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
pushquery(WORKSTATE * state, int4 type, int4 val)
{
	NODE	   *tmp = (NODE *) palloc(sizeof(NODE));

	tmp->type = type;
	tmp->val = val;
	tmp->next = state->str;
	state->str = tmp;
	state->num++;
}

#define STACKDEPTH	16

/*
 * make polish notation of query
 */
static int4
makepol(WORKSTATE * state)
{
	int4		val,
				type;
	int4		stack[STACKDEPTH];
	int4		lenstack = 0;

	while ((type = gettoken(state, &val)) != END)
	{
		switch (type)
		{
			case VAL:
				pushquery(state, type, val);
				while (lenstack && (stack[lenstack - 1] == (int4) '&' ||
									stack[lenstack - 1] == (int4) '!'))
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack]);
				}
				break;
			case OPR:
				if (lenstack && val == (int4) '|')
					pushquery(state, OPR, val);
				else
				{
					if (lenstack == STACKDEPTH)
						ereport(ERROR,
								(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
								 errmsg("statement too complex")));
					stack[lenstack] = val;
					lenstack++;
				}
				break;
			case OPEN:
				if (makepol(state) == ERR)
					return ERR;
				if (lenstack && (stack[lenstack - 1] == (int4) '&' ||
								 stack[lenstack - 1] == (int4) '!'))
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack]);
				}
				break;
			case CLOSE:
				while (lenstack)
				{
					lenstack--;
					pushquery(state, OPR, stack[lenstack]);
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
		pushquery(state, OPR, stack[lenstack]);
	};
	return END;
}

typedef struct
{
	int4	   *arrb;
	int4	   *arre;
}	CHKVAL;

/*
 * is there value 'val' in array or not ?
 */
static bool
checkcondition_arr(void *checkval, ITEM * item)
{
	int4	   *StopLow = ((CHKVAL *) checkval)->arrb;
	int4	   *StopHigh = ((CHKVAL *) checkval)->arre;
	int4	   *StopMiddle;

	/* Loop invariant: StopLow <= val < StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		if (*StopMiddle == item->val)
			return (true);
		else if (*StopMiddle < item->val)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}
	return false;
}

static bool
checkcondition_bit(void *checkval, ITEM * item)
{
	return GETBIT(checkval, HASHVAL(item->val));
}

/*
 * check for boolean condition
 */
static bool
execute(ITEM * curitem, void *checkval, bool calcnot, bool (*chkcond) (void *checkval, ITEM * item))
{

	if (curitem->type == VAL)
		return (*chkcond) (checkval, curitem);
	else if (curitem->val == (int4) '!')
	{
		return (calcnot) ?
			((execute(curitem - 1, checkval, calcnot, chkcond)) ? false : true)
			: true;
	}
	else if (curitem->val == (int4) '&')
	{
		if (execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return execute(curitem - 1, checkval, calcnot, chkcond);
		else
			return false;
	}
	else
	{							/* |-operator */
		if (execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return true;
		else
			return execute(curitem - 1, checkval, calcnot, chkcond);
	}
	return false;
}

/*
 * signconsistent & execconsistent called by *_consistent
 */
bool
signconsistent(QUERYTYPE * query, BITVEC sign, bool calcnot)
{
	return execute(
				   GETQUERY(query) + query->size - 1,
				   (void *) sign, calcnot,
				   checkcondition_bit
		);
}

bool
execconsistent(QUERYTYPE * query, ArrayType *array, bool calcnot)
{
	CHKVAL		chkval;

	CHECKARRVALID(array);
	chkval.arrb = ARRPTR(array);
	chkval.arre = chkval.arrb + ARRNELEMS(array);
	return execute(
				   GETQUERY(query) + query->size - 1,
				   (void *) &chkval, calcnot,
				   checkcondition_arr
		);
}

typedef struct
{
	ITEM	   *first;
	bool	   *mapped_check;
}	GinChkVal;

static bool
checkcondition_gin(void *checkval, ITEM * item)
{
	GinChkVal  *gcv = (GinChkVal *) checkval;

	return gcv->mapped_check[item - gcv->first];
}

bool
ginconsistent(QUERYTYPE * query, bool *check)
{
	GinChkVal	gcv;
	ITEM	   *items = GETQUERY(query);
	int			i,
				j = 0;

	if (query->size < 0)
		return FALSE;

	gcv.first = items;
	gcv.mapped_check = (bool *) palloc(sizeof(bool) * query->size);
	for (i = 0; i < query->size; i++)
		if (items[i].type == VAL)
			gcv.mapped_check[i] = check[j++];

	return execute(
				   GETQUERY(query) + query->size - 1,
				   (void *) &gcv, true,
				   checkcondition_gin
		);
}

/*
 * boolean operations
 */
Datum
rboolop(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(
							   boolop,
							   PG_GETARG_DATUM(1),
							   PG_GETARG_DATUM(0)
		);
}

Datum
boolop(PG_FUNCTION_ARGS)
{
	ArrayType  *val = (ArrayType *) PG_DETOAST_DATUM_COPY(PG_GETARG_POINTER(0));
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_POINTER(1));
	CHKVAL		chkval;
	bool		result;

	CHECKARRVALID(val);
	if (ARRISVOID(val))
	{
		pfree(val);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(false);
	}

	PREPAREARR(val);
	chkval.arrb = ARRPTR(val);
	chkval.arre = chkval.arrb + ARRNELEMS(val);
	result = execute(
					 GETQUERY(query) + query->size - 1,
					 &chkval, true,
					 checkcondition_arr
		);
	pfree(val);

	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

static void
findoprnd(ITEM * ptr, int4 *pos)
{
#ifdef BS_DEBUG
	elog(DEBUG3, (ptr[*pos].type == OPR) ?
		 "%d  %c" : "%d  %d", *pos, ptr[*pos].val);
#endif
	if (ptr[*pos].type == VAL)
	{
		ptr[*pos].left = 0;
		(*pos)--;
	}
	else if (ptr[*pos].val == (int4) '!')
	{
		ptr[*pos].left = -1;
		(*pos)--;
		findoprnd(ptr, pos);
	}
	else
	{
		ITEM	   *curitem = &ptr[*pos];
		int4		tmp = *pos;

		(*pos)--;
		findoprnd(ptr, pos);
		curitem->left = *pos - tmp;
		findoprnd(ptr, pos);
	}
}


/*
 * input
 */
Datum
bqarr_in(PG_FUNCTION_ARGS)
{
	char	   *buf = (char *) PG_GETARG_POINTER(0);
	WORKSTATE	state;
	int4		i;
	QUERYTYPE  *query;
	int4		commonlen;
	ITEM	   *ptr;
	NODE	   *tmp;
	int4		pos = 0;

#ifdef BS_DEBUG
	StringInfoData pbuf;
#endif

	state.buf = buf;
	state.state = WAITOPERAND;
	state.count = 0;
	state.num = 0;
	state.str = NULL;

	/* make polish notation (postfix, but in reverse order) */
	makepol(&state);
	if (!state.num)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty query")));

	commonlen = COMPUTESIZE(state.num);
	query = (QUERYTYPE *) palloc(commonlen);
	query->len = commonlen;
	query->size = state.num;
	ptr = GETQUERY(query);

	for (i = state.num - 1; i >= 0; i--)
	{
		ptr[i].type = state.str->type;
		ptr[i].val = state.str->val;
		tmp = state.str->next;
		pfree(state.str);
		state.str = tmp;
	}

	pos = query->size - 1;
	findoprnd(ptr, &pos);
#ifdef BS_DEBUG
	initStringInfo(&pbuf);
	for (i = 0; i < query->size; i++)
	{
		if (ptr[i].type == OPR)
			appendStringInfo(&pbuf, "%c(%d) ", ptr[i].val, ptr[i].left);
		else
			appendStringInfo(&pbuf, "%d ", ptr[i].val);
	}
	elog(DEBUG3, "POR: %s", pbuf.data);
	pfree(pbuf.data);
#endif

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
	int4		buflen;
}	INFIX;

#define RESIZEBUF(inf,addsize) while( ( (inf)->cur - (inf)->buf ) + (addsize) + 1 >= (inf)->buflen ) { \
	int4 len = inf->cur - inf->buf; \
	inf->buflen *= 2; \
	inf->buf = (char*) repalloc( (void*)inf->buf, inf->buflen ); \
	inf->cur = inf->buf + len; \
}

static void
infix(INFIX * in, bool first)
{
	if (in->curpol->type == VAL)
	{
		RESIZEBUF(in, 11);
		sprintf(in->cur, "%d", in->curpol->val);
		in->cur = strchr(in->cur, '\0');
		in->curpol--;
	}
	else if (in->curpol->val == (int4) '!')
	{
		bool		isopr = false;

		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol--;
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

		in->curpol--;
		if (op == (int4) '|' && !first)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}

		nrm.curpol = in->curpol;
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
bqarr_out(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_POINTER(0));
	INFIX		nrm;

	if (query->size == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty query")));

	nrm.curpol = GETQUERY(query) + query->size - 1;
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
	*(nrm.cur) = '\0';
	infix(&nrm, true);

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_POINTER(nrm.buf);
}

static int4
countdroptree(ITEM * q, int4 pos)
{
	if (q[pos].type == VAL)
		return 1;
	else if (q[pos].val == (int4) '!')
		return 1 + countdroptree(q, pos - 1);
	else
		return 1 + countdroptree(q, pos - 1) + countdroptree(q, pos + q[pos].left);
}

/*
 * common algorithm:
 * result of all '!' will be = 'true', so
 * we can modify query tree for clearing
 */
int4
shorterquery(ITEM * q, int4 len)
{
	int4		index,
				posnot,
				poscor;
	bool		notisleft = false;
	int4		drop,
				i;

	/* out all '!' */
	do
	{
		index = 0;
		drop = 0;
		/* find ! */
		for (posnot = 0; posnot < len; posnot++)
			if (q[posnot].type == OPR && q[posnot].val == (int4) '!')
			{
				index = 1;
				break;
			}

		if (posnot == len)
			return len;

		/* last operator is ! */
		if (posnot == len - 1)
			return 0;

		/* find operator for this operand */
		for (poscor = posnot + 1; poscor < len; poscor++)
		{
			if (q[poscor].type == OPR)
			{
				if (poscor == posnot + 1)
				{
					notisleft = false;
					break;
				}
				else if (q[poscor].left + poscor == posnot)
				{
					notisleft = true;
					break;
				}
			}
		}
		if (q[poscor].val == (int4) '!')
		{
			drop = countdroptree(q, poscor);
			q[poscor - 1].type = VAL;
			for (i = poscor + 1; i < len; i++)
				if (q[i].type == OPR && q[i].left + i <= poscor)
					q[i].left += drop - 2;
			memcpy((void *) &q[poscor - drop + 1],
				   (void *) &q[poscor - 1],
				   sizeof(ITEM) * (len - (poscor - 1)));
			len -= drop - 2;
		}
		else if (q[poscor].val == (int4) '|')
		{
			drop = countdroptree(q, poscor);
			q[poscor - 1].type = VAL;
			q[poscor].val = (int4) '!';
			q[poscor].left = -1;
			for (i = poscor + 1; i < len; i++)
				if (q[i].type == OPR && q[i].left + i < poscor)
					q[i].left += drop - 2;
			memcpy((void *) &q[poscor - drop + 1],
				   (void *) &q[poscor - 1],
				   sizeof(ITEM) * (len - (poscor - 1)));
			len -= drop - 2;
		}
		else
		{						/* &-operator */
			if (
				(notisleft && q[poscor - 1].type == OPR &&
				 q[poscor - 1].val == (int4) '!') ||
				(!notisleft && q[poscor + q[poscor].left].type == OPR &&
				 q[poscor + q[poscor].left].val == (int4) '!')
				)
			{					/* drop subtree */
				drop = countdroptree(q, poscor);
				q[poscor - 1].type = VAL;
				q[poscor].val = (int4) '!';
				q[poscor].left = -1;
				for (i = poscor + 1; i < len; i++)
					if (q[i].type == OPR && q[i].left + i < poscor)
						q[i].left += drop - 2;
				memcpy((void *) &q[poscor - drop + 1],
					   (void *) &q[poscor - 1],
					   sizeof(ITEM) * (len - (poscor - 1)));
				len -= drop - 2;
			}
			else
			{					/* drop only operator */
				int4		subtreepos = (notisleft) ?
				poscor - 1 : poscor + q[poscor].left;
				int4		subtreelen = countdroptree(q, subtreepos);

				drop = countdroptree(q, poscor);
				for (i = poscor + 1; i < len; i++)
					if (q[i].type == OPR && q[i].left + i < poscor)
						q[i].left += drop - subtreelen;
				memcpy((void *) &q[subtreepos + 1],
					   (void *) &q[poscor + 1],
					   sizeof(ITEM) * (len - (poscor - 1)));
				memcpy((void *) &q[poscor - drop + 1],
					   (void *) &q[subtreepos - subtreelen + 1],
					   sizeof(ITEM) * (len - (drop - subtreelen)));
				len -= drop - subtreelen;
			}
		}
	} while (index);
	return len;
}


Datum
querytree(PG_FUNCTION_ARGS)
{
	QUERYTYPE  *query = (QUERYTYPE *) PG_DETOAST_DATUM(PG_GETARG_POINTER(0));
	INFIX		nrm;
	text	   *res;
	ITEM	   *q;
	int4		len;

	if (query->size == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty query")));

	q = (ITEM *) palloc(sizeof(ITEM) * query->size);
	memcpy((void *) q, GETQUERY(query), sizeof(ITEM) * query->size);
	len = shorterquery(q, query->size);
	PG_FREE_IF_COPY(query, 0);

	if (len == 0)
	{
		res = (text *) palloc(1 + VARHDRSZ);
		VARATT_SIZEP(res) = 1 + VARHDRSZ;
		*((char *) VARDATA(res)) = 'T';
	}
	else
	{
		nrm.curpol = q + len - 1;
		nrm.buflen = 32;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
		*(nrm.cur) = '\0';
		infix(&nrm, true);

		res = (text *) palloc(nrm.cur - nrm.buf + VARHDRSZ);
		VARATT_SIZEP(res) = nrm.cur - nrm.buf + VARHDRSZ;
		strncpy(VARDATA(res), nrm.buf, nrm.cur - nrm.buf);
	}
	pfree(q);

	PG_RETURN_POINTER(res);
}
