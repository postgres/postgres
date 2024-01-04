%{
/*-------------------------------------------------------------------------
 *
 * exprparse.y
 *	  bison grammar for a simple expression syntax
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/exprparse.y
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "pgbench.h"

#define PGBENCH_NARGS_VARIABLE	(-1)
#define PGBENCH_NARGS_CASE		(-2)
#define PGBENCH_NARGS_HASH		(-3)
#define PGBENCH_NARGS_PERMUTE	(-4)

PgBenchExpr *expr_parse_result;

static PgBenchExprList *make_elist(PgBenchExpr *expr, PgBenchExprList *list);
static PgBenchExpr *make_null_constant(void);
static PgBenchExpr *make_boolean_constant(bool bval);
static PgBenchExpr *make_integer_constant(int64 ival);
static PgBenchExpr *make_double_constant(double dval);
static PgBenchExpr *make_variable(char *varname);
static PgBenchExpr *make_op(yyscan_t yyscanner, const char *operator,
							PgBenchExpr *lexpr, PgBenchExpr *rexpr);
static PgBenchExpr *make_uop(yyscan_t yyscanner, const char *operator, PgBenchExpr *expr);
static int	find_func(yyscan_t yyscanner, const char *fname);
static PgBenchExpr *make_func(yyscan_t yyscanner, int fnumber, PgBenchExprList *args);
static PgBenchExpr *make_case(yyscan_t yyscanner, PgBenchExprList *when_then_list, PgBenchExpr *else_part);

%}

%pure-parser
%expect 0
%name-prefix="expr_yy"

%parse-param {yyscan_t yyscanner}
%lex-param   {yyscan_t yyscanner}

%union
{
	int64		ival;
	double		dval;
	bool		bval;
	char	   *str;
	PgBenchExpr *expr;
	PgBenchExprList *elist;
}

%type <elist> elist when_then_list
%type <expr> expr case_control
%type <ival> INTEGER_CONST function
%type <dval> DOUBLE_CONST
%type <bval> BOOLEAN_CONST
%type <str> VARIABLE FUNCTION

%token NULL_CONST INTEGER_CONST MAXINT_PLUS_ONE_CONST DOUBLE_CONST
%token BOOLEAN_CONST VARIABLE FUNCTION
%token AND_OP OR_OP NOT_OP NE_OP LE_OP GE_OP LS_OP RS_OP IS_OP
%token CASE_KW WHEN_KW THEN_KW ELSE_KW END_KW

/* Precedence: lowest to highest, taken from postgres SQL parser */
%left	OR_OP
%left	AND_OP
%right  NOT_OP
%nonassoc IS_OP ISNULL_OP NOTNULL_OP
%nonassoc '<' '>' '=' LE_OP GE_OP NE_OP
%left   '|' '#' '&' LS_OP RS_OP '~'
%left	'+' '-'
%left	'*' '/' '%'
%right	UNARY

%%

result: expr				{
								expr_parse_result = $1;
								(void) yynerrs; /* suppress compiler warning */
							}

elist:						{ $$ = NULL; }
	| expr					{ $$ = make_elist($1, NULL); }
	| elist ',' expr		{ $$ = make_elist($3, $1); }
	;

expr: '(' expr ')'			{ $$ = $2; }
	| '+' expr %prec UNARY	{ $$ = $2; }
	/* unary minus "-x" implemented as "0 - x" */
	| '-' expr %prec UNARY	{ $$ = make_op(yyscanner, "-",
										   make_integer_constant(0), $2); }
	/* special PG_INT64_MIN handling, only after a unary minus */
	| '-' MAXINT_PLUS_ONE_CONST %prec UNARY
							{ $$ = make_integer_constant(PG_INT64_MIN); }
	/* binary ones complement "~x" implemented as 0xffff... xor x" */
	| '~' expr				{ $$ = make_op(yyscanner, "#",
										   make_integer_constant(~INT64CONST(0)), $2); }
	| NOT_OP expr			{ $$ = make_uop(yyscanner, "!not", $2); }
	| expr '+' expr			{ $$ = make_op(yyscanner, "+", $1, $3); }
	| expr '-' expr			{ $$ = make_op(yyscanner, "-", $1, $3); }
	| expr '*' expr			{ $$ = make_op(yyscanner, "*", $1, $3); }
	| expr '/' expr			{ $$ = make_op(yyscanner, "/", $1, $3); }
	| expr '%' expr			{ $$ = make_op(yyscanner, "mod", $1, $3); }
	| expr '<' expr			{ $$ = make_op(yyscanner, "<", $1, $3); }
	| expr LE_OP expr		{ $$ = make_op(yyscanner, "<=", $1, $3); }
	| expr '>' expr			{ $$ = make_op(yyscanner, "<", $3, $1); }
	| expr GE_OP expr		{ $$ = make_op(yyscanner, "<=", $3, $1); }
	| expr '=' expr			{ $$ = make_op(yyscanner, "=", $1, $3); }
	| expr NE_OP expr		{ $$ = make_op(yyscanner, "<>", $1, $3); }
	| expr '&' expr			{ $$ = make_op(yyscanner, "&", $1, $3); }
	| expr '|' expr			{ $$ = make_op(yyscanner, "|", $1, $3); }
	| expr '#' expr			{ $$ = make_op(yyscanner, "#", $1, $3); }
	| expr LS_OP expr		{ $$ = make_op(yyscanner, "<<", $1, $3); }
	| expr RS_OP expr		{ $$ = make_op(yyscanner, ">>", $1, $3); }
	| expr AND_OP expr		{ $$ = make_op(yyscanner, "!and", $1, $3); }
	| expr OR_OP expr		{ $$ = make_op(yyscanner, "!or", $1, $3); }
	/* IS variants */
	| expr ISNULL_OP		{ $$ = make_op(yyscanner, "!is", $1, make_null_constant()); }
	| expr NOTNULL_OP		{
								$$ = make_uop(yyscanner, "!not",
											  make_op(yyscanner, "!is", $1, make_null_constant()));
							}
	| expr IS_OP NULL_CONST	{ $$ = make_op(yyscanner, "!is", $1, make_null_constant()); }
	| expr IS_OP NOT_OP NULL_CONST
							{
								$$ = make_uop(yyscanner, "!not",
											  make_op(yyscanner, "!is", $1, make_null_constant()));
							}
	| expr IS_OP BOOLEAN_CONST
							{
								$$ = make_op(yyscanner, "!is", $1, make_boolean_constant($3));
							}
	| expr IS_OP NOT_OP BOOLEAN_CONST
							{
								$$ = make_uop(yyscanner, "!not",
											  make_op(yyscanner, "!is", $1, make_boolean_constant($4)));
							}
	/* constants */
	| NULL_CONST			{ $$ = make_null_constant(); }
	| BOOLEAN_CONST			{ $$ = make_boolean_constant($1); }
	| INTEGER_CONST			{ $$ = make_integer_constant($1); }
	| DOUBLE_CONST			{ $$ = make_double_constant($1); }
	/* misc */
	| VARIABLE				{ $$ = make_variable($1); }
	| function '(' elist ')' { $$ = make_func(yyscanner, $1, $3); }
	| case_control			{ $$ = $1; }
	;

when_then_list:
	  when_then_list WHEN_KW expr THEN_KW expr { $$ = make_elist($5, make_elist($3, $1)); }
	| WHEN_KW expr THEN_KW expr { $$ = make_elist($4, make_elist($2, NULL)); }

case_control:
	  CASE_KW when_then_list END_KW { $$ = make_case(yyscanner, $2, make_null_constant()); }
	| CASE_KW when_then_list ELSE_KW expr END_KW { $$ = make_case(yyscanner, $2, $4); }

function: FUNCTION			{ $$ = find_func(yyscanner, $1); pg_free($1); }
	;

%%

static PgBenchExpr *
make_null_constant(void)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_CONSTANT;
	expr->u.constant.type = PGBT_NULL;
	expr->u.constant.u.ival = 0;
	return expr;
}

static PgBenchExpr *
make_integer_constant(int64 ival)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_CONSTANT;
	expr->u.constant.type = PGBT_INT;
	expr->u.constant.u.ival = ival;
	return expr;
}

static PgBenchExpr *
make_double_constant(double dval)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_CONSTANT;
	expr->u.constant.type = PGBT_DOUBLE;
	expr->u.constant.u.dval = dval;
	return expr;
}

static PgBenchExpr *
make_boolean_constant(bool bval)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_CONSTANT;
	expr->u.constant.type = PGBT_BOOLEAN;
	expr->u.constant.u.bval = bval;
	return expr;
}

static PgBenchExpr *
make_variable(char *varname)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_VARIABLE;
	expr->u.variable.varname = varname;
	return expr;
}

/* binary operators */
static PgBenchExpr *
make_op(yyscan_t yyscanner, const char *operator,
		PgBenchExpr *lexpr, PgBenchExpr *rexpr)
{
	return make_func(yyscanner, find_func(yyscanner, operator),
					 make_elist(rexpr, make_elist(lexpr, NULL)));
}

/* unary operator */
static PgBenchExpr *
make_uop(yyscan_t yyscanner, const char *operator, PgBenchExpr *expr)
{
	return make_func(yyscanner, find_func(yyscanner, operator), make_elist(expr, NULL));
}

/*
 * List of available functions:
 * - fname: function name, "!..." for special internal functions
 * - nargs: number of arguments. Special cases:
 *			- PGBENCH_NARGS_VARIABLE is a special value for least & greatest
 *			  meaning #args >= 1;
 *			- PGBENCH_NARGS_CASE is for the "CASE WHEN ..." function, which
 *			  has #args >= 3 and odd;
 *			- PGBENCH_NARGS_HASH is for hash functions, which have one required
 *			  and one optional argument;
 * - tag: function identifier from PgBenchFunction enum
 */
static const struct
{
	const char *fname;
	int			nargs;
	PgBenchFunction tag;
}	PGBENCH_FUNCTIONS[] =
{
	/* parsed as operators, executed as functions */
	{
		"+", 2, PGBENCH_ADD
	},
	{
		"-", 2, PGBENCH_SUB
	},
	{
		"*", 2, PGBENCH_MUL
	},
	{
		"/", 2, PGBENCH_DIV
	},
	{
		"mod", 2, PGBENCH_MOD
	},
	/* actual functions */
	{
		"abs", 1, PGBENCH_ABS
	},
	{
		"least", PGBENCH_NARGS_VARIABLE, PGBENCH_LEAST
	},
	{
		"greatest", PGBENCH_NARGS_VARIABLE, PGBENCH_GREATEST
	},
	{
		"debug", 1, PGBENCH_DEBUG
	},
	{
		"pi", 0, PGBENCH_PI
	},
	{
		"sqrt", 1, PGBENCH_SQRT
	},
	{
		"ln", 1, PGBENCH_LN
	},
	{
		"exp", 1, PGBENCH_EXP
	},
	{
		"int", 1, PGBENCH_INT
	},
	{
		"double", 1, PGBENCH_DOUBLE
	},
	{
		"random", 2, PGBENCH_RANDOM
	},
	{
		"random_gaussian", 3, PGBENCH_RANDOM_GAUSSIAN
	},
	{
		"random_exponential", 3, PGBENCH_RANDOM_EXPONENTIAL
	},
	{
		"random_zipfian", 3, PGBENCH_RANDOM_ZIPFIAN
	},
	{
		"pow", 2, PGBENCH_POW
	},
	{
		"power", 2, PGBENCH_POW
	},
	/* logical operators */
	{
		"!and", 2, PGBENCH_AND
	},
	{
		"!or", 2, PGBENCH_OR
	},
	{
		"!not", 1, PGBENCH_NOT
	},
	/* bitwise integer operators */
	{
		"&", 2, PGBENCH_BITAND
	},
	{
		"|", 2, PGBENCH_BITOR
	},
	{
		"#", 2, PGBENCH_BITXOR
	},
	{
		"<<", 2, PGBENCH_LSHIFT
	},
	{
		">>", 2, PGBENCH_RSHIFT
	},
	/* comparison operators */
	{
		"=", 2, PGBENCH_EQ
	},
	{
		"<>", 2, PGBENCH_NE
	},
	{
		"<=", 2, PGBENCH_LE
	},
	{
		"<", 2, PGBENCH_LT
	},
	{
		"!is", 2, PGBENCH_IS
	},
	/* "case when ... then ... else ... end" construction */
	{
		"!case_end", PGBENCH_NARGS_CASE, PGBENCH_CASE
	},
	{
		"hash", PGBENCH_NARGS_HASH, PGBENCH_HASH_MURMUR2
	},
	{
		"hash_murmur2", PGBENCH_NARGS_HASH, PGBENCH_HASH_MURMUR2
	},
	{
		"hash_fnv1a", PGBENCH_NARGS_HASH, PGBENCH_HASH_FNV1A
	},
	{
		"permute", PGBENCH_NARGS_PERMUTE, PGBENCH_PERMUTE
	},
	/* keep as last array element */
	{
		NULL, 0, 0
	}
};

/*
 * Find a function from its name
 *
 * return the index of the function from the PGBENCH_FUNCTIONS array
 * or fail if the function is unknown.
 */
static int
find_func(yyscan_t yyscanner, const char *fname)
{
	int			i = 0;

	while (PGBENCH_FUNCTIONS[i].fname)
	{
		if (pg_strcasecmp(fname, PGBENCH_FUNCTIONS[i].fname) == 0)
			return i;
		i++;
	}

	expr_yyerror_more(yyscanner, "unexpected function name", fname);

	/* not reached */
	return -1;
}

/* Expression linked list builder */
static PgBenchExprList *
make_elist(PgBenchExpr *expr, PgBenchExprList *list)
{
	PgBenchExprLink *cons;

	if (list == NULL)
	{
		list = pg_malloc(sizeof(PgBenchExprList));
		list->head = NULL;
		list->tail = NULL;
	}

	cons = pg_malloc(sizeof(PgBenchExprLink));
	cons->expr = expr;
	cons->next = NULL;

	if (list->head == NULL)
		list->head = cons;
	else
		list->tail->next = cons;

	list->tail = cons;

	return list;
}

/* Return the length of an expression list */
static int
elist_length(PgBenchExprList *list)
{
	PgBenchExprLink *link = list != NULL ? list->head : NULL;
	int			len = 0;

	for (; link != NULL; link = link->next)
		len++;

	return len;
}

/* Build function call expression */
static PgBenchExpr *
make_func(yyscan_t yyscanner, int fnumber, PgBenchExprList *args)
{
	int len = elist_length(args);

	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	Assert(fnumber >= 0);

	/* validate arguments number including few special cases */
	switch (PGBENCH_FUNCTIONS[fnumber].nargs)
	{
		/* check at least one arg for least & greatest */
		case PGBENCH_NARGS_VARIABLE:
			if (len == 0)
				expr_yyerror_more(yyscanner, "at least one argument expected",
								  PGBENCH_FUNCTIONS[fnumber].fname);
			break;

		/* case (when ... then ...)+ (else ...)? end */
		case PGBENCH_NARGS_CASE:
			/* 'else' branch is always present, but could be a NULL-constant */
			if (len < 3 || len % 2 != 1)
				expr_yyerror_more(yyscanner,
								  "odd and >= 3 number of arguments expected",
								  "case control structure");
			break;

		/* hash functions with optional seed argument */
		case PGBENCH_NARGS_HASH:
			if (len < 1 || len > 2)
				expr_yyerror_more(yyscanner, "unexpected number of arguments",
								  PGBENCH_FUNCTIONS[fnumber].fname);

			if (len == 1)
			{
				PgBenchExpr *var = make_variable("default_seed");
				args = make_elist(var, args);
			}
			break;

		/* pseudorandom permutation function with optional seed argument */
		case PGBENCH_NARGS_PERMUTE:
			if (len < 2 || len > 3)
				expr_yyerror_more(yyscanner, "unexpected number of arguments",
								  PGBENCH_FUNCTIONS[fnumber].fname);

			if (len == 2)
			{
				PgBenchExpr *var = make_variable("default_seed");
				args = make_elist(var, args);
			}
			break;

		/* common case: positive arguments number */
		default:
			Assert(PGBENCH_FUNCTIONS[fnumber].nargs >= 0);

			if (PGBENCH_FUNCTIONS[fnumber].nargs != len)
				expr_yyerror_more(yyscanner, "unexpected number of arguments",
								  PGBENCH_FUNCTIONS[fnumber].fname);
	}

	expr->etype = ENODE_FUNCTION;
	expr->u.function.function = PGBENCH_FUNCTIONS[fnumber].tag;

	/* only the link is used, the head/tail is not useful anymore */
	expr->u.function.args = args != NULL ? args->head : NULL;
	if (args)
		pg_free(args);

	return expr;
}

static PgBenchExpr *
make_case(yyscan_t yyscanner, PgBenchExprList *when_then_list, PgBenchExpr *else_part)
{
	return make_func(yyscanner,
					 find_func(yyscanner, "!case_end"),
					 make_elist(else_part, when_then_list));
}
