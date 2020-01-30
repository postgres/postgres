/*
 * txtquery operations with ltree
 * Teodor Sigaev <teodor@stack.net>
 * contrib/ltree/ltxtquery_op.c
 */
#include "postgres.h"

#include <ctype.h>

#include "ltree.h"
#include "miscadmin.h"

PG_FUNCTION_INFO_V1(ltxtq_exec);
PG_FUNCTION_INFO_V1(ltxtq_rexec);

/*
 * check for boolean condition
 */
bool
ltree_execute(ITEM *curitem, void *checkval, bool calcnot, bool (*chkcond) (void *checkval, ITEM *val))
{
	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (curitem->type == VAL)
		return (*chkcond) (checkval, curitem);
	else if (curitem->val == (int32) '!')
	{
		return calcnot ?
			((ltree_execute(curitem + 1, checkval, calcnot, chkcond)) ? false : true)
			: true;
	}
	else if (curitem->val == (int32) '&')
	{
		if (ltree_execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return ltree_execute(curitem + 1, checkval, calcnot, chkcond);
		else
			return false;
	}
	else
	{							/* |-operator */
		if (ltree_execute(curitem + curitem->left, checkval, calcnot, chkcond))
			return true;
		else
			return ltree_execute(curitem + 1, checkval, calcnot, chkcond);
	}
}

typedef struct
{
	ltree	   *node;
	char	   *operand;
} CHKVAL;

static bool
checkcondition_str(void *checkval, ITEM *val)
{
	ltree_level *level = LTREE_FIRST(((CHKVAL *) checkval)->node);
	int			tlen = ((CHKVAL *) checkval)->node->numlevel;
	char	   *op = ((CHKVAL *) checkval)->operand + val->distance;
	int			(*cmpptr) (const char *, const char *, size_t);

	cmpptr = (val->flag & LVAR_INCASE) ? ltree_strncasecmp : strncmp;
	while (tlen > 0)
	{
		if (val->flag & LVAR_SUBLEXEME)
		{
			if (compare_subnode(level, op, val->length, cmpptr, (val->flag & LVAR_ANYEND)))
				return true;
		}
		else if ((val->length == level->len ||
				  (level->len > val->length && (val->flag & LVAR_ANYEND))) &&
				 (*cmpptr) (op, level->name, val->length) == 0)
			return true;

		tlen--;
		level = LEVEL_NEXT(level);
	}

	return false;
}

Datum
ltxtq_exec(PG_FUNCTION_ARGS)
{
	ltree	   *val = PG_GETARG_LTREE_P(0);
	ltxtquery  *query = PG_GETARG_LTXTQUERY_P(1);
	CHKVAL		chkval;
	bool		result;

	chkval.node = val;
	chkval.operand = GETOPERAND(query);

	result = ltree_execute(GETQUERY(query),
						   &chkval,
						   true,
						   checkcondition_str);

	PG_FREE_IF_COPY(val, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

Datum
ltxtq_rexec(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(ltxtq_exec,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										));
}
