/*-------------------------------------------------------------------------
 *
 * explain_state.h
 *	  prototypes for explain_state.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain_state.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_STATE_H
#define EXPLAIN_STATE_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_node.h"

typedef enum ExplainSerializeOption
{
	EXPLAIN_SERIALIZE_NONE,
	EXPLAIN_SERIALIZE_TEXT,
	EXPLAIN_SERIALIZE_BINARY,
} ExplainSerializeOption;

typedef enum ExplainFormat
{
	EXPLAIN_FORMAT_TEXT,
	EXPLAIN_FORMAT_XML,
	EXPLAIN_FORMAT_JSON,
	EXPLAIN_FORMAT_YAML,
} ExplainFormat;

typedef struct ExplainWorkersState
{
	int			num_workers;	/* # of worker processes the plan used */
	bool	   *worker_inited;	/* per-worker state-initialized flags */
	StringInfoData *worker_str; /* per-worker transient output buffers */
	int		   *worker_state_save;	/* per-worker grouping state save areas */
	StringInfo	prev_str;		/* saved output buffer while redirecting */
} ExplainWorkersState;

typedef struct ExplainState
{
	StringInfo	str;			/* output buffer */
	/* options */
	bool		verbose;		/* be verbose */
	bool		analyze;		/* print actual times */
	bool		costs;			/* print estimated costs */
	bool		buffers;		/* print buffer usage */
	bool		wal;			/* print WAL usage */
	bool		timing;			/* print detailed node timing */
	bool		summary;		/* print total planning and execution timing */
	bool		memory;			/* print planner's memory usage information */
	bool		settings;		/* print modified settings */
	bool		generic;		/* generate a generic plan */
	ExplainSerializeOption serialize;	/* serialize the query's output? */
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
	bool		hide_workers;	/* set if we find an invisible Gather */
	int			rtable_size;	/* length of rtable excluding the RTE_GROUP
								 * entry */
	/* state related to the current plan node */
	ExplainWorkersState *workers_state; /* needed if parallel plan */
	/* extensions */
	void	  **extension_state;
	int			extension_state_allocated;
} ExplainState;

typedef void (*ExplainOptionHandler) (ExplainState *, DefElem *, ParseState *);

/* Hook to perform additional EXPLAIN options validation */
typedef void (*explain_validate_options_hook_type) (struct ExplainState *es, List *options,
													ParseState *pstate);
extern PGDLLIMPORT explain_validate_options_hook_type explain_validate_options_hook;

extern ExplainState *NewExplainState(void);
extern void ParseExplainOptionList(ExplainState *es, List *options,
								   ParseState *pstate);

extern int	GetExplainExtensionId(const char *extension_name);
extern void *GetExplainExtensionState(ExplainState *es, int extension_id);
extern void SetExplainExtensionState(ExplainState *es, int extension_id,
									 void *opaque);

extern void RegisterExtensionExplainOption(const char *option_name,
										   ExplainOptionHandler handler);
extern bool ApplyExtensionExplainOption(ExplainState *es, DefElem *opt,
										ParseState *pstate);

#endif							/* EXPLAIN_STATE_H */
