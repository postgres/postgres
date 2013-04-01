/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/tcopprot.h,v 1.104 2010/02/26 02:01:28 momjian Exp $
 *
 * OLD COMMENTS
 *	  This file was created so that other c files could get the two
 *	  function prototypes without having to include tcop.h which single
 *	  handedly includes the whole f*cking tree -- mer 5 Nov. 1991
 *
 *-------------------------------------------------------------------------
 */
#ifndef TCOPPROT_H
#define TCOPPROT_H

#include "executor/execdesc.h"
#include "nodes/parsenodes.h"
#include "storage/procsignal.h"
#include "utils/guc.h"


/* Required daylight between max_stack_depth and the kernel limit, in bytes */
#define STACK_DEPTH_SLOP (512 * 1024L)

extern CommandDest whereToSendOutput;
extern PGDLLIMPORT const char *debug_query_string;
extern int	max_stack_depth;
extern int	PostAuthDelay;

/* GUC-configurable parameters */

typedef enum
{
	LOGSTMT_NONE,				/* log no statements */
	LOGSTMT_DDL,				/* log data definition statements */
	LOGSTMT_MOD,				/* log modification statements, plus DDL */
	LOGSTMT_ALL					/* log all statements */
} LogStmtLevel;

extern int	log_statement;

extern List *pg_parse_and_rewrite(const char *query_string,
					 Oid *paramTypes, int numParams);
extern List *pg_parse_query(const char *query_string);
extern List *pg_analyze_and_rewrite(Node *parsetree, const char *query_string,
					   Oid *paramTypes, int numParams);
extern List *pg_analyze_and_rewrite_params(Node *parsetree,
							  const char *query_string,
							  ParserSetupHook parserSetup,
							  void *parserSetupArg);
extern PlannedStmt *pg_plan_query(Query *querytree, int cursorOptions,
			  ParamListInfo boundParams);
extern List *pg_plan_queries(List *querytrees, int cursorOptions,
				ParamListInfo boundParams);

extern bool assign_max_stack_depth(int newval, bool doit, GucSource source);

extern void die(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS);
extern void StatementCancelHandler(SIGNAL_ARGS);
extern void FloatExceptionHandler(SIGNAL_ARGS);
extern void RecoveryConflictInterrupt(ProcSignalReason reason); /* called from SIGUSR1
																 * handler */
extern void prepare_for_client_read(void);
extern void client_read_ended(void);
extern void process_postgres_switches(int argc, char *argv[],
						  GucContext ctx, const char **dbname);
extern int	PostgresMain(int argc, char *argv[],
						 const char *dbname, const char *username);
extern long get_stack_depth_rlimit(void);
extern void ResetUsage(void);
extern void ShowUsage(const char *title);
extern int	check_log_duration(char *msec_str, bool was_logged);
extern void set_debug_options(int debug_flag,
				  GucContext context, GucSource source);
extern bool set_plan_disabling_options(const char *arg,
						   GucContext context, GucSource source);
extern const char *get_stats_option_name(const char *arg);

#endif   /* TCOPPROT_H */
