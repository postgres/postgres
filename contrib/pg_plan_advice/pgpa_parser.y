%{
/*
 * Parser for plan advice
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * contrib/pg_plan_advice/pgpa_parser.y
 */

#include "postgres.h"

#include <float.h>
#include <math.h>

#include "fmgr.h"
#include "nodes/miscnodes.h"
#include "utils/builtins.h"
#include "utils/float.h"

#include "pgpa_ast.h"
#include "pgpa_parser.h"

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.
 */
#define YYMALLOC palloc
#define YYFREE   pfree
%}

/* BISON Declarations */
%parse-param {List **result}
%parse-param {char **parse_error_msg_p}
%parse-param {yyscan_t yyscanner}
%lex-param {List **result}
%lex-param {char **parse_error_msg_p}
%lex-param {yyscan_t yyscanner}
%pure-parser
%expect 0
%name-prefix="pgpa_yy"

%union
{
	char	   *str;
	int			integer;
	List	   *list;
	pgpa_advice_item *item;
	pgpa_advice_target *target;
	pgpa_index_target *itarget;
}
%token <str> TOK_IDENT TOK_TAG_JOIN_ORDER TOK_TAG_INDEX
%token <str> TOK_TAG_SIMPLE TOK_TAG_GENERIC
%token <integer> TOK_INTEGER

%type <integer> opt_ri_occurrence
%type <item> advice_item
%type <list> advice_item_list generic_target_list
%type <list> index_target_list join_order_target_list
%type <list> opt_partition simple_target_list
%type <str> identifier opt_plan_name
%type <target> generic_sublist join_order_sublist
%type <target> relation_identifier
%type <itarget> index_name

%start parse_toplevel

/* Grammar follows */
%%

parse_toplevel: advice_item_list
		{
			(void) yynerrs;				/* suppress compiler warning */
			*result = $1;
		}
	;

advice_item_list: advice_item_list advice_item
		{ $$ = lappend($1, $2); }
	|
		{ $$ = NIL; }
	;

advice_item: TOK_TAG_JOIN_ORDER '(' join_order_target_list ')'
		{
			$$ = palloc0_object(pgpa_advice_item);
			$$->tag = PGPA_TAG_JOIN_ORDER;
			$$->targets = $3;
			if ($3 == NIL)
				pgpa_yyerror(result, parse_error_msg_p, yyscanner,
							 "JOIN_ORDER must have at least one target");
		}
	| TOK_TAG_INDEX '(' index_target_list ')'
		{
			$$ = palloc0_object(pgpa_advice_item);
			if (strcmp($1, "index_only_scan") == 0)
				$$->tag = PGPA_TAG_INDEX_ONLY_SCAN;
			else if (strcmp($1, "index_scan") == 0)
				$$->tag = PGPA_TAG_INDEX_SCAN;
			else
				elog(ERROR, "tag parsing failed: %s", $1);
			$$->targets = $3;
		}
	| TOK_TAG_SIMPLE '(' simple_target_list ')'
		{
			$$ = palloc0_object(pgpa_advice_item);
			if (strcmp($1, "bitmap_heap_scan") == 0)
				$$->tag = PGPA_TAG_BITMAP_HEAP_SCAN;
			else if (strcmp($1, "no_gather") == 0)
				$$->tag = PGPA_TAG_NO_GATHER;
			else if (strcmp($1, "seq_scan") == 0)
				$$->tag = PGPA_TAG_SEQ_SCAN;
			else if (strcmp($1, "tid_scan") == 0)
				$$->tag = PGPA_TAG_TID_SCAN;
			else
				elog(ERROR, "tag parsing failed: %s", $1);
			$$->targets = $3;
		}
	| TOK_TAG_GENERIC '(' generic_target_list ')'
		{
			bool	fail;

			$$ = palloc0_object(pgpa_advice_item);
			$$->tag = pgpa_parse_advice_tag($1, &fail);
			if (fail)
			{
				pgpa_yyerror(result, parse_error_msg_p, yyscanner,
							 "unrecognized advice tag");
			}

			if ($$->tag == PGPA_TAG_FOREIGN_JOIN)
			{
				foreach_ptr(pgpa_advice_target, target, $3)
				{
					if (target->ttype == PGPA_TARGET_IDENTIFIER ||
						list_length(target->children) == 1)
							pgpa_yyerror(result, parse_error_msg_p, yyscanner,
										 "FOREIGN_JOIN targets must contain more than one relation identifier");
				}
			}

			$$->targets = $3;
		}
	;

relation_identifier: identifier opt_ri_occurrence opt_partition opt_plan_name
		{
			$$ = palloc0_object(pgpa_advice_target);
			$$->ttype = PGPA_TARGET_IDENTIFIER;
			$$->rid.alias_name = $1;
			$$->rid.occurrence = $2;
			if (list_length($3) == 2)
			{
				$$->rid.partnsp = linitial($3);
				$$->rid.partrel = lsecond($3);
			}
			else if ($3 != NIL)
				$$->rid.partrel = linitial($3);
			$$->rid.plan_name = $4;
		}
	;

index_name: identifier
		{
			$$ = palloc0_object(pgpa_index_target);
			$$->indname = $1;
		}
	| identifier '.' identifier
		{
			$$ = palloc0_object(pgpa_index_target);
			$$->indnamespace = $1;
			$$->indname = $3;
		}
	;

opt_ri_occurrence:
	'#' TOK_INTEGER
		{
			if ($2 <= 0)
				pgpa_yyerror(result, parse_error_msg_p, yyscanner,
							 "only positive occurrence numbers are permitted");
			$$ = $2;
		}
	|
		{
			/* The default occurrence number is 1. */
			$$ = 1;
		}
	;

identifier: TOK_IDENT
	| TOK_TAG_JOIN_ORDER
	| TOK_TAG_INDEX
	| TOK_TAG_SIMPLE
	| TOK_TAG_GENERIC
	;

/*
 * When generating advice, we always schema-qualify the partition name, but
 * when parsing advice, we accept a specification that lacks one.
 */
opt_partition:
	'/' identifier '.' identifier
		{ $$ = list_make2($2, $4); }
	| '/' identifier
		{ $$ = list_make1($2); }
	|
		{ $$ = NIL; }
	;

opt_plan_name:
	'@' identifier
		{ $$ = $2; }
	|
		{ $$ = NULL; }
	;

generic_target_list: generic_target_list relation_identifier
		{ $$ = lappend($1, $2); }
	| generic_target_list generic_sublist
		{ $$ = lappend($1, $2); }
	|
		{ $$ = NIL; }
	;

generic_sublist: '(' simple_target_list ')'
		{
			$$ = palloc0_object(pgpa_advice_target);
			$$->ttype = PGPA_TARGET_ORDERED_LIST;
			$$->children = $2;
		}
	;

index_target_list:
	  index_target_list relation_identifier index_name
		{
			$2->itarget = $3;
			$$ = lappend($1, $2);
		}
	|
		{ $$ = NIL; }
	;

join_order_target_list: join_order_target_list relation_identifier
		{ $$ = lappend($1, $2); }
	| join_order_target_list join_order_sublist
		{ $$ = lappend($1, $2); }
	|
		{ $$ = NIL; }
	;

join_order_sublist:
	'(' join_order_target_list ')'
		{
			$$ = palloc0_object(pgpa_advice_target);
			$$->ttype = PGPA_TARGET_ORDERED_LIST;
			$$->children = $2;
		}
	| '{' simple_target_list '}'
		{
			$$ = palloc0_object(pgpa_advice_target);
			$$->ttype = PGPA_TARGET_UNORDERED_LIST;
			$$->children = $2;
		}
	;

simple_target_list: simple_target_list relation_identifier
		{ $$ = lappend($1, $2); }
	|
		{ $$ = NIL; }
	;

%%

/*
 * Parse an advice_string and return the resulting list of pgpa_advice_item
 * objects. If a parse error occurs, instead return NULL.
 *
 * If the return value is NULL, *error_p will be set to the error message;
 * otherwise, *error_p will be set to NULL.
 */
List *
pgpa_parse(const char *advice_string, char **error_p)
{
	yyscan_t	scanner;
	List	   *result;
	char	   *error = NULL;

	pgpa_scanner_init(advice_string, &scanner);
	pgpa_yyparse(&result, &error, scanner);
	pgpa_scanner_finish(scanner);

	if (error != NULL)
	{
		*error_p = error;
		return NULL;
	}

	*error_p = NULL;
	return result;
}
