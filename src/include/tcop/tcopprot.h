/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/tcopprot.h,v 1.78 2005/10/15 02:49:46 momjian Exp $
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
#include "nodes/params.h"
#include "tcop/dest.h"
#include "utils/guc.h"


extern CommandDest whereToSendOutput;
extern DLLIMPORT const char *debug_query_string;
extern int	max_stack_depth;

/* GUC-configurable parameters */

typedef enum
{
	LOGSTMT_NONE,				/* log no statements */
	LOGSTMT_DDL,				/* log data definition statements */
	LOGSTMT_MOD,				/* log modification statements, plus DDL */
	LOGSTMT_ALL					/* log all statements */
} LogStmtLevel;

extern LogStmtLevel log_statement;

#ifndef BOOTSTRAP_INCLUDE

extern List *pg_parse_and_rewrite(const char *query_string,
					 Oid *paramTypes, int numParams);
extern List *pg_parse_query(const char *query_string);
extern List *pg_analyze_and_rewrite(Node *parsetree,
					   Oid *paramTypes, int numParams);
extern Plan *pg_plan_query(Query *querytree, ParamListInfo boundParams);
extern List *pg_plan_queries(List *querytrees, ParamListInfo boundParams,
				bool needSnapshot);

extern bool assign_max_stack_depth(int newval, bool doit, GucSource source);
#endif   /* BOOTSTRAP_INCLUDE */

extern void die(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS);
extern void authdie(SIGNAL_ARGS);
extern void StatementCancelHandler(SIGNAL_ARGS);
extern void FloatExceptionHandler(SIGNAL_ARGS);
extern void prepare_for_client_read(void);
extern void client_read_ended(void);
extern int	PostgresMain(int argc, char *argv[], const char *username);
extern void ResetUsage(void);
extern void ShowUsage(const char *title);
extern void set_debug_options(int debug_flag,
				  GucContext context, GucSource source);

#endif   /* TCOPPROT_H */
