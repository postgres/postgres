/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/tcopprot.h,v 1.65 2004/04/11 00:54:45 momjian Exp $
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

#include <setjmp.h>

#include "executor/execdesc.h"
#include "tcop/dest.h"
#include "utils/guc.h"


extern DLLIMPORT sigjmp_buf Warn_restart;
extern bool Warn_restart_ready;
extern bool InError;
extern CommandDest whereToSendOutput;
extern bool log_hostname;
extern DLLIMPORT const char *debug_query_string;
extern char *rendezvous_name;
extern int	max_stack_depth;
extern bool in_fatal_exit;

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
extern Plan *pg_plan_query(Query *querytree);
extern List *pg_plan_queries(List *querytrees, bool needSnapshot);

extern bool assign_max_stack_depth(int newval, bool doit, GucSource source);

#endif   /* BOOTSTRAP_INCLUDE */

extern void die(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS);
extern void authdie(SIGNAL_ARGS);
extern int	PostgresMain(int argc, char *argv[], const char *username);
extern void ResetUsage(void);
extern void ShowUsage(const char *title);

#endif   /* TCOPPROT_H */
