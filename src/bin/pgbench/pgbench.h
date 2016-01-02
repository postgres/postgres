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

typedef enum PgBenchExprType
{
	ENODE_INTEGER_CONSTANT,
	ENODE_VARIABLE,
	ENODE_OPERATOR
} PgBenchExprType;

typedef struct PgBenchExpr PgBenchExpr;

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
			char		operator;
			PgBenchExpr *lexpr;
			PgBenchExpr *rexpr;
		}			operator;
	}			u;
};

extern PgBenchExpr *expr_parse_result;

extern int	expr_yyparse(void);
extern int	expr_yylex(void);
extern void expr_yyerror(const char *str);
extern void expr_scanner_init(const char *str, const char *source,
				  const int lineno, const char *line,
				  const char *cmd, const int ecol);
extern void syntax_error(const char *source, const int lineno, const char *line,
			 const char *cmd, const char *msg, const char *more,
			 const int col);
extern void expr_scanner_finish(void);

extern int64 strtoint64(const char *str);

#endif   /* PGBENCH_H */
