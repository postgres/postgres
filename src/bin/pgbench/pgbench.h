/*-------------------------------------------------------------------------
 *
 * pgbench.h
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_H
#define PGBENCH_H

#include "psqlscan.h"

/*
 * This file is included outside exprscan.l, in places where we can't see
 * flex's definition of typedef yyscan_t.  Fortunately, it's documented as
 * being "void *", so we can use a macro to keep the function declarations
 * here looking like the definitions in exprscan.l.  exprparse.y also
 * uses this to be able to declare things as "yyscan_t".
 */
#define yyscan_t  void *

/* Types of expression nodes */
typedef enum PgBenchExprType
{
	ENODE_INTEGER_CONSTANT,
	ENODE_VARIABLE,
	ENODE_FUNCTION
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
	PGBENCH_MIN,
	PGBENCH_MAX,
} PgBenchFunction;

typedef struct PgBenchExpr PgBenchExpr;
typedef struct PgBenchExprLink PgBenchExprLink;
typedef struct PgBenchExprList PgBenchExprList;

struct PgBenchExpr
{
	PgBenchExprType etype;
	union
	{
		struct
		{
			int64		ival;
		}			integer_constant;
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

extern PgBenchExpr *expr_parse_result;

extern int	expr_yyparse(yyscan_t yyscanner);
extern int	expr_yylex(yyscan_t yyscanner);
extern void expr_yyerror(yyscan_t yyscanner, const char *str) pg_attribute_noreturn();
extern void expr_yyerror_more(yyscan_t yyscanner, const char *str,
				  const char *more) pg_attribute_noreturn();
extern bool expr_lex_one_word(PsqlScanState state, PQExpBuffer word_buf,
				  int *offset);
extern yyscan_t expr_scanner_init(PsqlScanState state,
				  const char *source, int lineno, int start_offset,
				  const char *command);
extern void expr_scanner_finish(yyscan_t yyscanner);
extern int	expr_scanner_offset(PsqlScanState state);
extern char *expr_scanner_get_substring(PsqlScanState state,
						   int start_offset, int end_offset);
extern int	expr_scanner_get_lineno(PsqlScanState state, int offset);

extern void syntax_error(const char *source, int lineno, const char *line,
			 const char *cmd, const char *msg,
			 const char *more, int col) pg_attribute_noreturn();

extern int64 strtoint64(const char *str);

#endif   /* PGBENCH_H */
