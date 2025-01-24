/*-------------------------------------------------------------------------
 *
 * pgbench.h
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_H
#define PGBENCH_H

#include "fe_utils/psqlscan.h"

/*
 * This file is included outside exprscan.l, in places where we can't see
 * flex's definition of typedef yyscan_t.  Fortunately, it's documented as
 * being "void *", so we can use a macro to keep the function declarations
 * here looking like the definitions in exprscan.l.  exprparse.y and
 * pgbench.c also use this to be able to declare things as "yyscan_t".
 */
#define yyscan_t  void *

/*
 * Likewise, we can't see exprparse.y's definition of union YYSTYPE here,
 * but for now there's no need to know what the union contents are.
 */
union YYSTYPE;

/*
 * Variable types used in parser.
 */
typedef enum
{
	PGBT_NO_VALUE = 0,
	PGBT_NULL,
	PGBT_INT,
	PGBT_DOUBLE,
	PGBT_BOOLEAN,
	/* add other types here */
} PgBenchValueType;

typedef struct
{
	PgBenchValueType type;
	union
	{
		int64		ival;
		double		dval;
		bool		bval;
		/* add other types here */
	}			u;
} PgBenchValue;

/* Types of expression nodes */
typedef enum PgBenchExprType
{
	ENODE_CONSTANT,
	ENODE_VARIABLE,
	ENODE_FUNCTION,
} PgBenchExprType;

/* List of operators and callable functions */
typedef enum PgBenchFunction
{
	PGBENCH_ADD,
	PGBENCH_SUB,
	PGBENCH_MUL,
	PGBENCH_DIV,
	PGBENCH_MOD,
	PGBENCH_DEBUG,
	PGBENCH_ABS,
	PGBENCH_LEAST,
	PGBENCH_GREATEST,
	PGBENCH_INT,
	PGBENCH_DOUBLE,
	PGBENCH_PI,
	PGBENCH_SQRT,
	PGBENCH_LN,
	PGBENCH_EXP,
	PGBENCH_RANDOM,
	PGBENCH_RANDOM_GAUSSIAN,
	PGBENCH_RANDOM_EXPONENTIAL,
	PGBENCH_RANDOM_ZIPFIAN,
	PGBENCH_POW,
	PGBENCH_AND,
	PGBENCH_OR,
	PGBENCH_NOT,
	PGBENCH_BITAND,
	PGBENCH_BITOR,
	PGBENCH_BITXOR,
	PGBENCH_LSHIFT,
	PGBENCH_RSHIFT,
	PGBENCH_EQ,
	PGBENCH_NE,
	PGBENCH_LE,
	PGBENCH_LT,
	PGBENCH_IS,
	PGBENCH_CASE,
	PGBENCH_HASH_FNV1A,
	PGBENCH_HASH_MURMUR2,
	PGBENCH_PERMUTE,
} PgBenchFunction;

typedef struct PgBenchExpr PgBenchExpr;
typedef struct PgBenchExprLink PgBenchExprLink;
typedef struct PgBenchExprList PgBenchExprList;

struct PgBenchExpr
{
	PgBenchExprType etype;
	union
	{
		PgBenchValue constant;
		struct
		{
			char	   *varname;
		}			variable;
		struct
		{
			PgBenchFunction function;
			PgBenchExprLink *args;
		}			function;
	}			u;
};

/* List of expression nodes */
struct PgBenchExprLink
{
	PgBenchExpr *expr;
	PgBenchExprLink *next;
};

struct PgBenchExprList
{
	PgBenchExprLink *head;
	PgBenchExprLink *tail;
};

extern int	expr_yyparse(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner);
extern int	expr_yylex(union YYSTYPE *yylval_param, yyscan_t yyscanner);
extern void expr_yyerror(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner, const char *message) pg_attribute_noreturn();
extern void expr_yyerror_more(yyscan_t yyscanner, const char *message,
							  const char *more) pg_attribute_noreturn();
extern bool expr_lex_one_word(PsqlScanState state, PQExpBuffer word_buf,
							  int *offset);
extern yyscan_t expr_scanner_init(PsqlScanState state,
								  const char *source, int lineno, int start_offset,
								  const char *command);
extern void expr_scanner_finish(yyscan_t yyscanner);
extern int	expr_scanner_offset(PsqlScanState state);
extern char *expr_scanner_get_substring(PsqlScanState state,
										int start_offset, int end_offset,
										bool chomp);
extern int	expr_scanner_get_lineno(PsqlScanState state, int offset);

extern void syntax_error(const char *source, int lineno, const char *line,
						 const char *command, const char *msg,
						 const char *more, int column) pg_attribute_noreturn();

extern bool strtoint64(const char *str, bool errorOK, int64 *result);
extern bool strtodouble(const char *str, bool errorOK, double *dv);

#endif							/* PGBENCH_H */
