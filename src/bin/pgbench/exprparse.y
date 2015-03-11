%{
/*-------------------------------------------------------------------------
 *
 * exprparse.y
 *	  bison grammar for a simple expression syntax
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "pgbench.h"

PgBenchExpr *expr_parse_result;

static PgBenchExpr *make_integer_constant(int64 ival);
static PgBenchExpr *make_variable(char *varname);
static PgBenchExpr *make_op(char operator, PgBenchExpr *lexpr,
		PgBenchExpr *rexpr);

%}

%expect 0
%name-prefix="expr_yy"

%union
{
	int64		ival;
	char	   *str;
	PgBenchExpr *expr;
}

%type <expr> expr
%type <ival> INTEGER
%type <str> VARIABLE

%token INTEGER VARIABLE
%token CHAR_ERROR /* never used, will raise a syntax error */

/* Precedence: lowest to highest */
%left	'+' '-'
%left	'*' '/' '%'
%right	UMINUS

%%

result: expr				{ expr_parse_result = $1; }

expr: '(' expr ')'			{ $$ = $2; }
	| '+' expr %prec UMINUS	{ $$ = $2; }
	| '-' expr %prec UMINUS	{ $$ = make_op('-', make_integer_constant(0), $2); }
	| expr '+' expr			{ $$ = make_op('+', $1, $3); }
	| expr '-' expr			{ $$ = make_op('-', $1, $3); }
	| expr '*' expr			{ $$ = make_op('*', $1, $3); }
	| expr '/' expr			{ $$ = make_op('/', $1, $3); }
	| expr '%' expr			{ $$ = make_op('%', $1, $3); }
	| INTEGER				{ $$ = make_integer_constant($1); }
	| VARIABLE 				{ $$ = make_variable($1); }
	;

%%

static PgBenchExpr *
make_integer_constant(int64 ival)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_INTEGER_CONSTANT;
	expr->u.integer_constant.ival = ival;
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

static PgBenchExpr *
make_op(char operator, PgBenchExpr *lexpr, PgBenchExpr *rexpr)
{
	PgBenchExpr *expr = pg_malloc(sizeof(PgBenchExpr));

	expr->etype = ENODE_OPERATOR;
	expr->u.operator.operator = operator;
	expr->u.operator.lexpr = lexpr;
	expr->u.operator.rexpr = rexpr;
	return expr;
}

#include "exprscan.c"
