/*-------------------------------------------------------------------------
 *
 * parser.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parser.c,v 1.33 1998/02/26 04:33:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdio.h>

#include "postgres.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_node.h"
#include "parser/parser.h"

char	   *parseString;		/* the char* which holds the string to be
								 * parsed */
List	   *parsetree = NIL;

#ifdef SETS_FIXED
static void fixupsets();
static void define_sets();

#endif
/*
 * parser-- returns a list of parse trees
 *
 *	CALLER is responsible for free'ing the list returned
 */
QueryTreeList *
parser(char *str, Oid *typev, int nargs)
{
	QueryTreeList *queryList;
	int			yyresult;

#if defined(FLEX_SCANNER)
	extern void DeleteBuffer(void);

#endif							/* FLEX_SCANNER */

	init_io();

	parseString = pstrdup(str);

	parser_init(typev, nargs);
	yyresult = yyparse();

#if defined(FLEX_SCANNER)
	DeleteBuffer();
#endif							/* FLEX_SCANNER */

	clearerr(stdin);

	if (yyresult)				/* error */
		return ((QueryTreeList *) NULL);

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
	{
		return;
	}
	else if (IsA(clause, LispList))
	{
		define_sets(lfirst(clause));
		define_sets(lnext(clause));
	}
	else if (IsA(clause, Const))
	{
		if (get_constisnull((Const) clause) ||
			!get_constisset((Const) clause))
		{
			return;
		}
		setoid = SetDefine(((Const *) clause)->constvalue,
						   typeidTypeName(((Const *) clause)->consttype));
		set_constvalue((Const) clause, setoid);
		set_consttype((Const) clause, typeoid);
		set_constlen((Const) clause, oidsize);
		set_constypeByVal((Const) clause, oidbyval);
	}
	else if (IsA(clause, Iter))
	{
		define_sets(((Iter *) clause)->iterexpr);
	}
	else if (single_node(clause))
	{
		return;
	}
	else if (or_clause(clause) || and_clause(clause))
	{
		List	   *temp;

		/* mapcan */
		foreach(temp, ((Expr *) clause)->args)
		{
			define_sets(lfirst(temp));
		}
	}
	else if (is_funcclause(clause))
	{
		List	   *temp;

		/* mapcan */
		foreach(temp, ((Expr *) clause)->args)
		{
			define_sets(lfirst(temp));
		}
	}
	else if (IsA(clause, ArrayRef))
	{
		define_sets(((ArrayRef *) clause)->refassgnexpr);
	}
	else if (not_clause(clause))
	{
		define_sets(get_notclausearg(clause));
	}
	else if (is_opclause(clause))
	{
		define_sets(get_leftop(clause));
		define_sets(get_rightop(clause));
	}
}

#endif
