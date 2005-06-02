/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tcopprot.h,v 1.60.4.1 2005/06/02 21:04:08 tgl Exp $
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


extern DLLIMPORT sigjmp_buf Warn_restart;
extern bool Warn_restart_ready;
extern bool InError;
extern CommandDest whereToSendOutput;
extern bool log_hostname;
extern bool LogSourcePort;
extern DLLIMPORT const char *debug_query_string;
extern char *rendezvous_name;

#ifndef BOOTSTRAP_INCLUDE

extern List *pg_parse_and_rewrite(const char *query_string,
					 Oid *paramTypes, int numParams);
extern List *pg_parse_query(const char *query_string);
extern List *pg_analyze_and_rewrite(Node *parsetree,
					   Oid *paramTypes, int numParams);
extern List *pg_rewrite_queries(List *querytree_list);
extern Plan *pg_plan_query(Query *querytree);
extern List *pg_plan_queries(List *querytrees, bool needSnapshot);
#endif   /* BOOTSTRAP_INCLUDE */

extern void die(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS);
extern void authdie(SIGNAL_ARGS);
extern void prepare_for_client_read(void);
extern void client_read_ended(void);
extern int	PostgresMain(int argc, char *argv[], const char *username);
extern void ResetUsage(void);
extern void ShowUsage(const char *title);

#endif   /* TCOPPROT_H */
