/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * parse_analyze.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parser.c,v 1.56 2003/04/27 20:09:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "parser/gramparse.h"
#include "parser/parse.h"
#include "parser/parser.h"
#include "parser/parse_expr.h"


List	   *parsetree;			/* result of parsing is left here */

static Oid *param_type_info;	/* state for param_type() */
static int	param_count;

static int	lookahead_token;	/* one-token lookahead */
static bool have_lookahead;		/* lookahead_token set? */


/*
 * parser
 *		Given a query in string form, and optionally info about
 *		parameter types, do lexical and syntactic analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.
 */
List *
parser(const char *str, Oid *typev, int nargs)
{
	int			yyresult;

	parsetree = NIL;			/* in case parser forgets to set it */
	have_lookahead = false;

	scanner_init(str);
	parser_init();
	parse_expr_init();
	parser_param_set(typev, nargs);

	yyresult = yyparse();

	scanner_finish();
	clearerr(stdin);

	if (yyresult)				/* error */
		return NIL;

	return parsetree;
}


/*
 * Save information needed to fill out the type of Param references ($n)
 *
 * This is used for SQL functions, PREPARE statements, etc.  It's split
 * out from parser() setup because PREPARE needs to change the info after
 * the grammar runs and before parse analysis is done on the preparable
 * query.
 */
void
parser_param_set(Oid *typev, int nargs)
{
	param_type_info = typev;
	param_count = nargs;
}

/*
 * param_type()
 *
 * Fetch a parameter type previously passed to parser_param_set
 */
Oid
param_type(int t)
{
	if (t > param_count || t <= 0)
		return InvalidOid;
	return param_type_info[t - 1];
}


/*
 * Intermediate filter between parser and base lexer (base_yylex in scan.l).
 *
 * The filter is needed because in some cases SQL92 requires more than one
 * token lookahead.  We reduce these cases to one-token lookahead by combining
 * tokens here, in order to keep the grammar LR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words ...
 */
int
yylex(void)
{
	int			cur_token;

	/* Get next token --- we might already have it */
	if (have_lookahead)
	{
		cur_token = lookahead_token;
		have_lookahead = false;
	}
	else
		cur_token = base_yylex();

	/* Do we need to look ahead for a possible multiword token? */
	switch (cur_token)
	{
		case UNION:
			/* UNION JOIN must be reduced to a single UNIONJOIN token */
			lookahead_token = base_yylex();
			if (lookahead_token == JOIN)
				cur_token = UNIONJOIN;
			else
				have_lookahead = true;
			break;

		default:
			break;
	}

	return cur_token;
}
