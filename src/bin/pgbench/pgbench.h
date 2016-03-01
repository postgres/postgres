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

extern int	expr_yyparse(void);
extern int	expr_yylex(void);
extern void expr_yyerror(const char *str);
extern void expr_yyerror_more(const char *str, const char *more);
extern void expr_scanner_init(const char *str, const char *source,
				  const int lineno, const char *line,
				  const char *cmd, const int ecol);
extern void syntax_error(const char *source, const int lineno, const char *line,
			 const char *cmd, const char *msg, const char *more,
			 const int col);
extern void expr_scanner_finish(void);

extern int64 strtoint64(const char *str);

#endif   /* PGBENCH_H */
