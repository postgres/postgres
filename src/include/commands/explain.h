/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "parser/parse_node.h"

typedef enum ExplainFormat
{
	EXPLAIN_FORMAT_TEXT,
	EXPLAIN_FORMAT_XML,
	EXPLAIN_FORMAT_JSON,
	EXPLAIN_FORMAT_YAML
} ExplainFormat;

typedef struct ExplainState
{
	StringInfo	str;			/* output buffer */
	/* options */
	bool		verbose;		/* be verbose */
	bool		analyze;		/* print actual times */
	bool		costs;			/* print estimated costs */
	bool		buffers;		/* print buffer usage */
	bool		timing;			/* print detailed node timing */
	bool		summary;		/* print total planning and execution timing */
	bool		settings;		/* print modified settings */
	ExplainFormat format;		/* output format */
	/* state for output formatting --- not reset for each new plan tree */
	int			indent;			/* current indentation level */
	List	   *grouping_stack; /* format-specific grouping state */
	/* state related to the current plan tree (filled by ExplainPrintPlan) */
	PlannedStmt *pstmt;			/* top of plan */
	List	   *rtable;			/* range table */
	List	   *rtable_names;	/* alias names for RTEs */
	List	   *deparse_cxt;	/* context list for deparsing expressions */
	Bitmapset  *printed_subplans;	/* ids of SubPlans we've printed */
} ExplainState;

/* Hook for plugins to get control in ExplainOneQuery() */
typedef void (*ExplainOneQuery_hook_type) (Query *query,
										   int cursorOptions,
										   IntoClause *into,
										   ExplainState *es,
										   const char *queryString,
										   ParamListInfo params,
										   QueryEnvironment *queryEnv);
extern PGDLLIMPORT ExplainOneQuery_hook_type ExplainOneQuery_hook;

/* Hook for plugins to get control in explain_get_index_name() */
typedef const char *(*explain_get_index_name_hook_type) (Oid indexId);
extern PGDLLIMPORT explain_get_index_name_hook_type explain_get_index_name_hook;


extern void ExplainQuery(ParseState *pstate, ExplainStmt *stmt, const char *queryString,
						 ParamListInfo params, QueryEnvironment *queryEnv, DestReceiver *dest);

extern ExplainState *NewExplainState(void);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOneUtility(Node *utilityStmt, IntoClause *into,
							  ExplainState *es, const char *queryString,
							  ParamListInfo params, QueryEnvironment *queryEnv);

extern void ExplainOnePlan(PlannedStmt *plannedstmt, IntoClause *into,
						   ExplainState *es, const char *queryString,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   const instr_time *planduration);

extern void ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc);
extern void ExplainPrintTriggers(ExplainState *es, QueryDesc *queryDesc);

extern void ExplainPrintJITSummary(ExplainState *es, QueryDesc *queryDesc);
extern void ExplainPrintJIT(ExplainState *es, int jit_flags,
							struct JitInstrumentation *jit_instr, int worker_i);

extern void ExplainQueryText(ExplainState *es, QueryDesc *queryDesc);

extern void ExplainBeginOutput(ExplainState *es);
extern void ExplainEndOutput(ExplainState *es);
extern void ExplainSeparatePlans(ExplainState *es);

extern void ExplainPropertyList(const char *qlabel, List *data,
								ExplainState *es);
extern void ExplainPropertyListNested(const char *qlabel, List *data,
									  ExplainState *es);
extern void ExplainPropertyText(const char *qlabel, const char *value,
								ExplainState *es);
extern void ExplainPropertyInteger(const char *qlabel, const char *unit,
								   int64 value, ExplainState *es);
extern void ExplainPropertyFloat(const char *qlabel, const char *unit,
								 double value, int ndigits, ExplainState *es);
extern void ExplainPropertyBool(const char *qlabel, bool value,
								ExplainState *es);

extern void ExplainOpenGroup(const char *objtype, const char *labelname,
							 bool labeled, ExplainState *es);
extern void ExplainCloseGroup(const char *objtype, const char *labelname,
							  bool labeled, ExplainState *es);

#endif							/* EXPLAIN_H */
