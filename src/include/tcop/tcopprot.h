/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/tcopprot.h,v 1.71 2004/08/29 05:06:58 momjian Exp $
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
	/* Reverse order so GUC USERLIMIT is easier */
	LOGSTMT_ALL,				/* log all statements */
	LOGSTMT_DDL,				/* log data definition statements */
	LOGSTMT_MOD,				/* log modification statements, plus DDL */
	LOGSTMT_NONE				/* log no statements */
} LogStmtLevel;

extern LogStmtLevel log_statement;

#ifndef BOOTSTRAP_INCLUDE

extern List *pg_parse_and_rewrite(const char *query_string,
					 Oid *paramTypes, int numParams);
extern List *pg_parse_query(const char *query_string);
extern List *pg_analyze_and_rewrite(Node *parsetree,
					   Oid *paramTypes, int numParams);
extern List *pg_rewrite_queries(List *querytree_list);
extern Plan *pg_plan_query(Query *querytree, ParamListInfo boundParams);
extern List *pg_plan_queries(List *querytrees, ParamListInfo boundParams,
				bool needSnapshot);

extern bool assign_max_stack_depth(int newval, bool doit, GucSource source);
#endif   /* BOOTSTRAP_INCLUDE */

extern void die(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS);
extern void authdie(SIGNAL_ARGS);
extern int	PostgresMain(int argc, char *argv[], const char *username);
extern void ResetUsage(void);
extern void ShowUsage(const char *title);

#endif   /* TCOPPROT_H */
