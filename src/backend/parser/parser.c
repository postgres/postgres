/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL parser
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parser.c,v 1.46 2000/09/12 21:07:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse.h"
#include "parser/parser.h"
#include "parser/parse_expr.h"


#if defined(FLEX_SCANNER)
extern void DeleteBuffer(void);
#endif	 /* FLEX_SCANNER */

char	   *parseString;		/* the char* which holds the string to be
								 * parsed */
List	   *parsetree;			/* result of parsing is left here */

static int	lookahead_token;	/* one-token lookahead */
static bool have_lookahead;		/* lookahead_token set? */

#ifdef SETS_FIXED
static void fixupsets();
static void define_sets();

#endif

/*
 * parser-- returns a list of parse trees
 */
List *
parser(char *str, Oid *typev, int nargs)
{
	List	   *queryList;
	int			yyresult;

	parseString = str;
	parsetree = NIL;			/* in case parser forgets to set it */
	have_lookahead = false;

	scanner_init();
	parser_init(typev, nargs);
	parse_expr_init();

	yyresult = yyparse();

#if defined(FLEX_SCANNER)
	DeleteBuffer();
#endif	 /* FLEX_SCANNER */

	clearerr(stdin);

	if (yyresult)				/* error */
		return (List *) NULL;

	queryList = parse_analyze(parsetree, NULL);

#ifdef SETS_FIXED

	/*
	 * Fixing up sets calls the parser, so it reassigns the global
	 * variable parsetree. So save the real parsetree.
	 */
	savetree = parsetree;
	foreach(parse, savetree)
	{							/* savetree is really a list of parses */

		/* find set definitions embedded in query */
		fixupsets((Query *) lfirst(parse));

	}
	return savetree;
#endif

	return queryList;
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


#ifdef SETS_FIXED
static void
fixupsets(Query *parse)
{
	if (parse == NULL)
		return;
	if (parse->commandType == CMD_UTILITY)		/* utility */
		return;
	if (parse->commandType != CMD_INSERT)
		return;
	define_sets(parse);
}

/* Recursively find all of the Consts in the parsetree.  Some of
 * these may represent a set.  The value of the Const will be the
 * query (a string) which defines the set.	Call SetDefine to define
 * the set, and store the OID of the new set in the Const instead.
 */
static void
define_sets(Node *clause)
{
	Oid			setoid;
	Type		t = typeidType(OIDOID);
	Oid			typeoid = typeTypeId(t);
	Size		oidsize = typeLen(t);
	bool		oidbyval = typeByVal(t);

	if (clause == NULL)
		return;
	else if (IsA(clause, LispList))
	{
		define_sets(lfirst(clause));
		define_sets(lnext(clause));
	}
	else if (IsA(clause, Const))
	{
		if (get_constisnull((Const) clause) ||
			!get_constisset((Const) clause))
			return;
		setoid = SetDefine(((Const *) clause)->constvalue,
						   typeidTypeName(((Const *) clause)->consttype));
		set_constvalue((Const) clause, setoid);
		set_consttype((Const) clause, typeoid);
		set_constlen((Const) clause, oidsize);
		set_constypeByVal((Const) clause, oidbyval);
	}
	else if (IsA(clause, Iter))
		define_sets(((Iter *) clause)->iterexpr);
	else if (single_node(clause))
		return;
	else if (or_clause(clause) || and_clause(clause))
	{
		List	   *temp;

		/* mapcan */
		foreach(temp, ((Expr *) clause)->args)
			define_sets(lfirst(temp));
	}
	else if (is_funcclause(clause))
	{
		List	   *temp;

		/* mapcan */
		foreach(temp, ((Expr *) clause)->args)
			define_sets(lfirst(temp));
	}
	else if (IsA(clause, ArrayRef))
		define_sets(((ArrayRef *) clause)->refassgnexpr);
	else if (not_clause(clause))
		define_sets(get_notclausearg(clause));
	else if (is_opclause(clause))
	{
		define_sets(get_leftop(clause));
		define_sets(get_rightop(clause));
	}
}

#endif
